#pragma once
//
// GpsWebServer — serves the GPS status page + JSON over the motor-node AP's
// async web server. Self-contained HTML (no LittleFS upload needed).
//
// Endpoints:
//   GET /gps          -> HTML page showing all GNSS values, auto-refreshing
//   GET /api/gps      -> JSON snapshot of GpsSnapshot (chunked client polling)
//   GET /            -> redirect to /gps
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "GpsReceiver.h"
#include "log/LogTail.h"

class GpsWebServer {
 public:
  GpsWebServer(GpsReceiver& gps, uint16_t port = 80);
  ~GpsWebServer();

  void begin();
  void loop();   // polls LogTail -> pushes new lines to /api/log/events SSE

 private:
  static String snapshotToJson(const GpsSnapshot& s);
  void setupLogTail();

  GpsReceiver& gps_;
  AsyncWebServer server_;
  AsyncEventSource* logEvents_ = nullptr;
  uint64_t logLastSeq_ = 0;
  uint32_t logLastPollMs_ = 0;
};