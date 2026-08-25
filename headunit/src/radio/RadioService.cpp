//
// RadioService implementation
// Non-blocking Si4703 FM/RDS radio control using PU2CLR SI470X library
//

#include <Arduino.h>
#include <Wire.h>
#include "radio/RadioService.h"
#include "hardware/Pins.h"

namespace {
constexpr uint8_t SI470X_ADDR = 0x10;

// RAII Mutex guard for RadioService thread-safe critical sections
class MutexGuard {
public:
     explicit MutexGuard(SemaphoreHandle_t mutex, uint32_t timeout_ms = 100)
         : _mutex(mutex) {
         if (_mutex) {
             _acquired = xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
         }
     }
     ~MutexGuard() {
         if (_mutex && _acquired) {
             xSemaphoreGive(_mutex);
         }
     }
     bool acquired() const { return _acquired; }
private:
     SemaphoreHandle_t _mutex;
     bool _acquired = false;
};
}

RadioService::RadioService() {
     _status.frequency = app_config::kMinFrequency10kHz;
     _status.volume = 8;
     // Mutex created in begin() to avoid issues with static initialization
 }

 RadioService::~RadioService() {
     if (_mutex) {
         vSemaphoreDelete(_mutex);
     }
 }

 bool RadioService::begin() {
     // Create the mutex for thread-safe access
     _mutex = xSemaphoreCreateMutex();
     if (!_mutex) {
         Serial.println(F("[RADIO] Mutex creation failed"));
         return false;
     }

     Wire.begin(pins::I2C_SDA_PIN, pins::I2C_SCL_PIN);
     Wire.setClock(400000);

    if (!_initHardware()) {
        Serial.println(F("[RADIO] Hardware init failed"));
        return false;
    }

    _state = RadioState::Idle;
    _notifyStatus();
    Serial.println(F("[RADIO] Ready"));
    return true;
}

bool RadioService::_initHardware() {
    // Reset sequence
    if (pins::RADIO_RST_PIN >= 0) {
        pinMode(pins::RADIO_RST_PIN, OUTPUT);
        digitalWrite(pins::RADIO_RST_PIN, LOW);
        delay(10);
        digitalWrite(pins::RADIO_RST_PIN, HIGH);
        delay(10);
    }

    // Initialize SI470X - setup(resetPin, sdaPin, rdsInterruptPin, seekInterruptPin, oscillatorType)
    _si470x.setup(pins::RADIO_RST_PIN, pins::I2C_SDA_PIN, -1, -1, OSCILLATOR_TYPE_CRYSTAL);

    // Check if device is responding
    _si470x.getStatus();
    uint8_t partNumber = _si470x.getPartNumber();
    if (partNumber != 0x03 && partNumber != 0x01) {  // 3=Si4703, 1=Si4702
        Serial.printf("[RADIO] Unexpected part number: 0x%02X\n", partNumber);
        // Continue anyway
    }

    // Configure for Europe: 87.5-108 MHz (band 0), 50us de-emphasis.
    _si470x.setBand(0);        // FM_BAND_USA_EU = 87.5-108 MHz
    // Deliberately NOT calling _si470x.setSpace(2) here: the PU2CLR SI470X
    // library (as of its current release/master) has a bug where
    // setSpace() writes its argument into the wrong internal field -
    //   void SI470X::setSpace(uint8_t space) {
    //     this->currentFMBand = reg05->refined.SPACE = space;  // should be currentFMSpace
    //   }
    // - so calling it after setBand(0) silently corrupts the library's
    // internal band back to FM_BAND_JAPAN (76-90 MHz) while its internal
    // "current spacing" cache is *not* updated to match the hardware
    // register it *did* write correctly. The two stay out of sync, and
    // every subsequent setFrequency()/getRealFrequency() channel<->MHz
    // conversion is computed against the wrong band/spacing pair, tuning
    // to a frequency quite far from the one actually requested. Skipping
    // setSpace() keeps the library's default channel spacing (100 kHz,
    // set by its own powerUp()), which stays internally consistent and is
    // a perfectly standard raster for European FM.
    _si470x.setFmDeemphasis(1); // 50 us (Europe)

    // RDS enable
    _si470x.setRds(true);

    // Initial volume
    _applyVolume();

    // Tune to default frequency. The PU2CLR library's setFrequency()/
    // getFrequency()/getRealFrequency() all use the exact same 10 kHz-step
    // representation as our own RadioStatus.frequency (e.g. 8750 = 87.50
    // MHz - see startBand[]/endBand[] in SI470X.h and the setFrequency()
    // docstring "send 10650 for 106.5 MHz"), so no unit conversion is
    // needed or correct here. A previous "/ 10" here (based on an incorrect
    // "library uses 0.1 MHz steps" assumption) caused an unsigned
    // underflow inside the library's channel calculation, silently tuning
    // to a wrapped/garbage channel instead of the requested frequency.
    _si470x.setFrequency(_status.frequency);
    delay(100);
    _updateStatusFromHardware();

    return true;
}

