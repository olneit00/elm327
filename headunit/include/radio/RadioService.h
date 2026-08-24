#pragma once
//
// RadioService - Central abstraction for Si4703 FM/RDS radio
// Non-blocking state machine design, called periodically from loop()
// Thread-safe: WebServer (async) and loop() (main task) are protected by mutex
//

#include <Arduino.h>
#include "RadioTypes.h"
#include <SI470X.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class RadioService {
public:
    // Observer callback for status changes (used by WebServer for SSE)
    using StatusCallback = void (*)(const RadioStatus&);
    using StationListCallback = void (*)(const RadioStation*, size_t);
    using ScanProgressCallback = void (*)(uint8_t progress, uint8_t count, bool scanning);

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

    // Volume (0-15 hardware scale, UI uses 0-100%)
    bool setVolume(uint8_t volume);  // 0-15
    uint8_t getVolume() const { return _status.volume; }
    bool setMuted(bool muted);
    bool isMuted() const { return _status.muted; }

    // Status
    const RadioStatus& getStatus() const { return _status; }
    uint8_t getRSSI() const { return _status.rssi; }
    bool isStereo() const { return _status.stereo; }
    const char* getProgramService() const { return _status.programService; }
    const char* getRadioText() const { return _status.radioText; }

    // Callbacks for external consumers (WebServer, RadioScreen)
    void setStatusCallback(StatusCallback cb) { _statusCallback = cb; }
    void setStationListCallback(StationListCallback cb) { _stationListCallback = cb; }
    void setScanProgressCallback(ScanProgressCallback cb) { _scanProgressCallback = cb; }

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
        enum Phase { Idle, SeekStart, SeekWait, StoreStation, NextSeek, Complete } phase = Phase::Idle;
        uint16_t currentFreq = 8750;
        uint32_t lastSeekMs = 0;
        RadioStation stations[50];  // Max 50 stations
        size_t stationCount = 0;
    } _scan;

    // RDS polling
    uint32_t _lastRdsPollMs = 0;
    static constexpr uint32_t RDS_POLL_INTERVAL_MS = 40;

    // Seek/tune timeout
    uint32_t _lastTuneMs = 0;
    static constexpr uint32_t TUNE_TIMEOUT_MS = 3000;

    // GALA
    float _speedKmh = 0.0f;
    bool _galaEnabled = false;

    // Callbacks
    StatusCallback _statusCallback = nullptr;
    StationListCallback _stationListCallback = nullptr;
    ScanProgressCallback _scanProgressCallback = nullptr;

    // Internal helpers
    void _notifyStatus();
    void _notifyStationList();
    void _notifyScanProgress();

    bool _initHardware();
    void _applyVolume();
    void _updateStatusFromHardware();

    // State machine steps
    void _stepIdle();
    void _stepTuning();
    void _stepSeeking();
    void _stepScanning();
    void _stepScan();

    // Scan helpers
    bool _scanSeekStart();
    bool _scanSeekWait();
    bool _scanStoreStation();
    void _scanNextSeek();
    void _scanComplete();

    // RDS
    void _pollRds();

     // Duplicate check for scan
     bool _isDuplicateFrequency(uint16_t freq) const;
};