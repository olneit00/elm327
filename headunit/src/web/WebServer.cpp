//
// WebServer implementation
// ESPAsyncWebServer wrapper for Radio REST API + SSE
//

#include <Arduino.h>
#include "log/LogTail.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "web/WebServer.h"
#include "radio/RadioService.h"
#include "net/WifiManager.h"

namespace {

// Inline HTML/CSS/JS assets. These used to be kept artificially small and
// minified to stay under ~5.7 KB per response, working around a send-buffer
// accounting bug in ESPAsyncWebServer (upstream #315, fixed in #316 - see
// the pinned commit in platformio.ini). With that fix in place, responses
// of any size are delivered correctly, so there is no longer a hard size
// ceiling here; the assets stay minified purely to save flash/RAM, not to
// dodge a transport bug.

}  // namespace

WebServer::WebServer(RadioService& radioService, WifiManager& wifiManager)
    : _radioService(radioService), _wifiManager(wifiManager) {
}

WebServer::~WebServer() {
    // Deletion order matters here: AsyncWebServer::addHandler() stores a raw
    // pointer to _events but does not take ownership of it (ESPAsyncWebServer
    // never frees handlers registered this way), so _events must be deleted
    // by us - and only after _server is gone, in case its own teardown still
    // touches registered handlers.
    if (_server) {
        delete _server;
        _server = nullptr;
    }
    if (_events) {
        delete _events;
        _events = nullptr;
    }
}

bool WebServer::begin(uint16_t port) {
    _server = new AsyncWebServer(port);
    if (!_server) {
        LOG.println(F("[WEB] Failed to create AsyncWebServer"));
        return false;
    }

    _events = new AsyncEventSource("/api/radio/events");
    if (!_events) {
        LOG.println(F("[WEB] Failed to create AsyncEventSource"));
        return false;
    }

    // Log tail SSE source (see _setupLogTail / pollLogTail). Registered in
    // _setupSSE so it lives/dies with the server.
    _logEvents = new AsyncEventSource("/api/log/events");
    if (!_logEvents) {
        LOG.println(F("[WEB] Failed to create LogEventSource"));
        return false;
    }

    _setupRestEndpoints();
    _setupSSE();
    _setupStaticFiles();

    // curl-like request log on Serial: shows whether /api requests reach the
    // server at all (diagnosis for the unresponsive web frontend).
    static AsyncLoggingMiddleware loggingMiddleware;
    loggingMiddleware.setOutput(Serial);
    loggingMiddleware.setEnabled(true);
    _server->addMiddleware(&loggingMiddleware);

    _server->begin();
    LOG.printf("[WEB] Server started on port %d\n", port);
    LOG.println(F("[WEB] SSE endpoint: /api/radio/events"));

    return true;
}

void WebServer::loop() {
    // AsyncWebServer doesn't need loop(), but the log-tail SSE publisher
    // polls LogTail here so new debug lines reach web subscribers without a
    // serial connection.
    pollLogTail();
}

void WebServer::_sendJson(AsyncWebServerRequest* request, int code, const char* body) {
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", body);
    _addCorsHeaders(response);
    request->send(response);
}

void WebServer::_registerJsonPost(const char* uri, JsonBodyHandler handler) {
    // ESPAsyncWebServer calls the onBody callback per chunk and onRequest only
    // after the complete request (headers + full body) has been received.
    // The accumulated body therefore lives in request->_tempObject between
    // both callbacks. If a client aborts mid-transfer, onRequest never runs,
    // so onDisconnect() frees any body accumulated so far instead of leaking
    // it (relevant since this device is reachable from an open WiFi AP,
    // where repeated abort requests could otherwise leak heap over time).
    _server->on(uri, HTTP_POST,
        [handler](AsyncWebServerRequest* request) {
            String* body = static_cast<String*>(request->_tempObject);
            request->_tempObject = nullptr;

            JsonDocument doc;
            bool ok = body != nullptr;
            if (ok) {
                DeserializationError err = deserializeJson(doc, *body);
                ok = err == DeserializationError::Ok;
            }
            delete body;

            if (!ok) {
                _sendJson(request, 400, "{\"error\":\"Invalid JSON\"}");
                return;
            }
            handler(request, doc);
        },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            (void)index;
            (void)total;
            if (request->_tempObject == nullptr) {
                request->_tempObject = new String();
                request->onDisconnect([request]() {
                    delete static_cast<String*>(request->_tempObject);
                    request->_tempObject = nullptr;
                });
            }
            static_cast<String*>(request->_tempObject)->concat(reinterpret_cast<const char*>(data), len);
        });
}