void RadioService::loop() {
    switch (_state) {
        case RadioState::Off:
            break;
        case RadioState::Idle:
            _stepIdle();
            break;
        case RadioState::Tuning:
            _stepTuning();
            break;
        case RadioState::Seeking:
            _stepSeeking();
            break;
        case RadioState::Scanning:
            Serial.printf("[RADIO::LOOP] Scanning state detected, calling _stepScanning\n");
            _stepScanning();
            break;
    }
}

void RadioService::_stepIdle() {
    _pollRds();
    _updateStatusFromHardware();
}

void RadioService::_stepTuning() {
    if (millis() - _lastTuneMs > TUNE_TIMEOUT_MS) {
        Serial.println(F("[RADIO] Tune timeout"));
        _state = RadioState::Idle;
        _notifyStatus();
    } else {
        _updateStatusFromHardware();
        if (_status.rssi > 0 && abs((int)_status.frequency - (int)_config.lastFrequency) < 5) {
            _state = RadioState::Idle;
            _notifyStatus();
        }
    }
}

void RadioService::_stepSeeking() {
    _si470x.getStatus();
    if (_si470x.getShadownRegister(0x0A) & 0x01) {  // STC bit in register 0x0A
        {
            // The station just found by seek hasn't had its RDS text
            // decoded yet - clear the previous station's stale text (see
            // the same reasoning in setFrequency()).
            MutexGuard guard(_mutex);
            if (guard.acquired()) {
                _status.programService[0] = '\0';
                _status.radioText[0] = '\0';
            }
        }
        _updateStatusFromHardware();
        _state = RadioState::Idle;
        _notifyStatus();
        _notifyStationSelected(_status.frequency, "");
    }
}

void RadioService::_stepScanning() {
    Serial.printf("[RADIO::SCAN] _stepScanning called, phase=%d\n", (int)_scan.phase);
    _stepScan();
}

