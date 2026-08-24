#pragma once
//
// WifiManager - AP + STA dual mode WiFi management
//

#include <Arduino.h>
#include <WiFi.h>
#include "radio/RadioTypes.h"

class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    // Initialize WiFi: starts AP "Opel-Radio", optionally connects to STA if credentials stored
    bool begin(const char* apSsid = "Opel-Radio", const char* apPassword = "");

    // Main loop - call periodically
    void loop();

    // Get current mode and IP addresses
    bool isApActive() const { return _apActive; }
    bool isStaConnected() const { return _staConnected; }
    IPAddress getApIP() const { return WiFi.softAPIP(); }
    IPAddress getStaIP() const { return WiFi.localIP(); }
    int8_t getRSSI() const { return WiFi.RSSI(); }

    // STA connection management
    bool connectSta(const char* ssid, const char* password);
    void disconnectSta();
    void setStaCredentials(const char* ssid, const char* password);
    void getStaCredentials(char* ssid, char* password) const;

    // Callbacks
    using StaConnectCallback = void (*)(bool success, const IPAddress& ip);
    void setStaConnectCallback(StaConnectCallback cb) { _staConnectCallback = cb; }

private:
    const char* _apSsid = "Opel-Radio";
    const char* _apPassword = "";
    bool _apActive = false;
    bool _staConnected = false;
    uint32_t _lastStaCheckMs = 0;
    static constexpr uint32_t STA_CHECK_INTERVAL_MS = 10000;

    StaConnectCallback _staConnectCallback = nullptr;

    void _startAP();
    void _checkStaConnection();
};