void WebServer::_setupRestEndpoints() {
    // CORS for all endpoints
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
    // Firmware updates change the inline assets under the SAME URLs; without
    // this the phone may keep serving a stale app.js from the cache.
    DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");

    // GET /api/radio/status
    _server->on("/api/radio/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String json = _statusToJson(_radioService.getStatus());
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        _addCorsHeaders(response);
        request->send(response);
    });

    // GET /api/log - last captured debug lines as plain text (web tail without serial).
    // Returns newline-separated LOG output, CJK/Latin-1 safe (JSON-escaped).
    _server->on("/api/log", HTTP_GET, [](AsyncWebServerRequest* request) {
        char buf[8192];
        size_t n = LogTail::instance().dump(buf, sizeof(buf));
        // JSON-encode so the browser can display control chars / quotes safely.
        String json = "\"";
        for (size_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '"') json += "\\\"";
            else if (c == '\\') json += "\\\\";
            else if (c == '\n') json += "\\n";
            else if (c == '\r') { /* skip */ }
            else if ((unsigned char)c < 0x20) json += '?';
            else json += c;
        }
        json += "\"";
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        request->send(response);
    });

    // GET /log - self-contained web log tail page (no serial, no LittleFS
    // upload needed). Opens an EventSource on /api/log/events and live-appends
    // every debug line the device prints.
    _server->on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
        const char* html =
            "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>OBD · Log</title>"
            "<style>body{font-family:ui-monospace,Consolas,monospace;background:#0d1117;color:#c9d1d9;margin:0;}"
            "header{padding:10px 14px;background:#161b22;border-bottom:1px solid #30363d;"
            "font-weight:bold;position:sticky;top:0;display:flex;gap:10px;align-items:center;}"
            "header span{font-size:12px;color:#8b949e;font-weight:normal;}"
            "button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:4px 10px;cursor:pointer;}"
            "pre{white-space:pre-wrap;word-break:break-all;padding:10px 14px;margin:0;font-size:12.5px;line-height:1.5;}"
            ".rsttxt::before{color:#3fb950}pre span{white-space:pre-wrap}</style>"
            "</head><body>"
            "<header>📋 Live-Log <span>tail -f · ohne serielle Verbindung</span> "
            "<span id=dim style=margin-left:auto;color:#56d364>● verbunden</span></header>"
            "<pre id=log></pre>"
            "<script>"
            "const pre=document.getElementById('log');"
            "const es=new EventSource('/api/log/events');"
            "es.addEventListener('log',e=>{pre.textContent+=e.data;if(pre.textContent.length>60000)pre.textContent=pre.textContent.slice(-40000);window.scrollTo(0,document.body.scrollHeight);});"
            "es.onerror=()=>document.getElementById('dim').textContent='getrennt · Verbinde neu …';"
            "</script></body></html>";
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", html);
        request->send(response);
    });

    // GET /api/radio/stations
    _server->on("/api/radio/stations", HTTP_GET, [this](AsyncWebServerRequest* request) {
        size_t count = 0;
        const RadioStation* stations = _radioService.getStations(count);
        String json = _stationListToJson(stations, count);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        _addCorsHeaders(response);
        request->send(response);
    });

    // GET /api/radio/scan/progress
    _server->on("/api/radio/scan/progress", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String json = _scanProgressToJson(
            _radioService.getScanProgress(),
            _radioService.getScanCount(),
            _radioService.getStatus().state == RadioState::Scanning
        );
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        _addCorsHeaders(response);
        request->send(response);
    });

    // POST /api/radio/tune
    _registerJsonPost("/api/radio/tune", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        uint16_t freq = doc["frequency"] | 0;
        if (!freq || !isValidFrequency(freq)) {
            _sendJson(request, 400, "{\"error\":\"Invalid frequency\"}");
            return;
        }

        if (_radioService.setFrequency(freq)) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 500, "{\"error\":\"Tune failed\"}");
        }
    });

    // POST /api/radio/frequency (alias for /api/radio/tune, used by the web UI)
    _registerJsonPost("/api/radio/frequency", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        uint16_t freq = doc["frequency"] | 0;
        if (!freq || !isValidFrequency(freq)) {
            _sendJson(request, 400, "{\"error\":\"Invalid frequency\"}");
            return;
        }

        if (_radioService.setFrequency(freq)) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 500, "{\"error\":\"Tune failed\"}");
        }
    });

    // POST /api/radio/nudge - shift frequency by +0.1 MHz (10) or -0.1 MHz (-10)
    _registerJsonPost("/api/radio/nudge", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        int step = doc["step"] | 0;
        // Accept the legacy ±100 (1 MHz in 10kHz units) too and map it down to
        // ±0.1 MHz so a stale cached app.js still works; the intended step is
        // ±10 (0.1 MHz).
        if (step == 100) step = 10;
        if (step == -100) step = -10;
        if (step != 10 && step != -10) {
            _sendJson(request, 400, "{\"error\":\"Invalid step\"}");
            return;
        }
        if (_radioService.nudgeFrequency(step)) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 500, "{\"error\":\"Nudge failed (out of band or radio off)\"}");
        }
    });

    // POST /api/radio/seek
    _registerJsonPost("/api/radio/seek", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        const char* dir = doc["direction"] | "up";
        bool success = false;
        if (strcmp(dir, "up") == 0) {
            success = _radioService.seekUp();
        } else if (strcmp(dir, "down") == 0) {
            success = _radioService.seekDown();
        }

        if (success) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 400, "{\"error\":\"Seek failed or invalid direction\"}");
        }
    });

    // POST /api/radio/scan/start
    _server->on("/api/radio/scan/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_radioService.startScan()) {
            AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"success\":true}");
            _addCorsHeaders(response);
            request->send(response);
        } else {
            request->send(400, "application/json", "{\"error\":\"Scan already in progress\"}");
        }
    });

    // POST /api/radio/scan/cancel
    _server->on("/api/radio/scan/cancel", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _radioService.cancelScan();
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"success\":true}");
        _addCorsHeaders(response);
        request->send(response);
    });

    // POST /api/radio/volume
    _registerJsonPost("/api/radio/volume", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        uint8_t vol = doc["volume"] | 0;
        if (vol > 100) vol = 100;
        // Convert percent (0-100) to hardware scale (0-15), rounded to nearest step
        // so the echoed value doesn't visibly jump back (floor-truncation drift).
        uint8_t hwVol = ((vol * 15) + 50) / 100;

        if (_radioService.setVolume(hwVol)) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 500, "{\"error\":\"Volume set failed\"}");
        }
    });

    // POST /api/radio/mute
    _registerJsonPost("/api/radio/mute", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        bool muted = doc["muted"] | false;
        _radioService.setMuted(muted);
        _sendJson(request, 200, "{\"success\":true}");
    });

    // POST /api/radio/power
    _registerJsonPost("/api/radio/power", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        bool on = doc["on"] | false;
        if (on) {
            _radioService.powerOn();
        } else {
            _radioService.powerOff();
        }
        _sendJson(request, 200, "{\"success\":true}");
    });

    // POST /api/radio/favorite
    _registerJsonPost("/api/radio/favorite", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        uint16_t freq = doc["frequency"] | 0;
        bool fav = doc["favorite"] | false;

        if (_radioService.setFavorite(freq, fav)) {
            _sendJson(request, 200, "{\"success\":true}");
        } else {
            _sendJson(request, 404, "{\"error\":\"Station not found\"}");
        }
    });

    // GET/POST /api/radio/config
    _server->on("/api/radio/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        RadioConfig config = _radioService.getConfig();
        JsonDocument doc;
        doc["lastFrequency"] = config.lastFrequency;
        // Convert hardware volume (0-15) to percent (0-100)
        doc["lastVolume"] = (config.lastVolume * 100) / 15;
        doc["lastMuted"] = config.lastMuted;
        doc["staSsid"] = config.staSsid;
        String json;
        serializeJson(doc, json);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        _addCorsHeaders(response);
        request->send(response);
    });

    _registerJsonPost("/api/radio/config", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        RadioConfig config = _radioService.getConfig();
        if (!doc["lastFrequency"].isNull()) {
            config.lastFrequency = doc["lastFrequency"] | config.lastFrequency;
        }
        if (!doc["lastVolume"].isNull()) {
            uint8_t percent = doc["lastVolume"] | ((config.lastVolume * 100) / 15);
            config.lastVolume = ((percent * 15) + 50) / 100;
        }
        if (!doc["lastMuted"].isNull()) {
            config.lastMuted = doc["lastMuted"] | config.lastMuted;
        }
        if (!doc["staSsid"].isNull()) {
            strlcpy(config.staSsid, doc["staSsid"] | "", sizeof(config.staSsid));
        }
        if (!doc["staPassword"].isNull()) {
            strlcpy(config.staPassword, doc["staPassword"] | "", sizeof(config.staPassword));
        }

        _radioService.applyConfig(config);

        _sendJson(request, 200, "{\"success\":true}");
    });

    // WiFi STA connect endpoint. connectSta() no longer blocks (see
    // WifiManager::connectSta()), so this responds immediately with
    // "connecting" - poll GET /api/wifi/status for the actual outcome.
    _registerJsonPost("/api/wifi/connect", [this](AsyncWebServerRequest* request, const JsonDocument& doc) {
        const char* ssid = doc["ssid"] | "";
        const char* password = doc["password"] | "";

        if (strlen(ssid) == 0) {
            _sendJson(request, 400, "{\"error\":\"SSID required\"}");
            return;
        }

        _wifiManager.connectSta(ssid, password);
        _sendJson(request, 202, "{\"success\":true,\"status\":\"connecting\"}");
    });

    // GET /api/wifi/status - poll target for the outcome of connectSta().
    _server->on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        const char* stateStr = "idle";
        switch (_wifiManager.getStaConnectionState()) {
            case WifiManager::StaConnectionState::Connecting: stateStr = "connecting"; break;
            case WifiManager::StaConnectionState::Connected:  stateStr = "connected";  break;
            case WifiManager::StaConnectionState::Failed:     stateStr = "failed";     break;
            case WifiManager::StaConnectionState::Idle:       stateStr = "idle";       break;
        }
        JsonDocument resp;
        resp["state"] = stateStr;
        resp["connected"] = _wifiManager.isStaConnected();
        if (_wifiManager.isStaConnected()) {
            resp["ip"] = _wifiManager.getStaIP().toString();
        }
        String json;
        serializeJson(resp, json);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        _addCorsHeaders(response);
        request->send(response);
    });

    // 404 handler
    _server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "application/json", "{\"error\":\"Not found\"}");
    });
}