void RadioService::_stepScan() {
    switch (_scan.phase) {
        case ScanState::Idle:
            Serial.println(F("[RADIO::SCAN] Phase: Idle -> SeekStart"));
            _scan.phase = ScanState::SeekStart;
            _scan.currentFreq = app_config::kMinFrequency10kHz;
            _scan.stationCount = 0;
            _scan.lastSeekMs = millis();
            Serial.printf("[RADIO::SCAN]   Starting from freq=%u kHz\n", _scan.currentFreq);
            break;

        case ScanState::SeekStart:
            Serial.printf("[RADIO::SCAN] Phase: SeekStart at freq=%u kHz\n", _scan.currentFreq);
            if (_scanSeekStart()) {
                Serial.println(F("[RADIO::SCAN]   -> SeekWait"));
                _scan.phase = ScanState::SeekWait;
                _scan.lastSeekMs = millis();
            } else {
                Serial.println(F("[RADIO::SCAN]   _scanSeekStart() returned false!"));
            }
            break;

        case ScanState::SeekWait:
            if (_scanSeekWait()) {
                Serial.printf("[RADIO::SCAN] Phase: SeekWait - FOUND station, storing...\n");
                _scan.phase = ScanState::StoreStation;
            } else if (millis() - _scan.lastSeekMs > 2000) {
                Serial.printf("[RADIO::SCAN] Phase: SeekWait - TIMEOUT after %lu ms, moving to NextSeek\n", 
                    millis() - _scan.lastSeekMs);
                _scan.phase = ScanState::NextSeek;
            }
            break;

        case ScanState::StoreStation:
            Serial.println(F("[RADIO::SCAN] Phase: StoreStation"));
            if (_scanStoreStation()) {
                Serial.printf("[RADIO::SCAN]   Stored station #%u, moving to NextSeek\n", _scan.stationCount);
                _scan.phase = ScanState::NextSeek;
            } else {
                Serial.println(F("[RADIO::SCAN]   _scanStoreStation() failed or duplicate, moving to NextSeek anyway"));
                _scan.phase = ScanState::NextSeek;
            }
            break;

        case ScanState::NextSeek:
            Serial.printf("[RADIO::SCAN] Phase: NextSeek (current freq=%u)\n", _scan.currentFreq);
            _scanNextSeek();
            break;

        case ScanState::Complete:
            Serial.printf("[RADIO::SCAN] Phase: Complete - found %u stations\n", _scan.stationCount);
            _scanComplete();
            break;
    }
}

bool RadioService::powerOn() {
    if (_state == RadioState::Off) {
        if (!_initHardware()) return false;
        _state = RadioState::Idle;
        _notifyStatus();
    }
    return true;
}

void RadioService::powerOff() {
    if (_state != RadioState::Off) {
        _si470x.setMute(true);
        _si470x.setShadownRegister(0x02, 0);  // Power down (ENABLE=0)
        _si470x.setAllRegisters();
        _state = RadioState::Off;
        _notifyStatus();
    }
}

bool RadioService::setFrequency(uint16_t frequency) {
     if (!isValidFrequency(frequency)) return false;
     if (_state == RadioState::Off) return false;

     {
         MutexGuard guard(_mutex);
         if (!guard.acquired()) return false;

         _config.lastFrequency = frequency;
         _status.frequency = frequency;
         // The previous station's RDS text no longer applies to the new
         // frequency and hasn't been decoded for this one yet - clear it so
         // status consumers (web UI, station-selected overlay) fall back to
         // showing the frequency instead of a stale/wrong station name.
         _status.programService[0] = '\0';
         _status.radioText[0] = '\0';
     }

     // See the comment in _initHardware(): setFrequency() takes the same
     // 10 kHz-step units as RadioStatus.frequency, no conversion needed.
     _si470x.setFrequency(frequency);

     _state = RadioState::Tuning;
     _lastTuneMs = millis();
     _notifyStatus();
     _notifyStationSelected(frequency, "");
     return true;
}

bool RadioService::seekUp() {
     if (_state != RadioState::Idle && _state != RadioState::Seeking) return false;
     _state = RadioState::Seeking;
     _si470x.seek(0, 1);  // seekMode=0 (wrap), direction=1 (up)
     _notifyStatus();
     return true;
}

bool RadioService::seekDown() {
     if (_state != RadioState::Idle && _state != RadioState::Seeking) return false;
     _state = RadioState::Seeking;
     _si470x.seek(0, 0);  // seekMode=0 (wrap), direction=0 (down)
     _notifyStatus();
     return true;
}

