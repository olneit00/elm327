#pragma once
//
// RadioStore - LittleFS persistence for Radio config and stations
//

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "radio/RadioTypes.h"

class RadioStore {
public:
    RadioStore();
    ~RadioStore();

    bool begin();

    // Config persistence
    bool loadConfig(RadioConfig& config);
    bool saveConfig(const RadioConfig& config);

    // Station list persistence
    bool loadStations(RadioStation* stations, size_t maxStations, size_t& count);
    bool saveStations(const RadioStation* stations, size_t count);

    // STA WiFi credentials
    bool loadStaCredentials(char* ssid, char* password);
    bool saveStaCredentials(const char* ssid, const char* password);

    // Utility
    static void formatFS();

private:
    static constexpr const char* CONFIG_FILE = "/radio/config.json";
    static constexpr const char* STATIONS_FILE = "/radio/stations.json";
    static constexpr const char* STA_FILE = "/wifi/sta.json";

    static bool _ensureDirs();
    static String _readFile(const char* path);
    static bool _writeFile(const char* path, const String& content);
};