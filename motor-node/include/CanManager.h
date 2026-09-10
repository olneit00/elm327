#pragma once
//
// CanManager — periodically transmits the GpsSnapshot from GpsReceiver onto
// the vehicle CAN bus (ESP32 TWAI controller + external transceiver, e.g.
// TJA1050/MCP2551 or SN65HVD230) so the headunit's CanBusReceiver can pick
// it up. Frame IDs/signals follow elm327.dbc (see repo root).
//
// Hardware: see issue #17 (Hardware-Wiring + CAN-Schaltplan). Default pins
// are ESP32 GPIO4 (TWAI_TX -> transceiver D) / GPIO5 (TWAI_RX <- transceiver
// RX), 500 kBit/s (automotive standard, ISO 11898-1). Both bus ends need a
// 120 Ohm termination resistor.
//
// Thread-safety: begin()/updateAndTransmit() are only ever called from the
// Arduino loop task (mirrors GpsReceiver/TemperatureSensor), so no locking
// is needed here.
//
#include <Arduino.h>
#include <stdint.h>

#include "GpsReceiver.h"

class CanManager {
 public:
  // txPin/rxPin: ESP32 GPIO connected to the CAN transceiver's RX/TX pins
  // respectively (see class comment for the naming — TWAI_TX drives the
  // transceiver's driver input, TWAI_RX reads the transceiver's receiver
  // output). bitrateBps defaults to the automotive-standard 500 kBit/s.
  CanManager(uint8_t txPin, uint8_t rxPin, uint32_t bitrateBps = 500000);

  // Installs and starts the TWAI driver. Logs and returns false on failure
  // (e.g. no transceiver wired yet); the caller can keep running without
  // CAN, matching the "don't crash" requirement in issue #14.
  bool begin();

  // Call once per loop() iteration. Internally rate-limits each CAN
  // message to its own cycle time (see elm327.dbc GenMsgCycleTime) and
  // transmits any that are due from the given snapshot. No-op if begin()
  // failed or was not called.
  void updateAndTransmit(const GpsSnapshot& snap);

  // True once begin() has successfully installed + started the driver.
  bool isReady() const { return ready_; }

  // Running counter of failed twai_transmit() calls, for diagnostics
  // (e.g. surfaced on the GPS web status page).
  uint32_t txErrors() const { return txErrors_; }

 private:
  void transmitPosition(const GpsSnapshot& snap);
  void transmitFix(const GpsSnapshot& snap);
  void transmitVelocity(const GpsSnapshot& snap);
  void transmitTime(const GpsSnapshot& snap);
  void transmitSatellites(const GpsSnapshot& snap);
  void transmitStatus(const GpsSnapshot& snap);

  bool send(uint32_t id, const uint8_t* data, uint8_t len);

  uint8_t txPin_, rxPin_;
  uint32_t bitrateBps_;
  bool ready_ = false;

  uint32_t lastPositionMs_ = 0;
  uint32_t lastFixMs_ = 0;
  uint32_t lastVelocityMs_ = 0;
  uint32_t lastTimeMs_ = 0;
  uint32_t lastSatellitesMs_ = 0;
  uint32_t lastStatusMs_ = 0;

  uint32_t txErrors_ = 0;
};
