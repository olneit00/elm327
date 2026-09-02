#pragma once
//
// RadioService - Central abstraction for Si4703 FM/RDS radio
// Non-blocking state machine design, called periodically from loop()
// Thread-safe: WebServer (async) and loop() (main task) are protected by mutex
//

#include <Arduino.h>
#include "RadioTypes.h"
#include "config/AppConfig.h"
#include <SI470X.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class RadioService {
public:
    // Observer callback for status changes (used by WebServer for SSE)
    using StatusCallback = void (*)(const RadioStatus&);
    using StationListCallback = void (*)(const RadioStation*, size_t);
    using ScanProgressCallback = void (*)(uint8_t progress, uint8_t count, bool scanning);
    // Fired once when a station is actively selected (explicit tune request
    // or a completed seek), as opposed to StatusCallback which also fires
    // continuously for RDS/RSSI polling updates. programService is empty
    // when not yet known (e.g. right after a fresh tune, before RDS has had
    // a chance to decode it) - consumers should fall back to the frequency.
    using StationSelectedCallback = void (*)(uint16_t frequency, const char* programService);

    RadioService();
    ~RadioService();

    // Initialize hardware (I2C, Si4703 reset). Returns false if Si4703 not found.
    bool begin();

    // Main loop - call periodically (e.g., every 33ms). Non-blocking.
    void loop();

    // Power control
    bool powerOn();
    void powerOff();
    bool isPoweredOn() const { return _state != RadioState::Off; }

    // Frequency control
    bool setFrequency(uint16_t frequency);  // 8750-10800 (10kHz steps)
    bool nudgeFrequency(int step);  // +100 or -100 (== +0.1 MHz / -0.1 MHz), keeps within band
    uint16_t getFrequency() const { return _status.frequency; }

    // Seek
    bool seekUp();
    bool seekDown();

    // Scan (Schnellscan: only freq+RSSI+stereo during scan)
    bool startScan();
    void cancelScan();
    uint8_t getScanProgress() const { return _status.scanProgress; }
    uint8_t getScanCount() const { return _status.scanCount; }
    const RadioStation* getStations(size_t& count) const;
    void setStations(const RadioStation* stations, size_t count);

    // Marks/unmarks a scanned station as favorite by frequency. Returns
    // false if no station with that frequency is currently in the scan
    // list. Favorites are embedded in RadioStation and persisted together
    // with the station list (see RadioStore::saveStations()).
    bool setFavorite(uint16_t frequency, bool favorite);

    // Volume (0-15 hardware scale, UI uses 0-100%)
    bool setVolume(uint8_t volume);  // 0-15
    uint8_t getVolume() const { return _status.volume; }
    bool setMuted(bool muted);
    bool isMuted() const { return _status.muted; }

    // Status. Returns a snapshot copy (not a reference) protected by _mutex:
    // _status is written continuously from loop() (main task) while this is
    // called from AsyncWebServer handlers (a different FreeRTOS task), so an
    // unprotected reference could observe a struct torn mid-update.
    RadioStatus getStatus() const;
    uint8_t getRSSI() const { return getStatus().rssi; }
    bool isStereo() const { return getStatus().stereo; }
    // Returned as String (not a raw pointer into _status) so the RDS text
    // is copied out while still under the mutex, avoiding a torn read of a
    // char[] concurrently being strncpy()'d in _pollRds() on the other task.
    String getProgramService() const { return getStatus().programService; }
    String getRadioText() const { return getStatus().radioText; }

    // Callbacks for external consumers (WebServer, RadioScreen)
    using TimeCallback = void (*)(uint8_t hour, uint8_t minute, uint8_t second);
    void setTimeCallback(TimeCallback cb) { _timeCallback = cb; }
    void setStatusCallback(StatusCallback cb) { _statusCallback = cb; }
    void setStationListCallback(StationListCallback cb) { _stationListCallback = cb; }
    void setScanProgressCallback(ScanProgressCallback cb) { _scanProgressCallback = cb; }
    void setStationSelectedCallback(StationSelectedCallback cb) { _stationSelectedCallback = cb; }

    // GALA interface - called with vehicle speed km/h
    void setSpeedKmh(float speedKmh);

    // Persistence helpers
    RadioConfig getConfig() const;
    void applyConfig(const RadioConfig& config);

