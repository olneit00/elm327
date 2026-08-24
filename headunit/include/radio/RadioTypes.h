#pragma once
//
// Radio data types shared across RadioService, WebServer, RadioScreen, RadioStore
//

#include <stdint.h>

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

inline bool isValidFrequency(uint16_t freq) {
    return freq >= 8750 && freq <= 10800;
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