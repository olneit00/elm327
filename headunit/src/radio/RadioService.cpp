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

// Even with RDSS ("RDS Synchronized") true, the block error rate (BLERA)
// can still leave the occasional corrupted byte in decoded PS/RT text -
// visible in practice as embedded control characters (e.g. SOH 0x01, SI
// 0x0F) rendered as boxes/mojibake. Reject the whole string rather than
// display it partially garbled; RDS text is either clean ASCII/Latin-1 or
// not trustworthy at all for this purpose.
bool looksLikeValidRdsText(const char* text) {
    if (text == nullptr) return false;
    for (const char* p = text; *p != '\0'; ++p) {
        const uint8_t c = static_cast<uint8_t>(*p);
        if (c < 0x20 && c != ' ') return false;  // C0 control chars (not the space itself)
    }
    return true;
}
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

    // Re-enable AGC (Automatic Gain Control), which the PU2CLR library's own
    // powerUp() disables unconditionally:
    //   reg04->refined.AGCD = 1;  // 0 = AGC enable (default per the chip's
    //                             // own register comment), 1 = disable
    // With AGC off, the front-end gain is fixed rather than adapted to the
    // actual signal strength. On a moderate/strong station this let the
    // input stage run too hot for the much lower-amplitude 57 kHz RDS
    // subcarrier, which is far more sensitive to front-end distortion than
    // the main audio path: RSSI read fine and audio sounded normal, RDSR
    // ("RDS ready") occasionally blipped true on a lucky group, but RDSS
    // ("RDS Synchronized") never latched because the subcarrier itself was
    // too distorted to track continuously. Re-enabling AGC here restores
    // the chip's normal adaptive behavior.
    _si470x.setAgc(true);

    // RDS enable
    _si470x.setRds(true);

    // KRITISCH: RDSS (RDS Synchronized) and BLERA/B/C/D (block error rates)
    // are only meaningful in RDS "verbose" mode. The PU2CLR library's own
    // powerUp() unconditionally sets RDSM=0 (standard mode) via
    // reg02->refined.RDSM = 0, and in standard mode the chip hardware
    // forces RDSS and the BLER fields to always read 0 (Si4702/03-C19
    // datasheet Sec. 4.4 "RDS/RBDS Processor and Functionality", and the
    // Reg 0Ah bit comments for RDSS/BLERA: "Available only in RDS Verbose
    // mode (RDSM 02h[11] = 1)"). Without switching to verbose mode here,
    // _pollRds()'s "rdsReady && rdsSynced" gate can never pass - RDSS is
    // hardware-clamped to 0 - regardless of signal quality or the AGC fix.
    // RDSR itself is valid in both modes, which is why it kept blipping
    // true while RDSS never latched.
    _si470x.setRdsMode(1);  // RDS Verbose mode: enables real RDSS/BLER

    // Initial volume
    _applyVolume();

// Tune to default frequency. The PU2CLR library's setFrequency()/
    // getFrequency()/getRealFrequency() all use the exact same 10 kHz-step
    // representation as our own RadioStatus.frequency (e.g. 8750 = 87.50
    // MHz - see startBand[]/endBand[] in SI470X.h and the setFrequency()
    // doc comment "send 10650 for 106.5 MHz"), so no unit conversion is
    // needed or correct here. A previous "/ 10" here (based on an incorrect
    // "library uses 0.1 MHz steps" assumption) caused an unsigned
    // underflow inside the library's channel calculation, silently tuning
    // to a wrong/garbage channel instead of the command.
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
    // Apply a queued tune here (loop task) instead of in the web/async_tcp
    // task, which must not block on the Si4703 STC spin.
    if (_tunePending) {
        _tunePending = false;
        _tuneApplied = true;
        // Same 10 kHz-step units as RadioStatus.frequency, no conversion.
        _si470x.setFrequency(_pendingFrequency);
        _lastTuneMs = millis();
    }

    if (millis() - _lastTuneMs > TUNE_TIMEOUT_MS) {
        Serial.println(F("[RADIO] Tune timeout"));
        _state = RadioState::Idle;
        _notifyStatus();
    } else {
        _updateStatusFromHardware();
        if (_status.rssi > 0 && abs((int)_status.frequency - (int)_config.lastFrequency) < 5) {
            _state = RadioState::Idle;
            // Fire deferred notifications here on the loop task: the tune
            // request came from the web/async_tcp task, which must not touch
            // LVGL or SSE.
            if (_tuneApplied) {
                _tuneApplied = false;
                _notifyStationSelected(_status.frequency, "");
            }
            _notifyStatus();
        }
    }
}

