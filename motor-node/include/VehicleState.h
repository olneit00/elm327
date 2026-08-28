#pragma once

#include <stdint.h>

struct VehicleState {
  uint16_t rpm = 750;
  float coolantTemperature = 82.0f;
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
