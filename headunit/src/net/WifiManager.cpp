//
// WifiManager implementation
// AP + STA dual mode WiFi management
//

#include <Arduino.h>
#include <WiFi.h>
#include "net/WifiManager.h"
#include "radio/RadioTypes.h"

WifiManager::WifiManager() {
}

WifiManager::~WifiManager() {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
}

bool WifiManager::begin(const char* apSsid, const char* apPassword) {
    _apSsid = apSsid;
    _apPassword = apPassword;

    WiFi.mode(WIFI_AP_STA);

    _startAP();

    Serial.println(F("[WIFI] AP + STA mode started"));
    Serial.print(F("[WIFI] AP SSID: "));
    Serial.println(_apSsid);
    Serial.print(F("[WIFI] AP IP: "));
    Serial.println(WiFi.softAPIP());

    return true;
}

void WifiManager::_startAP() {
    if (_apPassword && strlen(_apPassword) >= 8) {
        WiFi.softAP(_apSsid, _apPassword);
    } else {
        WiFi.softAP(_apSsid);
    }
    _apActive = true;
}

void WifiManager::loop() {
    _checkStaConnection();
}

bool WifiManager::connectSta(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) return false;

    Serial.print(F("[WIFI] Connecting to STA: "));
    Serial.println(ssid);

    WiFi.begin(ssid, password ? password : "");

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 20000) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        _staConnected = true;
        Serial.print(F("[WIFI] STA connected, IP: "));
        Serial.println(WiFi.localIP());

        if (_staConnectCallback) {
            _staConnectCallback(true, WiFi.localIP());
        }
        return true;
    } else {
        Serial.println(F("[WIFI] STA connection timeout"));
        if (_staConnectCallback) {
            _staConnectCallback(false, IPAddress(0,0,0,0));
        }
        return false;
    }
}

void WifiManager::disconnectSta() {
    WiFi.disconnect(true);
    _staConnected = false;
    Serial.println(F("[WIFI] STA disconnected"));
}

void WifiManager::setStaCredentials(const char* ssid, const char* password) {
    // Store in RadioConfig via RadioService later
    // For now just connect
    connectSta(ssid, password);
}

void WifiManager::getStaCredentials(char* ssid, char* password) const {
    // Will be filled from RadioConfig
    ssid[0] = '\0';
    password[0] = '\0';
}

void WifiManager::_checkStaConnection() {
    if (millis() - _lastStaCheckMs < STA_CHECK_INTERVAL_MS) return;
    _lastStaCheckMs = millis();

    if (_staConnected && WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WIFI] STA connection lost"));
        _staConnected = false;
        if (_staConnectCallback) {
            _staConnectCallback(false, IPAddress(0,0,0,0));
        }
    }
}