void RadioService::_stepSeeking() {
    // Start a queued seek here (loop task) rather than in the web/async_tcp
    // task, which must not block on the Si4703 seek/STC spin. Raw SEEK is
    // issued non-blocking and STC is polled below - the library's seek()
    // waits for STC internally and would stall the whole loop task.
    if (_seekPending) {
        _seekPending = false;
        _lastTuneMs = millis();
        _si470x.getAllRegisters();
        uint16_t r02 = _si470x.getShadownRegister(0x02);
        r02 &= ~0x0C00u;                     // clear SEEKUP + SKMODE
        r02 |= 0x0100u;                      // SEEK = 1
        r02 |= (_seekUp ? 0x0200u : 0u);     // SEEKUP per request
        _si470x.setShadownRegister(0x02, r02);
        uint16_t r03 = _si470x.getShadownRegister(0x03);
        _si470x.setShadownRegister(0x03, r03 | 0x8000u);  // TUNE = 1
        _si470x.setAllRegisters();
        return;
    }

    _si470x.getStatus();
    // STC ("Seek/Tune Complete") is bit 14 of register 0x0A, not bit 0 or
    // bit 1. This is documented explicitly in SI470X.h itself ("STC 0Ah[14]
    // bit", "RDSR 0Ah[15] bit") and matches the si470x_reg0a bitfield
    // struct's declaration order (RSSI:8, ST:1, BLERA:2, RDSS:1, AFCRL:1,
    // SF_BL:1, STC:1, RDSR:1 - GCC packs bitfields LSB-first in declaration
    // order on little-endian targets, so STC lands at bit 14 and RDSR at
    // bit 15). The library's own getRdsReady()/getRdsSync() read exactly
    // this struct, confirming the layout. A previous "empirically
    // confirmed" fix used bit 0/1/2 here instead, which only happened to
    // work by coincidence (those bits are actually part of the RSSI byte,
    // bits 7:0) - it broke again once the AGC fix changed the RSSI
    // distribution enough to change how often those bits happened to be set.
    const uint16_t reg0a = _si470x.getShadownRegister(0x0A);
    const bool stcSet = (reg0a & 0x4000) != 0;  // STC: seek/tune complete (bit 14)
    if (!stcSet) {
        // Safety net: a full-band wrap seek shouldn't take this long; abort
        // so the CLI never hangs in the Seeking state.
        if (millis() - _lastTuneMs > 10000) {
            Serial.println(F("[RADIO::SEEK] Seek timed out, no station found"));
            uint16_t r02 = _si470x.getShadownRegister(0x02);
            _si470x.setShadownRegister(0x02, r02 & ~0x0100u);  // SEEK = 0
            uint16_t r03 = _si470x.getShadownRegister(0x03);
            _si470x.setShadownRegister(0x03, r03 & ~0x8000u);  // TUNE = 0
            _si470x.setAllRegisters();
            _state = RadioState::Idle;
            _notifyStatus();
        }
        return;
    }
    // Capture SF/BL before clearing SEEK - the chip clears both flags when
    // SEEK goes back low.
    const bool seekFail = (reg0a & 0x2000) != 0;  // SF/BL: seek fail / band limit

    // Terminate the pending seek (mirrors the library's waitAndFinishTune()).
    uint16_t r02 = _si470x.getShadownRegister(0x02);
    _si470x.setShadownRegister(0x02, r02 & ~0x0100u);  // SEEK = 0
    uint16_t r03 = _si470x.getShadownRegister(0x03);
    _si470x.setShadownRegister(0x03, r03 & ~0x8000u);  // TUNE = 0
    _si470x.setAllRegisters();

    if (seekFail) {
        Serial.println(F("[RADIO::SEEK] Seek hit band limit, no station found"));
    } else {
        _si470x.getAllRegisters();
        uint16_t foundFreq = _si470x.getRealFrequency();
        _si470x.setFrequency(foundFreq);  // sync cached frequency
        Serial.printf("[RADIO::SEEK] Tuned to %.2f MHz\n", foundFreq / 100.0f);
    }

    // The station just found by seek hasn't had its RDS text decoded yet -
    // clear the previous station's stale text (see the same reasoning in
    // setFrequency()).
    {
        MutexGuard guard(_mutex);
        if (guard.acquired()) {
            _status.frequency = _si470x.getFrequency();
            _status.programService[0] = '\0';
            _status.radioText[0] = '\0';
        }
    }
    _updateStatusFromHardware();
    _state = RadioState::Idle;
    _notifyStatus();
    _notifyStationSelected(_status.frequency, "");
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

        case ScanState::SeekWait: {
            const int seekResult = _scanSeekWait();
            // 0 = still running, 1 = station found, 2 = seek fail (no more stations)
            if (seekResult == 1) {
                Serial.printf("[RADIO::SCAN] Phase: SeekWait - FOUND station, settling...\n");
                _scan.phase = ScanState::Settle;
                _scan.settleStartMs = millis();
            } else if (seekResult == 2) {
                Serial.printf("[RADIO::SCAN] Phase: SeekWait - seek fail, scan complete (%u found)\n", _scan.stationCount);
                _scan.phase = ScanState::Complete;
            } else if (millis() - _scan.lastSeekMs > 12000) {
                Serial.printf("[RADIO::SCAN] Phase: SeekWait - TIMEOUT after %lu ms, moving to NextSeek\n", 
                    millis() - _scan.lastSeekMs);
                _scan.phase = ScanState::NextSeek;
            }
            break;
            }

        // RSSI/AGC needs roughly 100-200 ms to settle after STC (seek
        // complete) before it reflects the actual signal strength at the
        // new frequency - reading it immediately produced systematically
        // too-low RSSI values (see issue #2), which combined with the
        // static kMinStationRssi threshold to reject stations the chip's
        // own seek logic had already validated. Non-blocking wait: the
        // loop() task keeps running (UI, RDS polling for other frequencies
        // don't apply here since we're mid-scan, but nothing blocks).
        case ScanState::Settle:
            if (millis() - _scan.settleStartMs >= 150) {
                _scan.phase = ScanState::StoreStation;
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

         // Queue the tune instead of blocking here: this runs on the
         // async_tcp web task and the Si4703 setChannel() spins on STC,
         // which trips the task watchdog. _stepTuning() (loop task) applies it.
         _pendingFrequency = frequency;
         _tunePending = true;
         _tuneApplied = false;
     }

     _state = RadioState::Tuning;
     _lastTuneMs = millis();
     // Deliberately do NOT call _notifyStatus()/station-selected here: this
     // runs on the async_tcp web task, and LVGL/SSE are not thread-safe. The
     // loop task fires them once the tune is actually applied in _stepTuning().
     return true;
}

