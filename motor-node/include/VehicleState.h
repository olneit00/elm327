#pragma once

#include <stdint.h>

struct VehicleState {
  uint16_t rpm = 750;
  // Coolant temperature in deg C. Kept at the former emulator default 82.0 as
  // a pre-sensor fallback; once the real TemperatureSensor supplies a fix the
  // field is updated from it and coolantTemperatureValid becomes true.
  float coolantTemperature = 82.0f;
  // True only while the hardware ECT sensor reports a valid (OK) reading.
  // PID 05 / web UI use this to distinguish a real value from the fallback.
  bool coolantTemperatureValid = false;
  float speedKmh = 42.0f;
  float fuelPercent = 30.0f;
  float ignitionAdvanceDeg = 10.0f;
  uint32_t runtimeSec = 0;  // seconds since engine start (fake, ticks in loop())

  uint16_t rpmToObdRaw() const;
  uint8_t coolantToObdRaw() const;
  uint8_t speedToObdRaw() const;
  uint8_t fuelToObdRaw() const;
  uint8_t ignitionAdvanceToObdRaw() const;
  uint16_t runtimeToObdMinutes() const;
};