void WebServer::_setupSSE() {
    _events->onConnect([this](AsyncEventSourceClient* client) {
        LOG.println(F("[WEB] SSE client connected"));
        // Send initial status
        String json = _statusToJson(_radioService.getStatus());
        client->send(json.c_str(), "status");
    });

    _server->addHandler(_events);

    // Log-tail SSE source: on connect, hand the subscriber the whole current
    // buffer so they start with history, then pollLogTail() pushes new lines.
    _logEvents->onConnect([this](AsyncEventSourceClient* client) {
        LOG.println(F("[WEB] Log SSE client connected"));
        char buf[8192];
        LogTail::instance().dump(buf, sizeof(buf));
        // The whole history as one SSE message (multi-line -> multiple data: lines).
        client->send(buf, "log", millis());
        _logLastSeq = LogTail::instance().lastSeq();
    });
    _server->addHandler(_logEvents);
}

void WebServer::pollLogTail() {
    // Push any new LOG lines to /api/log/events subscribers.
    if (!_logEvents || _logEvents->count() == 0) return;
    uint32_t now = millis();
    if (now - _logLastPollMs < 250) return;   // ~4 Hz is plenty for a log tail
    _logLastPollMs = now;

    uint64_t maxSeq = _logLastSeq;
    char buf[2048];
    size_t n = LogTail::instance().dumpAfter(_logLastSeq, &maxSeq, buf, sizeof(buf));
    if (n > 0) {
        _logEvents->send(buf, "log", millis());
        _logLastSeq = maxSeq;
    }
}