bool RadioService::startScan() {
    Serial.printf("[RADIO::SCAN] startScan() called, current state: %d\n", (int)_state);
    if (_state == RadioState::Scanning) {
        Serial.println(F("[RADIO::SCAN] ERROR: Scan already in progress, rejecting start request"));
        return false;
    }
    Serial.println(F("[RADIO::SCAN] Starting new scan..."));
    _state = RadioState::Scanning;
    _scan.phase = ScanState::Idle;
    _scan.currentFreq = app_config::kMinFrequency10kHz;
    _scan.stationCount = 0;
    Serial.printf("[RADIO::SCAN] State changed to Scanning, phase=Idle, freq=%u\n", _scan.currentFreq);
    _notifyScanProgress();
    return true;
}

void RadioService::cancelScan() {
    if (_state == RadioState::Scanning) {
        _state = RadioState::Idle;
        _scan.phase = ScanState::Idle;
        _notifyScanProgress();
    }
}

const RadioStation* RadioService::getStations(size_t& count) const {
    count = _scan.stationCount;
    return _scan.stations;
}

bool RadioService::setFavorite(uint16_t frequency, bool favorite) {
    bool found = false;
    {
        MutexGuard guard(_mutex);
        if (!guard.acquired()) return false;
        for (size_t i = 0; i < _scan.stationCount; i++) {
            if (_scan.stations[i].frequency == frequency) {
                _scan.stations[i].favorite = favorite;
                found = true;
                break;
            }
        }
    }
    if (found) _notifyStationList();
    return found;
}

bool RadioService::setVolume(uint8_t volume) {
     if (volume > 15) volume = 15;

     {
         MutexGuard guard(_mutex);
         if (!guard.acquired()) return false;

         _status.volume = volume;
         _config.lastVolume = volume;
     }

     _applyVolume();
     _notifyStatus();
     return true;
}

void RadioService::_applyVolume() {
    _si470x.setVolume(_status.volume);
}

bool RadioService::setMuted(bool muted) {
     {
         MutexGuard guard(_mutex);
         if (!guard.acquired()) return false;

         _status.muted = muted;
         _config.lastMuted = muted;
     }

     _si470x.setMute(muted);
     _notifyStatus();
     return true;
}

void RadioService::_updateStatusFromHardware() {
    _si470x.getStatus();

    const uint8_t rssi = _si470x.getRssi();
    const bool stereo = _si470x.isStereo();
    // getFrequency() already returns the same 10 kHz-step units as
    // RadioStatus.frequency (see the comment in _initHardware()) - no *10.
    const uint16_t frequency = _si470x.getFrequency();

    MutexGuard guard(_mutex);
    if (!guard.acquired()) return;
    _status.rssi = rssi;
    _status.stereo = stereo;
    _status.frequency = frequency;
}

void RadioService::_pollRds() {
    if (millis() - _lastRdsPollMs < RDS_POLL_INTERVAL_MS) return;

    _lastRdsPollMs = millis();

    if (_si470x.getRdsReady()) {
        bool changed = false;
        {
            MutexGuard guard(_mutex);
            if (!guard.acquired()) return;

            // Program Service (PS) - station name from RDS 0A
            char* ps = _si470x.getRdsText0A();
            if (ps && ps[0] != '\0' && strcmp(ps, _status.programService) != 0) {
                strncpy(_status.programService, ps, 8);
                _status.programService[8] = '\0';
                changed = true;
            }

            // RadioText (RT) - song info etc. from RDS 2A
            char* rt = _si470x.getRdsText2A();
            if (rt && rt[0] != '\0' && strcmp(rt, _status.radioText) != 0) {
                strncpy(_status.radioText, rt, 64);
                _status.radioText[64] = '\0';
                changed = true;
            }
        }
        if (changed) _notifyStatus();
    }
}

bool RadioService::_scanSeekStart() {
    // Start seek from current scan frequency. currentFreq is already in the
    // same 10 kHz-step units setFrequency() expects (see _initHardware()).
    Serial.printf("[RADIO::SCAN::SEEK] Starting seek at %.2f MHz\n", _scan.currentFreq / 100.0f);
    _si470x.setFrequency(_scan.currentFreq);
    delay(50);
    Serial.println(F("[RADIO::SCAN::SEEK] Calling seek(0, 1)..."));
    _si470x.seek(0, 1);  // wrap, up
    Serial.println(F("[RADIO::SCAN::SEEK] Seek started"));
    return true;
}

