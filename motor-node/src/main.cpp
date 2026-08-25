#include <Arduino.h>

#include "Elm327Server.h"
#include "VehicleState.h"

namespace {
constexpr uint16_t kTcpPort = 35000;
const char* kAccessPointSsid = "Opel1935";
// An empty password creates an open (unencrypted) AP that anyone in range
// can join and use to control this emulator or sniff traffic. WPA2 requires
// at least 8 characters (enforced in Elm327Server::begin()).
const char* kAccessPointPassword = "Opel1935emu";

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
