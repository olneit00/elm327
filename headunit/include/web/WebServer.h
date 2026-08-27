#pragma once
//
// WebServer - ESPAsyncWebServer wrapper for Radio REST API + SSE
//

#include <Arduino.h>
#include <functional>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "radio/RadioTypes.h"
#include "radio/RadioService.h"
#include "net/WifiManager.h"

class WebServer {
public:
    WebServer(RadioService& radioService, WifiManager& wifiManager);
    ~WebServer();

    bool begin(uint16_t port = 80);
    void loop();  // Not needed for async, but kept for consistency

    // SSE client management
    void broadcastStatus(const RadioStatus& status);
    void broadcastStationList(const RadioStation* stations, size_t count);
    void broadcastScanProgress(uint8_t progress, uint8_t count, bool scanning);

    // Live log tail over SSE (no serial required); publish from loop().
    void pollLogTail();

private:
    RadioService& _radioService;
    WifiManager& _wifiManager;
    AsyncWebServer* _server = nullptr;
    AsyncEventSource* _events = nullptr;
    AsyncEventSource* _logEvents = nullptr;
    uint64_t _logLastSeq = 0;   // SSE log publish cursor
    uint32_t _logLastPollMs = 0;

    // REST endpoints
    void _setupRestEndpoints();
    void _setupSSE();
    void _setupStaticFiles();

    // JSON-POST helper: accumulates the request body across onBody chunks,
    // deserializes it once the full body has arrived (onRequest), and hands
    // the parsed document to the handler. Replies 400 on parse failure.
    using JsonBodyHandler = std::function<void(AsyncWebServerRequest*, const JsonDocument&)>;
    void _registerJsonPost(const char* uri, JsonBodyHandler handler);

    // Uniform JSON responses with CORS headers
    static void _sendJson(AsyncWebServerRequest* request, int code, const char* body);

    // Static file content
    static const char* _getRadioIndexHtml();
    static const char* _getStationHtml();
    static const char* _getStyleCss();
    static const char* _getAppJs();

    // JSON helpers
    static String _statusToJson(const RadioStatus& status);
    static String _stationListToJson(const RadioStation* stations, size_t count);
    static String _scanProgressToJson(uint8_t progress, uint8_t count, bool scanning);

    // CORS
    static void _addCorsHeaders(AsyncWebServerResponse* response);
};