void WebServer::_setupStaticFiles() {
    // Serve the web assets from LittleFS. The ESPAsyncWebServer truncation
    // fix (PR #316, pinned in platformio.ini) makes responses of any size
    // reliable, so external files are preferred over embedded strings.
    // Fall back to the embedded strings only if LittleFS is unavailable.
    if (LittleFS.begin()) {
        _server->serveStatic("/radio/style.css", LittleFS, "/style.css").setDefaultFile("/style.css");
        _server->serveStatic("/radio/app.js", LittleFS, "/app.js").setDefaultFile("/app.js");
        _server->serveStatic("/radio", LittleFS, "/index.html").setDefaultFile("/index.html");
        _server->serveStatic("/", LittleFS, "/index.html").setDefaultFile("/index.html");

        // Classic 1930s-styled frontend (issue #8), served alongside /radio
        // from the same backend/API. Unlike /radio, this frontend has no
        // embedded C++ fallback copy - if LittleFS is unavailable, /radio
        // still works via its own fallback, and /classic-radio simply 404s
        // (see headunit/docs/issue-8-classic-radio-frontend-plan.md for the
        // rationale: doubling the maintenance of a whole second themed UI as
        // raw C++ strings wasn't judged worth it).
        _server->serveStatic("/classic-radio/classic-radio.css", LittleFS, "/classic-radio/classic-radio.css");
        _server->serveStatic("/classic-radio/classic-radio.js", LittleFS, "/classic-radio/classic-radio.js");
        _server->serveStatic("/classic-radio", LittleFS, "/classic-radio/index.html").setDefaultFile("/classic-radio/index.html");
    }

    _server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/radio");
    });

    _server->on("/radio", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", _getRadioIndexHtml());
    });

    _server->on("/radio/station", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", _getStationHtml());
    });

    _server->on("/radio/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/css", _getStyleCss());
    });

    _server->on("/radio/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/javascript", _getAppJs());
    });
}

void WebServer::broadcastStatus(const RadioStatus& status) {
    if (_events && _events->count() > 0) {
        String json = _statusToJson(status);
        _events->send(json.c_str(), "status");
    }
}

void WebServer::broadcastStationList(const RadioStation* stations, size_t count) {
    if (_events && _events->count() > 0) {
        String json = _stationListToJson(stations, count);
        _events->send(json.c_str(), "stations");
    }
}

void WebServer::broadcastScanProgress(uint8_t progress, uint8_t count, bool scanning) {
    if (_events && _events->count() > 0) {
        String json = _scanProgressToJson(progress, count, scanning);
        _events->send(json.c_str(), "scan");
    }
}

String WebServer::_statusToJson(const RadioStatus& status) {
    JsonDocument doc;
    doc["state"] = static_cast<int>(status.state);
    doc["frequency"] = status.frequency;
    doc["frequencyMHz"] = status.frequency / 100.0;
    doc["programService"] = status.programService;
    doc["radioText"] = status.radioText;
    doc["rssi"] = status.rssi;
    doc["stereo"] = status.stereo;
    // Convert hardware volume (0-15) to percent (0-100)
    doc["volume"] = (status.volume * 100) / 15;
    doc["volumeRaw"] = status.volume;
    doc["muted"] = status.muted;
    doc["scanProgress"] = status.scanProgress;
    doc["scanCount"] = status.scanCount;

    // RDS detail fields (issue #3 "detailed view of rds texts").
    JsonObject rds = doc["rds"].to<JsonObject>();
    rds["synced"] = status.rdsSynced;
    char piHex[7];
    snprintf(piHex, sizeof(piHex), "0x%04X", status.rdsPiCode);
    rds["piCode"] = piHex;
    rds["pty"] = status.rdsPty;
    rds["ptyName"] = rdsProgramTypeName(status.rdsPty);
    rds["tp"] = status.rdsTp;
    rds["ta"] = status.rdsTa;
    JsonObject bler = rds["bler"].to<JsonObject>();
    bler["a"] = status.rdsBlerA;
    bler["b"] = status.rdsBlerB;
    bler["c"] = status.rdsBlerC;
    bler["d"] = status.rdsBlerD;
    rds["lastUpdateMs"] = status.lastRdsUpdateMs;

    String json;
    serializeJson(doc, json);
    return json;
}

String WebServer::_stationListToJson(const RadioStation* stations, size_t count) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["frequency"] = stations[i].frequency;
        obj["frequencyMHz"] = stations[i].frequency / 100.0;
        obj["programService"] = stations[i].programService;
        obj["radioText"] = stations[i].radioText;
        obj["rssi"] = stations[i].rssi;
        obj["stereo"] = stations[i].stereo;
        obj["favorite"] = stations[i].favorite;
    }
    String json;
    serializeJson(doc, json);
    return json;
}

String WebServer::_scanProgressToJson(uint8_t progress, uint8_t count, bool scanning) {
    JsonDocument doc;
    doc["progress"] = progress;
    doc["count"] = count;
    doc["scanning"] = scanning;
    String json;
    serializeJson(doc, json);
    return json;
}