bool RadioService::_scanSeekWait() {
    _si470x.getStatus();
    bool stcSet = (_si470x.getShadownRegister(0x0A) & 0x01) != 0;  // STC bit
    if (stcSet) {
        // getFrequency() already returns 10 kHz-step units - no *10 (see
        // the comment in _initHardware()).
        uint16_t freq = _si470x.getFrequency();
        uint8_t rssi = _si470x.getRssi();
        bool stereo = _si470x.isStereo();
        Serial.printf("[RADIO::SCAN::SEEK] FOUND: freq=%u (%.2f MHz), RSSI=%u, stereo=%s\n", 
            freq, freq / 100.0f, rssi, stereo ? "yes" : "no");
    }
    return stcSet;
}

bool RadioService::_scanStoreStation() {
    _si470x.getStatus();
    
    // Use getRealFrequency() instead of getFrequency()!
    // getFrequency() returns cached value, getRealFrequency() reads from register.
    // Both already return 10 kHz-step units - no *10 (see _initHardware()).
    uint16_t freq = _si470x.getRealFrequency();
    
    uint8_t rssi = _si470x.getRssi();
    bool stereo = _si470x.isStereo();

    Serial.printf("[RADIO::SCAN::STORE] getRealFrequency()=%u (%.2f MHz), RSSI=%u, stereo=%s\n", 
        freq, freq / 100.0f, rssi, stereo ? "yes" : "no");

    if (!isValidFrequency(freq) || _isDuplicateFrequency(freq)) {
        Serial.printf("[RADIO::SCAN::STORE] Rejected: invalid=%s (freq=%u, range=[8750-10800]), duplicate=%s\n",
            !isValidFrequency(freq) ? "yes" : "no",
            freq,
            _isDuplicateFrequency(freq) ? "yes" : "no");
        return false;
    }

    if (_scan.stationCount < app_config::kMaxStations) {
        RadioStation& st = _scan.stations[_scan.stationCount];
        st.frequency = freq;
        st.rssi = rssi;
        st.stereo = stereo;
        st.programService[0] = '\0';
        st.radioText[0] = '\0';
        st.favorite = false;
        _scan.stationCount++;
        {
            MutexGuard guard(_mutex);
            if (guard.acquired()) _status.scanCount = _scan.stationCount;
        }
        Serial.printf("[RADIO::SCAN::STORE] ✓ STORED station #%u at %.2f MHz\n", 
            _scan.stationCount, freq / 100.0f);
        _notifyStationList();
    } else {
        Serial.println(F("[RADIO::SCAN::STORE] Buffer full (50 stations), ignoring"));
    }
    return true;
}

void RadioService::_scanNextSeek() {
    _scan.currentFreq += 5;  // 50kHz = 5 * 10kHz steps

    Serial.printf("[RADIO::SCAN::NEXSEEK] currentFreq now=%u, MAX_FREQ=%u, comparison: %u > %u = %s\n",
        _scan.currentFreq, app_config::kMaxFrequency10kHz, _scan.currentFreq, app_config::kMaxFrequency10kHz,
        (_scan.currentFreq > app_config::kMaxFrequency10kHz) ? "TRUE" : "FALSE");

    if (_scan.currentFreq > app_config::kMaxFrequency10kHz) {
        Serial.printf("[RADIO::SCAN] Reached MAX_FREQ (%u), setting Complete phase\n", app_config::kMaxFrequency10kHz);
        _scan.phase = ScanState::Complete;
    } else {
        Serial.printf("[RADIO::SCAN] Moving to next seek frequency: %.2f MHz\n", _scan.currentFreq / 100.0f);
        _scan.phase = ScanState::SeekStart;
        _scan.lastSeekMs = millis();
    }

    const uint8_t scanProgress = static_cast<uint8_t>(
        ((_scan.currentFreq - app_config::kMinFrequency10kHz) * 100) / (app_config::kMaxFrequency10kHz - app_config::kMinFrequency10kHz)
    );
    {
        MutexGuard guard(_mutex);
        if (guard.acquired()) _status.scanProgress = scanProgress;
    }
    Serial.printf("[RADIO::SCAN] Progress: %u%% (%u stations found)\n", 
        scanProgress, _scan.stationCount);
    _notifyScanProgress();
}

