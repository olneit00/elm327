//
// RadioStore implementation
// LittleFS persistence for Radio config and stations
//

#include <Arduino.h>
#include "log/LogTail.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "radio/RadioStore.h"
#include "radio/RadioTypes.h"

RadioStore::RadioStore() {
}

RadioStore::~RadioStore() {
}

bool RadioStore::begin() {
    if (!LittleFS.begin()) {
        LOG.println(F("[STORE] LittleFS mount failed, formatting..."));
        LittleFS.format();
        if (!LittleFS.begin()) {
            LOG.println(F("[STORE] LittleFS format failed"));
            return false;
        }
    }
    _ensureDirs();
    LOG.println(F("[STORE] Ready"));
    return true;
}

bool RadioStore::_ensureDirs() {
    if (!LittleFS.exists("/radio")) {
        LittleFS.mkdir("/radio");
    }
    if (!LittleFS.exists("/wifi")) {
        LittleFS.mkdir("/wifi");
    }
    return true;
}

String RadioStore::_readFile(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) return "";
    String content = file.readString();
    file.close();
    return content;
}

bool RadioStore::_writeFile(const char* path, const String& content) {
    _ensureDirs();
    File file = LittleFS.open(path, "w");
    if (!file) return false;
    file.print(content);
    file.close();
    return true;
}

bool RadioStore::loadConfig(RadioConfig& config) {
    String content = _readFile(CONFIG_FILE);
    if (content.isEmpty()) {
        LOG.println(F("[STORE] No config file, using defaults"));
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) {
        LOG.printf("[STORE] Config parse error: %s\n", err.c_str());
        return false;
    }

    config.lastFrequency = doc["lastFrequency"] | 8750;
    config.lastVolume = doc["lastVolume"] | 8;
    config.lastMuted = doc["lastMuted"] | false;
    strlcpy(config.staSsid, doc["staSsid"] | "", sizeof(config.staSsid));
    strlcpy(config.staPassword, doc["staPassword"] | "", sizeof(config.staPassword));

    LOG.println(F("[STORE] Config loaded"));
    return true;
}

bool RadioStore::saveConfig(const RadioConfig& config) {
    JsonDocument doc;
    doc["lastFrequency"] = config.lastFrequency;
    doc["lastVolume"] = config.lastVolume;
    doc["lastMuted"] = config.lastMuted;
    doc["staSsid"] = config.staSsid;
    doc["staPassword"] = config.staPassword;

    String json;
    serializeJson(doc, json);
    bool ok = _writeFile(CONFIG_FILE, json);
    if (ok) LOG.println(F("[STORE] Config saved"));
    return ok;
}

bool RadioStore::loadStations(RadioStation* stations, size_t maxStations, size_t& count) {
    String content = _readFile(STATIONS_FILE);
    if (content.isEmpty()) {
        count = 0;
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) {
        LOG.printf("[STORE] Stations parse error: %s\n", err.c_str());
        count = 0;
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    count = 0;
    for (JsonObject obj : arr) {
        if (count >= maxStations) break;
        RadioStation& st = stations[count];
        st.frequency = obj["frequency"] | 0;
        strlcpy(st.programService, obj["programService"] | "", sizeof(st.programService));
        strlcpy(st.radioText, obj["radioText"] | "", sizeof(st.radioText));
        st.rssi = obj["rssi"] | 0;
        st.stereo = obj["stereo"] | false;
        st.favorite = obj["favorite"] | false;
        count++;
    }

    LOG.printf("[STORE] Loaded %d stations\n", count);
    return true;
}

bool RadioStore::saveStations(const RadioStation* stations, size_t count) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (size_t i = 0; i < count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["frequency"] = stations[i].frequency;
        obj["programService"] = stations[i].programService;
        obj["radioText"] = stations[i].radioText;
        obj["rssi"] = stations[i].rssi;
        obj["stereo"] = stations[i].stereo;
        obj["favorite"] = stations[i].favorite;
    }

    String json;
    serializeJson(doc, json);
    bool ok = _writeFile(STATIONS_FILE, json);
    if (ok) LOG.printf("[STORE] Saved %d stations\n", count);
    return ok;
}

bool RadioStore::loadStaCredentials(char* ssid, char* password) {
    String content = _readFile(STA_FILE);
    if (content.isEmpty()) {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) return false;

    strlcpy(ssid, doc["ssid"] | "", 33);
    strlcpy(password, doc["password"] | "", 65);
    return true;
}

bool RadioStore::saveStaCredentials(const char* ssid, const char* password) {
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["password"] = password;
    String json;
    serializeJson(doc, json);
    return _writeFile(STA_FILE, json);
}

void RadioStore::formatFS() {
    LittleFS.format();
    _ensureDirs();
}