void WebServer::_addCorsHeaders(AsyncWebServerResponse* response) {
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

// HTML/CSS/JS content
const char* WebServer::_getRadioIndexHtml() {
    return R"html(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
    <title>Opel Radio</title>
    <link rel="stylesheet" href="/radio/style.css">
</head>
<body>
    <div class="app">
        <header class="header">
            <h1>Opel Radio</h1>
            <div class="header-buttons">
                <button id="settings-btn" class="btn btn-secondary" title="Einstellungen">⚙</button>
            </div>
        </header>

        <div class="status-indicators">
            <span id="wifi-status" class="status-badge">Init...</span>
            <span id="radio-state" class="status-badge">Off</span>
        </div>

        <main class="main">
            <!-- RDS Detail Section -->
            <section class="rds-section">
                <div class="rds-head">
                    <span id="rds-ps" class="rds-ps">--</span>
                    <span id="rds-sync" class="status-badge">RDS --</span>
                </div>
                <div id="rds-rt" class="rds-rt">--</div>
                <div class="rds-meta">
                    <span id="rds-pty">PTY: --</span>
                    <span id="rds-pi">PI: --</span>
                    <span id="rds-tp" class="rds-flag">TP</span>
                    <span id="rds-ta" class="rds-flag">TA</span>
                </div>
                <div class="rds-bler">
                    <span>BLER</span>
                    <span id="rds-bler-a" class="bler-dot" title="Block A"></span>
                    <span id="rds-bler-b" class="bler-dot" title="Block B"></span>
                    <span id="rds-bler-c" class="bler-dot" title="Block C"></span>
                    <span id="rds-bler-d" class="bler-dot" title="Block D"></span>
                </div>
            </section>

            <!-- Scan Section -->
            <section class="scan-section">
                <div class="scan-buttons">
                    <button id="scan-btn" class="btn btn-primary">Scan starten</button>
                    <button id="cancel-scan-btn" class="btn btn-danger hidden">Abbrechen</button>
                </div>
                <div id="scan-progress" class="progress-container" style="display:none;">
                    <div class="progress-bar">
                        <div id="scan-bar" class="progress-fill"></div>
                    </div>
                    <span id="scan-text">0% - 0 Sender</span>
                </div>
            </section>

            <!-- Stations Section -->
            <section class="stations-section">
                <h2>Sender</h2>
                <ul id="station-list" class="station-list"></ul>
            </section>

            <!-- Volume Section -->
            <section class="volume-section">
                <label for="volume">Lautstärke: <span id="volume-value">50</span>%</label>
                <input type="range" id="volume" min="0" max="100" value="50" class="volume-slider">
            </section>
        </main>

        <footer class="footer">
            <button id="mute-btn" class="btn btn-secondary">Mute</button>
        </footer>
    </div>

    <!-- Settings Modal -->
    <div id="settings-modal" class="modal">
        <div class="modal-content">
            <button class="modal-close" onclick="closeSettings()">&times;</button>
            <h2 class="modal-title">Einstellungen</h2>
            <div class="modal-section">
                <label class="modal-label">WiFi SSID:</label>
                <input type="text" id="wifi-ssid" class="modal-input" placeholder="SSID">
            </div>
            <div class="modal-section">
                <label class="modal-label">WiFi Password:</label>
                <input type="password" id="wifi-pass" class="modal-input" placeholder="Password">
            </div>
            <div class="modal-section" style="display:flex;gap:8px">
                <button class="btn btn-primary" onclick="saveSettings()" style="flex:1">Speichern</button>
                <button class="btn btn-secondary" onclick="closeSettings()" style="flex:1">Abbrechen</button>
            </div>
        </div>
    </div>

    <script src="/radio/app.js"></script>
</body>
</html>
)html";
}



