#include <Arduino.h>

#include "Elm327Server.h"
#include "VehicleState.h"

namespace {
constexpr uint16_t kTcpPort = 35000;
const char* kAccessPointSsid = "Opel1935";
const char* kAccessPointPassword = "";

VehicleState vehicleState;
Elm327Server elm327Server(vehicleState, kAccessPointSsid, kAccessPointPassword, kTcpPort);
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println(F("ESP32 Opel1935 ELM327 Emulator"));
  Serial.printf("[VEHICLE] initial coolantTemperature=%.2f C\n", vehicleState.coolantTemperature);
  elm327Server.begin();
}

void loop() {
  elm327Server.update();
  delay(2);
}