bool RadioService::nudgeFrequency(int step) {
    if (step == 0) return false;
    int16_t target = static_cast<int16_t>(_status.frequency) + step;
    if (target < app_config::kMinFrequency10kHz || target > app_config::kMaxFrequency10kHz) return false;
    uint16_t newFreq = static_cast<uint16_t>(target);
    if (!isValidFrequency(newFreq)) return false;
    return setFrequency(newFreq);
}

bool RadioService::seekUp() {
     if (_state != RadioState::Idle && _state != RadioState::Seeking) return false;
     // Queue the seek; actual _si470x.seek() runs in _stepSeeking() (loop
     // task) so the async_tcp web task never blocks on the STC spin.
     _seekUp = true;
     _seekPending = true;
     _state = RadioState::Seeking;
     _notifyStatus();
     return true;
}

bool RadioService::seekDown() {
     if (_state != RadioState::Idle && _state != RadioState::Seeking) return false;
     _seekUp = false;
     _seekPending = true;
     _state = RadioState::Seeking;
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

void RadioService::setStations(const RadioStation* stations, size_t count) {
    if (count > app_config::kMaxStations) count = app_config::kMaxStations;
    {
        MutexGuard guard(_mutex);
        if (!guard.acquired()) return;
        for (size_t i = 0; i < count; i++) {
            _scan.stations[i] = stations[i];
        }
        _scan.stationCount = count;
        _status.scanCount = count;
    }
    _notifyStationList();
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

    const bool rdsReady = _si470x.getRdsReady();
    // RDSR ("RDS ready") only means a group *slot* just completed - it
    // says nothing about whether that group's bits were actually decoded
    // correctly. RDSS ("RDS Synchronized", exposed as getRdsSync()) is the
    // flag that means the decoder has actually achieved block
    // synchronization on the 57 kHz subcarrier.
    // Neither our previous code nor the library's own getRdsText0A()/
    // getRdsText2A()/getRdsTime() ever check RDSS - they trust whatever
    // bytes are sitting in the block registers the moment RDSR blips true,
    // even while RDSS=0 (never synchronized). On a weak/noisy signal RDSR
    // can flip briefly from noise alone while RDSS stays 0 the entire
    // time, and decoding then "succeeds" against garbage register content
    // - producing exactly the kind of mangled text ("y SW<garbage>") seen
    // in practice, instead of no text at all.
    const bool rdsSynced = _si470x.getRdsSync();

    // Rate-limited visibility into whether the tuner is asserting RDS-ready
    // and RDS-synchronized at all, independent of whether we ever manage to
    // decode text from it. Distinguishes three different situations that
    // all look like "no usable RDS" from the outside:
    //   - ready=no, ~always: the Si4703 never even sees a group boundary on
    //     this signal - almost always a hardware/RF issue (weak/short
    //     antenna, poor placement/grounding), not a firmware bug.
    //   - ready=yes sometimes but synced=no: groups are arriving but the
    //     decoder never locks onto the bitstream - borderline signal
    //     quality. This used to be silently decoded as garbage text before
    //     the RDSS check below was added.
    //   - synced=yes but text never updates: a real bug in our group-type
    //     handling below, or in the library's getRdsText0A()/getRdsText2A()
    //     - worth filing upstream at that point.
    static uint32_t lastRdsDiagMs = 0;
    if (millis() - lastRdsDiagMs > 5000) {
        lastRdsDiagMs = millis();
        Serial.printf("[RDS::DIAG] ready=%s synced=%s rssi=%u stereo=%s freq=%.2f MHz\n",
                      rdsReady ? "yes" : "no", rdsSynced ? "yes" : "no", _status.rssi,
                      _status.stereo ? "yes" : "no", _status.frequency / 100.0f);
    }

    // BLERA ("Block A error rate", register 0x0A bits 10:9) is only
    // meaningful now that RDS verbose mode is enabled (see setRdsMode(1) in
    // _initHardware()) - in standard mode it hardware-reads 0 always. Per
    // the datasheet / the library's own getRdsReady() doc comment: "If
    // BLERA indicates 6 or more errors, the data in RDSA should be
    // discarded." 0=0 errors, 1=1-2, 2=3-5 (still correctable), 3=6+ or
    // uncorrectable checkword - reject that case outright even if RDSS
    // reports synchronized, rather than risk decoding a corrupted group.
    const uint8_t blerA = (_si470x.getShadownRegister(0x0A) >> 9) & 0x03;
    const bool blockAOk = blerA < 3;

    if (rdsReady && rdsSynced && blockAOk) {
        bool changed = false;
        {
            MutexGuard guard(_mutex);
            if (!guard.acquired()) return;

            // Program Service (PS) - station name from RDS 0A
            char* ps = _si470x.getRdsText0A();
            if (ps && ps[0] != '\0' && looksLikeValidRdsText(ps) && strcmp(ps, _status.programService) != 0) {
                strncpy(_status.programService, ps, 8);
                _status.programService[8] = '\0';
                changed = true;
                Serial.printf("[RDS::PS] decoded station name: \"%s\"\n", _status.programService);
            }

            // RadioText (RT) - song info etc. from RDS 2A
            char* rt = _si470x.getRdsText2A();
            if (rt && rt[0] != '\0' && looksLikeValidRdsText(rt) && strcmp(rt, _status.radioText) != 0) {
                strncpy(_status.radioText, rt, 64);
                _status.radioText[64] = '\0';
                changed = true;
                Serial.printf("[RDS::DEBUG] radio text decoded: \"%.48s\"\n", _status.radioText);
            }

            Serial.printf("[RDS::DEBUG] ready=true state=%d freq=%.2fMHz PS=\"%.8s\" RT=\"%.16s\"\n",
                          (int)_state, _status.frequency / 100.0f,
                          _status.programService, _status.radioText);
        }
        if (changed) _notifyStatus();

        // Clock Time (CT) - real UTC time from RDS group 4A, format "HH:MM +01:00"
        char* rdsTime = _si470x.getRdsTime();
        if (rdsTime && rdsTime[0] != '\0' && strlen(rdsTime) >= 5) {
            int hh = (rdsTime[0] - '0') * 10 + (rdsTime[1] - '0');
            int mm = (rdsTime[3] - '0') * 10 + (rdsTime[4] - '0');
            // Apply the local offset from the RDS string (e.g. "+01:00" at chars 6..11)
            int offsetMinutes = 0;
            if (strlen(rdsTime) >= 12 && rdsTime[5] != '\0') {
                char sign = rdsTime[6];
                int oh = (rdsTime[7] - '0') * 10 + (rdsTime[8] - '0');
                int om = (rdsTime[10] - '0') * 10 + (rdsTime[11] - '0');
                offsetMinutes = oh * 60 + om;
                if (sign == '-') offsetMinutes = -offsetMinutes;
            }
            int localMinutes = hh * 60 + mm + offsetMinutes;
            localMinutes = ((localMinutes % 1440) + 1440) % 1440;
            uint8_t localHour = localMinutes / 60;
            uint8_t localMinute = localMinutes % 60;
            if (_timeCallback) {
                _timeCallback(localHour, localMinute, 0);
            }
            Serial.printf("[RDS::TIME] CT=%s -> local %02u:%02u\n",
                          rdsTime, localHour, localMinute);
        }
    } else {
        // Throttled: log roughly every 2s so we can see the radio is in Idle
        // and polling but the current frequency carries no (or no decodable)
        // RDS yet. Include the raw status register (0x0A) so we can see
        // whether the chip is even synchronizing (RDSS) or flagging
        // RDS-ready (RDSR) at the hardware level.
        static uint32_t lastNoRdsLogMs = 0;
        if (millis() - lastNoRdsLogMs > 2000) {
            lastNoRdsLogMs = millis();
            _si470x.getStatus();  // refresh shadow registers 0x0A-0x0F
            uint16_t reg0a = _si470x.getShadownRegister(0x0A);
            uint16_t reg0d = _si470x.getShadownRegister(0x0D);
            uint16_t reg0f = _si470x.getShadownRegister(0x0F);
            // Bit layout per SI470X.h's own doc comments ("STC 0Ah[14] bit",
            // "RDSR 0Ah[15] bit") and the si470x_reg0a struct declaration
            // order: bit15=RDSR, bit14=STC, bit13=SF/BL, bit11=RDSS. See
            // _scanSeekWait()/_stepSeeking() for the full derivation. An
            // earlier version of this line used bit0/bit4 based on a
            // seek-path fix that turned out to be testing RSSI bits by
            // coincidence, not the actual status bits - now corrected.
            Serial.printf("[RDS::DEBUG] no RDS (state=%d freq=%.2fMHz RSSI=%u) reg0A=0x%04X (RDSR=%u RDSS=%u) 0D=0x%04X 0F=0x%04X\n",
                          (int)_state, _status.frequency / 100.0f, _status.rssi,
                          reg0a, (reg0a >> 15) & 0x01, (reg0a >> 11) & 0x01,
                          reg0d, reg0f);
        }
    }
}

bool RadioService::_scanSeekStart() {
    // Start seek from current scan frequency. currentFreq is already in the
    // same 10 kHz-step units setFrequency() expects (see _initHardware()).
    Serial.printf("[RADIO::SCAN::SEEK] Starting seek at %.2f MHz\n", _scan.currentFreq / 100.0f);
    _si470x.setFrequency(_scan.currentFreq);
    delay(10);

    // Do NOT use the library's seek(0,1): that overload blocks inside
    // waitAndFinishTune() until STC, then clears SEEK (which also clears the
    // STC and SF/BL flags) before returning. Polling STC afterwards in
    // _scanSeekWait() would therefore never see it - every seek timed out and
    // the scan found nothing. Instead issue the raw SEEK command non-blocking
    // and let _scanSeekWait() poll STC (bit 14) / SF/BL (bit 13) itself.
    // reg02: bit8=SEEK, bit9=SEEKUP, bit10=SKMODE (0=wrap). reg03: bit15=TUNE.
    _si470x.getAllRegisters();  // refresh shadow regs, like the library does
    uint16_t r02 = _si470x.getShadownRegister(0x02);
    r02 &= ~0x0C00u;                     // clear SEEKUP + SKMODE
    r02 |= 0x0100u;                      // SEEK = 1
    r02 |= 0x0200u;                      // SEEKUP = 1 (seek up)
    _si470x.setShadownRegister(0x02, r02);  // SKMODE stays 0 = wrap
    uint16_t r03 = _si470x.getShadownRegister(0x03);
    _si470x.setShadownRegister(0x03, r03 | 0x8000u);  // TUNE = 1
    _si470x.setAllRegisters();
    Serial.println(F("[RADIO::SCAN::SEEK] Seek started (non-blocking)"));
    return true;
}

int RadioService::_scanSeekWait() {
    _si470x.getStatus();
    // Register 0x0A bit layout (per SI470X.h's own doc comments - "STC
    // 0Ah[14] bit", "RDSR 0Ah[15] bit" - and the si470x_reg0a bitfield
    // struct declaration order): bit15=RDSR, bit14=STC (Seek/Tune complete),
    // bit13=SF/BL (seek fail / band limit), bit11=RDSS. See _stepSeeking()
    // for the full explanation.
    const uint16_t reg0a = _si470x.getShadownRegister(0x0A);
    const bool stcSet   = (reg0a & 0x4000) != 0;  // bit 14
    if (!stcSet) return 0;  // still seeking

    // Capture SF/BL *before* clearing SEEK - the chip clears both STC and
    // SF/BL when SEEK goes back low.
    const bool seekFail = (reg0a & 0x2000) != 0;  // bit 13

    // Terminate the pending seek (mirrors the library's waitAndFinishTune()).
    uint16_t r02 = _si470x.getShadownRegister(0x02);
    _si470x.setShadownRegister(0x02, r02 & ~0x0100u);  // SEEK = 0
    uint16_t r03 = _si470x.getShadownRegister(0x03);
    _si470x.setShadownRegister(0x03, r03 & ~0x8000u);  // TUNE = 0
    _si470x.setAllRegisters();

    if (seekFail) {
        Serial.printf("[RADIO::SCAN::SEEK] Seek failed (SF/BL): no more valid stations in band\n");
        return 2;
    }

    // READCHAN (reg 0x0B) is only refreshed by getAllRegisters(), so pull the
    // full bank: getRealFrequency() then reads the actual tuned channel.
    _si470x.getAllRegisters();
    uint16_t freq = _si470x.getRealFrequency();
    _si470x.setFrequency(freq);  // sync the library's cached frequency
    uint8_t rssi = _si470x.getRssi();
    bool stereo = _si470x.isStereo();
    Serial.printf("[RADIO::SCAN::SEEK] FOUND: freq=%u (%.2f MHz), RSSI=%u, stereo=%s\n", 
        freq, freq / 100.0f, rssi, stereo ? "yes" : "no");
    return 1;
}

bool RadioService::_scanStoreStation() {
    _si470x.getAllRegisters();
    // Use getRealFrequency() instead of getFrequency()! getFrequency()
    // returns a cached value while getRealFrequency() reads from the
    // register. Both already return 10 kHz-step units (no *10 - see
    // _initHardware()); fall back to the scan cursor if the read returns 0.
    uint16_t realFreq = _si470x.getRealFrequency();
    uint16_t freq = (realFreq > 0) ? realFreq : _scan.currentFreq;
    
    uint8_t rssi = _si470x.getRssi();
    bool stereo = _si470x.isStereo();

    Serial.printf("[RADIO::SCAN::STORE] getRealFrequency()=%u (%.2f MHz), RSSI=%u, stereo=%s\n", 
        freq, freq / 100.0f, rssi, stereo ? "yes" : "no");

    // Jump the scan cursor past the found station so the next seek continues
    // upward instead of re-finding this same station. Must happen before any
    // early return (invalid/duplicate), otherwise the scan stalls at the same
    // frequencies and never reaches the upper part of the band.
    if (freq > _scan.currentFreq) {
        _scan.currentFreq = freq;
    }

    if (!isValidFrequency(freq) || _isDuplicateFrequency(freq)) {
        Serial.printf("[RADIO::SCAN::STORE] Rejected: invalid=%s (freq=%u, range=[8750-10800]), duplicate=%s\n",
            !isValidFrequency(freq) ? "yes" : "no",
            freq,
            _isDuplicateFrequency(freq) ? "yes" : "no");
        return false;
    }

    // Skip weak/noise hits so the limited station buffer is not filled with
    // marginal channels and real stations at the top of the band make it in.
    if (rssi < app_config::kMinStationRssi) {
        Serial.printf("[RADIO::SCAN::STORE] Skipped weak station %.2f MHz (RSSI=%u < %u)\n",
            freq / 100.0f, rssi, app_config::kMinStationRssi);
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