const char* WebServer::_getStationHtml() {
    return R"html(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>Opel Radio - Sender</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: sans-serif; background: #0a0a0a; color: #e0e0e0; }
    .app { display: flex; flex-direction: column; min-height: 100vh; }
    .header { padding: 16px; background: #121212; border-bottom: 1px solid #2a2a2a; }
    .main { flex: 1; padding: 16px; overflow-y: auto; text-align: center; }
    .footer { padding: 16px; border-top: 1px solid #2a2a2a; background: #121212; }
    button { padding: 12px 16px; margin: 4px; border: 1px solid #2a2a2a; background: #121212; color: #e0e0e0; cursor: pointer; }
    input[type="range"] { width: 100%; }
    .frequency { font-size: 2.5rem; font-weight: bold; color: #00cc44; margin: 16px 0; }
    .station-info { text-align: center; }
    .seek-section { display: flex; gap: 12px; margin-top: 24px; }
    .seek-section button { flex: 1; }
  </style>
</head>
<body>
  <div class="app">
    <header class="header">
      <a href="/radio">← Zurück</a>
      <h1 id="station-name">Sender</h1>
    </header>

    <main class="main station-view">
      <div class="station-info">
        <div class="frequency" id="frequency">--.-- MHz</div>
        <div id="radiotext">--</div>
      </div>

      <section class="volume-section">
        <label for="volume">Lautstärke</label>
        <input type="range" id="volume" min="0" max="100" value="50" class="volume-slider">
        <span id="volume-value">50%</span>
        <button id="mute-btn">Mute</button>
      </section>

      <section class="seek-section">
        <button id="seek-down">◀◀ Seek -</button>
        <button id="seek-up">Seek + ▶▶</button>
      </section>
    </main>
  </div>
  <script>
    (function() {
      var link = document.createElement('link');
      link.rel = 'stylesheet';
      link.href = '/radio/style.css';
      document.head.appendChild(link);
    })();
  </script>
  <script src="/radio/app.js"></script>
</body>
</html>
)html";
}

const char* WebServer::_getStyleCss() {
    return R"css(
/* Root colors */
:root {
    --bg: #0a0a0a;
    --fg: #e0e0e0;
    --accent: #00cc44;
    --border: #2a2a2a;
}

/* Base styles */
* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

body {
    font-family: sans-serif;
    background: var(--bg);
    color: var(--fg);
    display: flex;
    flex-direction: column;
    min-height: 100vh;
}

/* Layout */
.app {
    display: flex;
    flex-direction: column;
    min-height: 100vh;
    width: 100%;
}

.header {
    padding: 16px;
    background: #121212;
    border-bottom: 1px solid var(--border);
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.header h1 {
    color: var(--accent);
    font-weight: 600;
    margin: 0;
}

.header-buttons {
    display: flex;
    gap: 8px;
}

.header .btn {
    padding: 8px 12px;
    font-size: 0.85rem;
    min-height: auto;
}

.status-indicators {
    padding: 0 16px;
    display: flex;
    gap: 12px;
}

.status-badge {
    font-size: 0.7rem;
    padding: 4px 8px;
    background: var(--border);
    border-radius: 4px;
}

.status-badge.connected {
    background: var(--accent);
    color: #000;
}

.main {
    flex: 1;
    padding: 16px;
    overflow-y: auto;
}

.footer {
    padding: 16px;
    border-top: 1px solid var(--border);
    text-align: center;
}

/* Buttons */
.btn {
    padding: 14px 24px;
    border: none;
    border-radius: 8px;
    font-weight: 600;
    cursor: pointer;
    min-height: 48px;
}

.btn-primary {
    background: var(--accent);
    color: #000;
}

.btn-secondary {
    background: #121212;
    color: var(--fg);
    border: 1px solid var(--border);
}

.btn-danger {
    background: #c41e3a;
    color: #fff;
}

/* Sections */
.scan-section {
    margin-bottom: 24px;
}

.scan-buttons {
    display: flex;
    gap: 8px;
}

.scan-buttons .btn {
    flex: 1;
}

.progress-container {
    margin-top: 16px;
}

.progress-bar {
    height: 8px;
    background: var(--border);
    border-radius: 4px;
    overflow: hidden;
}

.progress-fill {
    height: 100%;
    background: var(--accent);
    width: 0%;
    transition: width 0.3s;
}

#scan-text {
    display: block;
    margin-top: 8px;
    font-size: 0.9rem;
    color: #888;
}

/* Stations */
.stations-section h2 {
    color: var(--accent);
    margin-bottom: 12px;
}

.station-list {
    list-style: none;
}

.station-item {
    display: block;
    padding: 16px;
    background: #121212;
    border: 1px solid var(--border);
    border-radius: 10px;
    margin-bottom: 10px;
    cursor: pointer;
    color: var(--fg);
    transition: all 0.2s;
    text-decoration: none;
}

.station-item:hover {
    border-color: var(--accent);
}

.station-item.current {
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent);
}

.station-name {
    font-weight: 600;
    margin-bottom: 4px;
}

.station-head {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
}

.station-nudge {
    display: flex;
    gap: 6px;
    flex-shrink: 0;
}

.nudge-btn {
    min-width: 36px;
    height: 36px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: #0a0a0a;
    color: var(--accent);
    font-size: 1.1rem;
    font-weight: 700;
    cursor: pointer;
    line-height: 1;
    padding: 0 10px;
}

.nudge-btn:active {
    background: var(--accent);
    color: #000;
}

.station-meta {
    display: flex;
    gap: 12px;
    font-size: 0.85rem;
    color: #888;
}

/* Volume */
.volume-section {
    margin-top: 24px;
    padding: 16px;
    background: #121212;
    border: 1px solid var(--border);
    border-radius: 10px;
}

.volume-section label {
    display: block;
    margin-bottom: 8px;
    font-weight: 600;
}

.volume-slider {
    width: 100%;
    height: 8px;
    background: var(--border);
    border-radius: 4px;
    outline: none;
    -webkit-appearance: none;
    appearance: none;
}

.volume-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 16px;
    height: 16px;
    background: var(--accent);
    cursor: pointer;
    border-radius: 50%;
}

.volume-slider::-moz-range-thumb {
    width: 16px;
    height: 16px;
    background: var(--accent);
    cursor: pointer;
    border-radius: 50%;
    border: none;
}

/* Modal */
.modal {
    display: none;
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background: rgba(0, 0, 0, 0.8);
    z-index: 1000;
    align-items: center;
    justify-content: center;
}

.modal.show {
    display: flex;
}

.modal-content {
    background: #121212;
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 24px;
    max-width: 90%;
    max-height: 80vh;
    overflow: auto;
    width: 400px;
}

.modal-title {
    color: var(--accent);
    font-weight: 600;
    margin-bottom: 16px;
    font-size: 1.2rem;
}

.modal-close {
    float: right;
    cursor: pointer;
    font-size: 1.5rem;
    color: var(--fg);
    background: none;
    border: none;
    padding: 0;
}

.modal-section {
    margin-bottom: 16px;
}

.modal-label {
    display: block;
    margin-bottom: 8px;
    font-weight: 600;
}

.modal-input {
    width: 100%;
    padding: 8px;
    background: #0a0a0a;
    border: 1px solid var(--border);
    color: var(--fg);
    border-radius: 4px;
}

/* RDS detail section */
.rds-section {
    margin-bottom: 24px;
    padding: 16px;
    background: #121212;
    border: 1px solid var(--border);
    border-radius: 10px;
}

.rds-head {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
}

.rds-ps {
    font-weight: 600;
    font-size: 1.1rem;
    color: var(--accent);
}

.rds-rt {
    margin-top: 8px;
    font-size: 0.9rem;
    color: #ccc;
    min-height: 1.2em;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
}

.rds-meta {
    display: flex;
    gap: 12px;
    margin-top: 10px;
    font-size: 0.8rem;
    color: #888;
    flex-wrap: wrap;
    align-items: center;
}

.rds-flag {
    padding: 2px 6px;
    border: 1px solid var(--border);
    border-radius: 4px;
    opacity: 0.35;
}

.rds-flag.active {
    opacity: 1;
    color: var(--accent);
    border-color: var(--accent);
}

.rds-bler {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-top: 10px;
    font-size: 0.8rem;
    color: #888;
}

.bler-dot {
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background: #444;
    display: inline-block;
}

.bler-dot.bler-0 { background: var(--accent); }
.bler-dot.bler-1 { background: #cca300; }
.bler-dot.bler-2 { background: #cc7a00; }
.bler-dot.bler-3 { background: #c41e3a; }

/* Utilities */
.hidden {
    display: none !important;
}
)css";
}



const char* WebServer::_getAppJs() {
    return R"js(
const API = '/api/radio';
const EVENTS = '/api/radio/events';

let eventSource = null;
let currentFreq = 0;
let isScanning = false;

// Initialize on page load
function init() {
    document.getElementById('wifi-status').textContent = 'Loading...';
    setupControls();
    loadInitialData();
    try {
        connectSSE();
    } catch (e) {
        console.error('SSE init failed:', e);
    }
}

// Connect to Server-Sent Events stream
function connectSSE() {
    eventSource = new EventSource(EVENTS);
    
    eventSource.onopen = function() {
        const el = document.getElementById('wifi-status');
        if (el) {
            el.textContent = 'Verbunden';
            el.classList.add('connected');
        }
    };
    
    eventSource.addEventListener('status', (e) => {
        const s = JSON.parse(e.data);
        document.getElementById('radio-state').textContent = 
            ['Off', 'Idle', 'Tuning', 'Seeking', 'Scanning'][s.state] || '?';
        updateFreq(s.frequency);
        updateVolDisplay(s.volume);
        updateRds(s);

        if (document.getElementById('mute-btn')) {
            document.getElementById('mute-btn').textContent = 
                s.muted ? 'Unmute' : 'Mute';
        }
    });
    
    eventSource.addEventListener('stations', (e) => {
        renderStations(JSON.parse(e.data));
    });
    
    eventSource.addEventListener('scan', (e) => {
        try {
            const scan = JSON.parse(e.data);
            isScanning = scan.scanning;
            updateScanUI(scan.progress, scan.count);
        } catch (e) {}
    });
    
    eventSource.onerror = () => {
        const el = document.getElementById('wifi-status');
        if (el) el.textContent = 'Getrennt';
        setTimeout(connectSSE, 5000);
    };
}

// Load initial status and stations from server
function loadInitialData() {
    fetch(`${API}/status`)
        .then(r => r.json())
        .then(s => {
            updateFreq(s.frequency);
            updateVolDisplay(s.volume);
            updateRds(s);
        })
        .catch(e => console.error('status fetch error:', e));
    
    fetch(`${API}/stations`)
        .then(r => r.json())
        .then(renderStations)
        .catch(e => console.error('stations fetch error:', e));
}

// Setup button click handlers
function setupControls() {
    console.log('[RADIO] binding buttons');
    
    // Scan button
    const scanBtn = document.getElementById('scan-btn');
    if (scanBtn) {
        scanBtn.onclick = () => {
            isScanning = true;
            updateScanUI(0, 0);
            fetch(`${API}/scan/start`, { method: 'POST' })
                .catch(console.error);
        };
        console.log('✓ scan-btn bound');
    } else {
        console.error('✗ scan-btn NOT FOUND');
    }
    
    // Cancel scan button
    const cancelBtn = document.getElementById('cancel-scan-btn');
    if (cancelBtn) {
        cancelBtn.onclick = () => {
            fetch(`${API}/scan/cancel`, { method: 'POST' })
                .catch(console.error);
        };
        console.log('✓ cancel-scan-btn bound');
    }
    
    // Volume slider
    const volSlider = document.getElementById('volume');
    if (volSlider) {
        volSlider.onchange = (e) => {
            const percent = parseInt(e.target.value);
            console.log('[VOLUME] User set to ' + percent + '%');
            fetch(`${API}/volume`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ volume: percent })
            }).catch(console.error);
        };
        console.log('✓ volume slider bound');
    } else {
        console.error('✗ volume slider NOT FOUND');
    }
    
    // Mute button
    const muteBtn = document.getElementById('mute-btn');
    if (muteBtn) {
        muteBtn.onclick = () => {
            const muted = muteBtn.textContent === 'Mute';
            fetch(`${API}/mute`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ muted: muted })
            }).catch(console.error);
        };
        console.log('✓ mute-btn bound');
    } else {
        console.error('✗ mute-btn NOT FOUND');
    }
    
    // Settings button
    const settingsBtn = document.getElementById('settings-btn');
    if (settingsBtn) {
        settingsBtn.onclick = openSettings;
        console.log('✓ settings-btn bound');
    }
}

