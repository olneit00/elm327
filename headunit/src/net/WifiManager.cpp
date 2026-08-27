//
// WifiManager implementation
// AP + STA dual mode WiFi management
//

#include <Arduino.h>
#include "log/LogTail.h"
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

    LOG.println(F("[WIFI] AP + STA mode started"));
    LOG.print(F("[WIFI] AP SSID: "));
    LOG.println(_apSsid);
    LOG.print(F("[WIFI] AP IP: "));
    LOG.println(WiFi.softAPIP());

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
    if (_staConnectionState == StaConnectionState::Connecting) {
        _pollStaConnecting();
    } else {
        _checkStaConnection();
    }
}

bool WifiManager::connectSta(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) return false;

    LOG.print(F("[WIFI] Connecting to STA (async): "));
    LOG.println(ssid);

    WiFi.begin(ssid, password ? password : "");
    _staConnectionState = StaConnectionState::Connecting;
    _staConnectStartMs = millis();
    _lastStaConnectPollMs = 0;  // poll immediately on the next loop()
    return true;
}

void WifiManager::_pollStaConnecting() {
    if (millis() - _lastStaConnectPollMs < STA_CONNECT_POLL_INTERVAL_MS) return;
    _lastStaConnectPollMs = millis();

    if (WiFi.status() == WL_CONNECTED) {
        _staConnected = true;
        _staConnectionState = StaConnectionState::Connected;
        LOG.print(F("[WIFI] STA connected, IP: "));
        LOG.println(WiFi.localIP());
        if (_staConnectCallback) {
            _staConnectCallback(true, WiFi.localIP());
        }
        return;
    }

    if (millis() - _staConnectStartMs > STA_CONNECT_TIMEOUT_MS) {
        _staConnectionState = StaConnectionState::Failed;
        LOG.println(F("[WIFI] STA connection timeout"));
        if (_staConnectCallback) {
            _staConnectCallback(false, IPAddress(0, 0, 0, 0));
        }
    }
}

void WifiManager::disconnectSta() {
    WiFi.disconnect(true);
    _staConnected = false;
    _staConnectionState = StaConnectionState::Idle;
    LOG.println(F("[WIFI] STA disconnected"));
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
        LOG.println(F("[WIFI] STA connection lost"));
        _staConnected = false;
        if (_staConnectCallback) {
            _staConnectCallback(false, IPAddress(0,0,0,0));
        }
    }
}