private:
     // Internal SI470X instance (from PU2CLR library)
     SI470X _si470x;

     // Mutex for thread-safe access to _status and _config from WebServer (async)
     // and loop() (main task). Acquired via xSemaphoreTake in critical sections.
     SemaphoreHandle_t _mutex = nullptr;

    // State machine
    RadioState _state = RadioState::Off;
    RadioStatus _status;
    RadioConfig _config;

    // Scan state machine
    struct ScanState {
        enum Phase { Idle, SeekStart, SeekWait, Settle, StoreStation, NextSeek, Complete } phase = Phase::Idle;
        uint16_t currentFreq = app_config::kMinFrequency10kHz;
        uint32_t lastSeekMs = 0;
        // Timestamp when STC (seek complete) was detected for the current
        // hit, used by the Settle phase to wait for RSSI/AGC to stabilize
        // before _scanStoreStation() reads it - see _stepScan()'s Settle
        // case for the full explanation.
        uint32_t settleStartMs = 0;
        RadioStation stations[app_config::kMaxStations];
        size_t stationCount = 0;
    } _scan;

    // RDS polling
    uint32_t _lastRdsPollMs = 0;
    static constexpr uint32_t RDS_POLL_INTERVAL_MS = 40;

    // Seek/tune timeout
    uint32_t _lastTuneMs = 0;
    static constexpr uint32_t TUNE_TIMEOUT_MS = 3000;

    // Pending frequency for async tuning: setFrequency()/nudgeFrequency() are
    // called from the async_tcp web task and must NOT block on the Si4703 bus
    // (its setChannel() spins on STC and trips the task watchdog). The request
    // stores the target here; _stepTuning() applies it in the loop() task.
    uint16_t _pendingFrequency = 0;
    bool _tunePending = false;
    bool _tuneApplied = false;

    // Pending seek start (async, same reason as tuning).
    bool _seekPending = false;
    bool _seekUp = true;

    // Pending volume/mute/power requests: setVolume()/setMuted()/powerOn()/
    // powerOff() are called from the async_tcp web task (via WebServer) and
    // must NOT touch the Si4703 I2C bus directly - only loop() (main task)
    // may do that, otherwise its register read-modify-write sequences
    // (_stepSeeking(), _pollRds(), _updateStatusFromHardware()) can
    // interleave with a concurrent I2C transaction from the other task and
    // corrupt the chip's shadow registers. Same pattern as _pendingFrequency
    // above; applied in _applyPendingHardwareOps(), called at the top of
    // loop().
    bool _volumePending = false;
    uint8_t _pendingVolume = 0;
    bool _mutePending = false;
    bool _pendingMuted = false;
    bool _powerOnPending = false;
    bool _powerOffPending = false;

    // GALA
    float _speedKmh = 0.0f;
    bool _galaEnabled = false;

    // Callbacks
    StatusCallback _statusCallback = nullptr;
    StationListCallback _stationListCallback = nullptr;
    ScanProgressCallback _scanProgressCallback = nullptr;
StationSelectedCallback _stationSelectedCallback = nullptr;
    TimeCallback _timeCallback = nullptr;

    // Internal helpers
    void _notifyStatus();
    void _notifyStationList();
    void _notifyScanProgress();
    void _notifyStationSelected(uint16_t frequency, const char* programService);

    bool _initHardware();
    void _applyVolume();
    void _updateStatusFromHardware();
    void _applyPendingHardwareOps();

    // State machine steps
    void _stepIdle();
    void _stepTuning();
    void _stepSeeking();
    void _stepScanning();
    void _stepScan();

// Scan helpers
    bool _scanSeekStart();
    int _scanSeekWait();   // 0=noch laufend, 1=Sender gefunden, 2=Seek-Fail (Band zu Ende)
    bool _scanStoreStation();
    void _scanNextSeek();
    void _scanComplete();

    // RDS
    void _pollRds();

     // Duplicate check for scan
     bool _isDuplicateFrequency(uint16_t freq) const;
};