// Settings modal
function openSettings() {
    document.getElementById('settings-modal').classList.add('show');
}

function closeSettings() {
    document.getElementById('settings-modal').classList.remove('show');
}

function saveSettings() {
    const ssid = document.getElementById('wifi-ssid').value;
    const pass = document.getElementById('wifi-pass').value;
    console.log('Settings saved (placeholder)', ssid, pass);
    closeSettings();
}

// Update current frequency display
function updateFreq(freq) {
    currentFreq = freq;
}

// Update volume display (0-100%)
function updateVolDisplay(percent) {
    const el = document.getElementById('volume-value');
    const slider = document.getElementById('volume');
    
    if (el) {
        el.textContent = percent + '%';
    }
    
    if (slider && slider.value != percent) {
        slider.value = percent;
    }
}

// Update scan progress UI
function updateScanUI(progress, count) {
    const scanBtn = document.getElementById('scan-btn');
    const cancelBtn = document.getElementById('cancel-scan-btn');
    const progressDiv = document.getElementById('scan-progress');
    
    if (isScanning) {
        if (scanBtn) scanBtn.classList.add('hidden');
        if (cancelBtn) cancelBtn.classList.remove('hidden');
        if (progressDiv) progressDiv.style.display = 'block';
    } else {
        if (scanBtn) scanBtn.classList.remove('hidden');
        if (cancelBtn) cancelBtn.classList.add('hidden');
        if (progressDiv) progressDiv.style.display = 'none';
    }
    
    if (progressDiv) {
        document.getElementById('scan-bar').style.width = progress + '%';
        document.getElementById('scan-text').textContent = progress + '% - ' + count + ' Sender';
    }
}

