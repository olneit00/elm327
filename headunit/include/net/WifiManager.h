#pragma once
//
// WifiManager - AP + STA dual mode WiFi management
//

#include <Arduino.h>
#include <WiFi.h>
#include "radio/RadioTypes.h"

class WifiManager {
public:
    // STA connection lifecycle, driven asynchronously from loop() so
    // connectSta() itself never blocks the caller (see connectSta() below).
    enum class StaConnectionState { Idle, Connecting, Connected, Failed };

    WifiManager();
    ~WifiManager();

    // Initialize WiFi: starts AP "Opel-Radio", optionally connects to STA if credentials stored.
    // apPassword defaults to a non-empty value: WPA2 requires >= 8 characters,
    // otherwise WiFi.softAP() silently falls back to an open (unencrypted) AP.
    bool begin(const char* apSsid = "Opel-Radio", const char* apPassword = "OpelRadio1935");

    // Main loop - call periodically
    void loop();

    // Get current mode and IP addresses
    bool isApActive() const { return _apActive; }
    bool isStaConnected() const { return _staConnected; }
    IPAddress getApIP() const { return WiFi.softAPIP(); }
    IPAddress getStaIP() const { return WiFi.localIP(); }
    int8_t getRSSI() const { return WiFi.RSSI(); }

    // STA connection management. connectSta() only *starts* WiFi.begin() and
    // returns immediately (true if a connection attempt was started, false
    // for an empty SSID) - it does not block until connected. Poll
    // getStaConnectionState() (or the staConnectCallback) for the outcome.
    // A previous blocking implementation waited synchronously for up to 20s,
    // which - when called from an AsyncWebServer request handler - stalled
    // the whole async_tcp task (all other HTTP/SSE traffic) for that long.
    bool connectSta(const char* ssid, const char* password);
    StaConnectionState getStaConnectionState() const { return _staConnectionState; }
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

    StaConnectionState _staConnectionState = StaConnectionState::Idle;
    uint32_t _staConnectStartMs = 0;
    uint32_t _lastStaConnectPollMs = 0;
    static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 20000;
    static constexpr uint32_t STA_CONNECT_POLL_INTERVAL_MS = 250;

    StaConnectCallback _staConnectCallback = nullptr;

    void _startAP();
    void _checkStaConnection();
    void _pollStaConnecting();
};