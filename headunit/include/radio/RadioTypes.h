#pragma once
//
// Radio data types shared across RadioService, WebServer, RadioScreen, RadioStore
//

#include <stdint.h>
#include <string.h>

#include "config/AppConfig.h"

enum class RadioState {
    Off,
    Idle,
    Tuning,
    Seeking,
    Scanning
};

struct RadioStation {
    uint16_t frequency = 0;          // 10kHz steps: 8750 = 87.50 MHz
    char programService[9] = {0};    // RDS PS name (8 chars + null)
    char radioText[65] = {0};        // RDS RadioText (64 chars + null)
    uint8_t rssi = 0;                // Signal strength 0-255
    bool stereo = false;             // Stereo indicator
    bool favorite = false;           // User favorite marker
};

struct RadioStatus {
    RadioState state = RadioState::Off;
    uint16_t frequency = 8750;       // Current frequency
    char programService[9] = {0};    // Current PS name
    char radioText[65] = {0};        // Current RadioText
    uint8_t rssi = 0;                // Current RSSI
    bool stereo = false;             // Current stereo status
    uint8_t volume = 8;              // 0-15 (hardware scale)
    bool muted = false;              // Mute state
    uint8_t scanProgress = 0;        // 0-100%
    uint8_t scanCount = 0;           // Stations found during scan

    // RDS detail fields (issue #3 "detailed view of rds texts"). Only
    // meaningful once RDS verbose mode is enabled (see
    // RadioService::_initHardware()) - rdsSynced/bler* hardware-read 0
    // otherwise. Refreshed on every _pollRds() poll regardless of whether
    // PS/RT text itself changed, so the web frontend can show live sync
    // status even while no (new) text is available.
    bool rdsSynced = false;          // RDSS - RDS decoder synchronized
    uint16_t rdsPiCode = 0;          // PI (Programme Identification), block A, 0x0000 = unknown
    uint8_t rdsPty = 0;              // PTY (Program Type) code, 0-31
    bool rdsTp = false;              // Traffic Program flag
    bool rdsTa = false;              // Traffic Announcement flag (only set within group type 0)
    uint8_t rdsBlerA = 0;            // Block error rate A, 0=none .. 3=6+/uncorrectable
    uint8_t rdsBlerB = 0;            // Block error rate B
    uint8_t rdsBlerC = 0;            // Block error rate C
    uint8_t rdsBlerD = 0;            // Block error rate D
    uint32_t lastRdsUpdateMs = 0;    // millis() of the last accepted PS/RT update
};


struct RadioConfig {
    uint16_t lastFrequency = 8750;   // Last tuned frequency
    uint8_t lastVolume = 8;          // Last volume (0-15)
    bool lastMuted = false;          // Last mute state
    char staSsid[33] = {0};          // WiFi STA SSID
    char staPassword[65] = {0};      // WiFi STA password
};

inline bool operator==(const RadioStation& a, const RadioStation& b) {
    return a.frequency == b.frequency;
}

// Full field-wise equality of everything RadioStore actually persists (not
// just the frequency, unlike operator== above). Used by main.cpp to detect
// whether the station list needs to be re-saved to flash.
inline bool stationPersistEquals(const RadioStation& a, const RadioStation& b) {
    return a.frequency == b.frequency && a.favorite == b.favorite && a.rssi == b.rssi &&
           a.stereo == b.stereo && strcmp(a.programService, b.programService) == 0 &&
           strcmp(a.radioText, b.radioText) == 0;
}

// Field-wise comparison (not memcmp, to stay correct across struct padding)
// used by main.cpp to only persist RadioConfig when something actually
// changed, instead of writing to flash on every periodic save tick.
inline bool operator==(const RadioConfig& a, const RadioConfig& b) {
    return a.lastFrequency == b.lastFrequency &&
           a.lastVolume == b.lastVolume &&
           a.lastMuted == b.lastMuted &&
           strcmp(a.staSsid, b.staSsid) == 0 &&
           strcmp(a.staPassword, b.staPassword) == 0;
}
inline bool operator!=(const RadioConfig& a, const RadioConfig& b) {
    return !(a == b);
}

inline bool isValidFrequency(uint16_t freq) {
    return freq >= app_config::kMinFrequency10kHz && freq <= app_config::kMaxFrequency10kHz;
}

inline uint16_t freqToDisplay(uint16_t freq) {
    return freq;  // Stored as 10kHz steps
}

inline float freqToMHz(uint16_t freq) {
    return freq / 100.0f;
}

inline uint16_t mhzToFreq(float mhz) {
    return static_cast<uint16_t>(mhz * 100.0f + 0.5f);
}

inline uint8_t volumePercentToHardware(uint8_t percent) {
    if (percent > 100) percent = 100;
    return static_cast<uint8_t>((percent * 15 + 50) / 100);  // Round to nearest
}

inline uint8_t volumeHardwareToPercent(uint8_t hw) {
    if (hw > 15) hw = 15;
    return static_cast<uint8_t>((hw * 100 + 7) / 15);  // Round to nearest
}

// RDS PTY (Program Type) code -> human-readable name, European RDS table
// (EN 50067 / IEC 62106). Used by the web frontend's RDS detail view
// (issue #3). PTY is a 5-bit code (0-31), always present in RDS block B
// regardless of group type.
inline const char* rdsProgramTypeName(uint8_t pty) {
    static const char* const kNames[32] = {
        "Kein Programmtyp", "Nachrichten", "Zeitgeschehen", "Info",
        "Sport", "Bildung", "Hoerspiel", "Kultur",
        "Wissenschaft", "Verschiedenes", "Popmusik", "Rockmusik",
        "Unterhaltungsmusik", "Klassik (leicht)", "Klassik (ernst)", "Andere Musik",
        "Wetter", "Finanzen", "Kinderprogramm", "Soziales",
        "Religion", "Anrufsendung", "Reise", "Freizeit",
        "Jazz", "Country", "Nationale Musik", "Oldies",
        "Folk", "Dokumentation", "Alarmtest", "Alarm"
    };
    return (pty < 32) ? kNames[pty] : "Unbekannt";
}