// Render station list
function renderStations(stations) {
    const list = document.getElementById('station-list');
    if (!list) return;
    
    list.innerHTML = (stations || [])
        .map(st => {
            const isCurrentStation = st.frequency === currentFreq;
            const stationName = st.programService || ((st.frequency / 100).toFixed(2) + ' MHz');
            const freq = (st.frequency / 100).toFixed(2);
            const stereo = st.stereo ? 'Stereo' : 'Mono';
            const rssi = 'RSSI ' + st.rssi;
            
            // +/- shift buttons tune to the station, then fine-shift by 0.1 MHz
            return `<div class="station-item${isCurrentStation ? ' current' : ''}" onclick="selectStation(${st.frequency})">
                <div class="station-head">
                    <div class="station-name">${stationName}</div>
                    <div class="station-nudge">
                        <button class="nudge-btn" onclick="event.stopPropagation();nudgeFrom(${st.frequency},-100)">−</button>
                        <button class="nudge-btn" onclick="event.stopPropagation();nudgeFrom(${st.frequency},100)">+</button>
                    </div>
                </div>
                <div class="station-meta">
                    <span>${freq} MHz</span>
                    <span>${stereo}</span>
                    <span>${rssi}</span>
                </div>
            </div>`;
        })
        .join('');
}

// Update the RDS detail section (PS/RT/PTY/PI/TP/TA/BLER) from a status payload
function updateRds(s) {
    const psEl = document.getElementById('rds-ps');
    if (psEl) psEl.textContent = (s.programService && s.programService.trim()) || '--';

    const rtEl = document.getElementById('rds-rt');
    if (rtEl) rtEl.textContent = (s.radioText && s.radioText.trim()) || '--';

    const rds = s.rds || {};

    const syncEl = document.getElementById('rds-sync');
    if (syncEl) {
        syncEl.textContent = rds.synced ? 'RDS Sync' : 'RDS --';
        syncEl.classList.toggle('connected', !!rds.synced);
    }

    const ptyEl = document.getElementById('rds-pty');
    if (ptyEl) ptyEl.textContent = 'PTY: ' + (rds.ptyName || '--');

    const piEl = document.getElementById('rds-pi');
    if (piEl) piEl.textContent = 'PI: ' + (rds.piCode || '--');

    const tpEl = document.getElementById('rds-tp');
    if (tpEl) tpEl.classList.toggle('active', !!rds.tp);

    const taEl = document.getElementById('rds-ta');
    if (taEl) taEl.classList.toggle('active', !!rds.ta);

    const bler = rds.bler || {};
    ['a', 'b', 'c', 'd'].forEach((block) => {
        const el = document.getElementById('rds-bler-' + block);
        if (!el) return;
        const level = bler[block];
        el.className = 'bler-dot' + (level !== undefined ? ' bler-' + level : '');
        el.title = 'Block ' + block.toUpperCase() + (level !== undefined ? ' (' + level + ')' : '');
    });
}

// Tune to the given frequency first, then shift by step (e.g. 100 = +0.1 MHz)
function nudgeFrom(baseFreq, step) {
    fetch(`${API}/frequency`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: baseFreq })
    }).then(() => {
        fetch(`${API}/nudge`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ step: step })
        }).catch(console.error);
    }).catch(console.error);
}

// Select and tune to a station
function selectStation(freq) {
    fetch(`${API}/frequency`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: freq })
    }).catch(console.error);
}

// Bind init to DOMContentLoaded
document.addEventListener('DOMContentLoaded', init);
if (document.readyState !== 'loading') {
    init();
}
)js";
}

