#include <Arduino.h>
#include "log/LogTail.h"

#include "Elm327Server.h"
#include "GpsReceiver.h"
#include "GpsWebServer.h"
#include "TemperatureSensor.h"
#include "VehicleState.h"

namespace {
constexpr uint16_t kTcpPort = 35000;
const char* kAccessPointSsid = "Opel1935";
// An empty password creates an open (unencrypted) AP that anyone in range
// can join and use to control this emulator or sniff traffic. WPA2 requires
// at least 8 characters (enforced in Elm327Server::begin()).
const char* kAccessPointPassword = "Opel1935emu";

// GPS module wiring (u-blox NEO-8M over UART2). Classic ESP32 UART2 default
// pins; free pins can be substituted here.
constexpr uint8_t kGpsRxPin = 16;  // module TXD -> ESP32 GPIO16 (RX)
constexpr uint8_t kGpsTxPin = 17;  // module RXD -> ESP32 GPIO17 (TX)
constexpr uint32_t kGpsBaud = 9600;

// ECT temperature sensor (ADC).
//   GPIO34: ADC1 input, input-only, no internal pull-up -> external 510-ohm
//   pull-up is the only load on the divider. See TemperatureSensor.h.
constexpr uint8_t kTempAdcPin = 34;
constexpr float kTempPullupOhm = 510.0f;
constexpr float kTempSupplyVoltage = 3.3f;

VehicleState vehicleState;
Elm327Server elm327Server(vehicleState, kAccessPointSsid, kAccessPointPassword, kTcpPort);
GpsReceiver gpsReceiver(kGpsRxPin, kGpsTxPin, kGpsBaud);
TemperatureSensor temperatureSensor(kTempAdcPin, kTempPullupOhm, kTempSupplyVoltage);
GpsWebServer gpsWebServer(gpsReceiver, vehicleState, temperatureSensor);
}  // namespace

// Global tee: all LOG.println/printf/print go to both the real Serial and
// LogTail's ring buffer so the debug output can be tailed over the web
// without a serial connection (see log/LogTail.h). Serial.begin() below is
// called directly on the real Serial object.
TeePrint LOG(Serial, LogTail::instance());

void setup() {
  Serial.begin(115200);
  delay(300);

  LOG.println();
  LOG.println(F("ESP32 Opel1935 ELM327 Emulator"));
  LOG.printf("[VEHICLE] initial coolantTemperature=%.2f C\n", vehicleState.coolantTemperature);
  elm327Server.begin();

  // ECT sensor on ADC1, then GPS on UART2, then the web UI over the same AP.
  temperatureSensor.begin();
  gpsReceiver.begin();
  gpsWebServer.begin();
}

void loop() {
  static uint32_t lastRuntimeMs = 0;
  if (millis() - lastRuntimeMs >= 1000) {
    lastRuntimeMs = millis();
    vehicleState.runtimeSec++;
  }

  // Sensor -> central vehicle model. getTemperatureC() holds the last good
  // reading while a fault is active; the validity flag lets ELM327/UI decide
  // whether to trust it.
  temperatureSensor.update();
  vehicleState.coolantTemperature = temperatureSensor.getTemperatureC();
  vehicleState.coolantTemperatureValid = temperatureSensor.isValid();

  elm327Server.update();
  gpsReceiver.update();
  gpsWebServer.loop();
  delay(2);
}