void RadioService::_scanComplete() {
    Serial.printf("[RADIO::SCAN] _scanComplete() called, total stations: %u\n", _scan.stationCount);
    
    // Sort stations by frequency. Guard against stationCount == 0: since
    // stationCount is unsigned, "stationCount - 1" would otherwise wrap
    // around to SIZE_MAX and walk far past the end of the stations array.
    for (size_t i = 0; i + 1 < _scan.stationCount; i++) {
        for (size_t j = i + 1; j < _scan.stationCount; j++) {
            if (_scan.stations[i].frequency > _scan.stations[j].frequency) {
                RadioStation tmp = _scan.stations[i];
                _scan.stations[i] = _scan.stations[j];
                _scan.stations[j] = tmp;
            }
        }
    }

    Serial.println(F("[RADIO::SCAN] Scan complete - stations sorted, resetting state to Idle"));
    _state = RadioState::Idle;
    _scan.phase = ScanState::Idle;
    {
        MutexGuard guard(_mutex);
        if (guard.acquired()) _status.scanProgress = 100;
    }
    Serial.printf("[RADIO::SCAN] State reset: RadioState=%d, ScanState=%d\n", (int)_state, (int)_scan.phase);
    _notifyScanProgress();
    _notifyStationList();
    _notifyStatus();
}

bool RadioService::_isDuplicateFrequency(uint16_t freq) const {
    for (size_t i = 0; i < _scan.stationCount; i++) {
        if (abs((int)_scan.stations[i].frequency - (int)freq) < 5) {
            return true;
        }
    }
    return false;
}

void RadioService::_notifyStatus() {
    // Keep the RadioState mirrored into _status right before every
    // notification/read: _state is the authoritative state machine value,
    // but _status (the struct actually exposed to WebServer/RadioScreen)
    // never had it assigned anywhere, so external consumers always saw the
    // default RadioState::Off regardless of the real state.
    {
        MutexGuard guard(_mutex);
        if (guard.acquired()) _status.state = _state;
    }
    if (_statusCallback) _statusCallback(_status);
}

RadioStatus RadioService::getStatus() const {
    MutexGuard guard(_mutex);
    return _status;
}

void RadioService::_notifyStationList() {
    if (_stationListCallback) _stationListCallback(_scan.stations, _scan.stationCount);
}

void RadioService::_notifyStationSelected(uint16_t frequency, const char* programService) {
    if (_stationSelectedCallback) _stationSelectedCallback(frequency, programService);
}

void RadioService::_notifyScanProgress() {
    if (_scanProgressCallback) _scanProgressCallback(_status.scanProgress, _status.scanCount, _state == RadioState::Scanning);
}

void RadioService::setSpeedKmh(float speedKmh) {
    _speedKmh = speedKmh;
}

RadioConfig RadioService::getConfig() const {
     MutexGuard guard(_mutex);
     return _config;
}

void RadioService::applyConfig(const RadioConfig& config) {
     {
         MutexGuard guard(_mutex);
         if (!guard.acquired()) return;

         _config = config;
         if (isValidFrequency(config.lastFrequency)) {
             _status.frequency = config.lastFrequency;
         }
         _status.volume = config.lastVolume;
         _status.muted = config.lastMuted;
     }
}