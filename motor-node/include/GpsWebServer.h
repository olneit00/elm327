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

class GpsWebServer {
 public:
  GpsWebServer(GpsReceiver& gps, uint16_t port = 80);
  ~GpsWebServer();

  void begin();

 private:
  static String snapshotToJson(const GpsSnapshot& s);

  GpsReceiver& gps_;
  AsyncWebServer server_;
};