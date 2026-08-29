#pragma once
//
// GpsWebServer — serves the GPS status page, the ELM327/OBD live values page
// and the log tail over the motor-node AP's async web server. Self-contained
// HTML (no LittleFS upload needed).
//
// Endpoints:
//   GET /gps          -> HTML page showing all GNSS values, auto-refreshing
//   GET /vehicle      -> HTML page showing all ELM327/OBD live values
//   GET /log          -> live log tail page
//   GET /api/gps      -> JSON snapshot of GpsSnapshot (chunked client polling)
//   GET /api/vehicle  -> JSON snapshot of VehicleState + raw OBD bytes
//   GET /api/log      -> last log lines as JSON text
//   GET /api/log/events -> SSE live log stream
//   GET /            -> redirect to /gps
//
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "GpsReceiver.h"
#include "TemperatureSensor.h"
#include "VehicleState.h"
#include "log/LogTail.h"

class GpsWebServer {
 public:
  GpsWebServer(GpsReceiver& gps, VehicleState& vehicle, TemperatureSensor& sensor,
               uint16_t port = 80);
  ~GpsWebServer();

  void begin();
  void loop();   // polls LogTail -> pushes new lines to /api/log/events SSE

 private:
  static String snapshotToJson(const GpsSnapshot& s);
  static String vehicleToJson(const VehicleState& v, const TemperatureSensor& sensor);
  void setupLogTail();

  GpsReceiver& gps_;
  VehicleState& vehicle_;
  TemperatureSensor& sensor_;
  AsyncWebServer server_;
  AsyncEventSource* logEvents_ = nullptr;
  uint64_t logLastSeq_ = 0;
  uint32_t logLastPollMs_ = 0;
};