#include "VehicleState.h"

#include <math.h>

namespace {
uint8_t clampToByte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return static_cast<uint8_t>(value);
}
}  // namespace

uint16_t VehicleState::rpmToObdRaw() const {
  const uint32_t raw = static_cast<uint32_t>(rpm) * 4U;
  if (raw > 0xFFFFU) {
    return 0xFFFFU;
  }
  return static_cast<uint16_t>(raw);
}

uint8_t VehicleState::coolantToObdRaw() const {
  const int raw = static_cast<int>(lroundf(coolantTemperature + 40.0f));
  return clampToByte(raw);
}

uint8_t VehicleState::speedToObdRaw() const {
  const int raw = static_cast<int>(lroundf(speedKmh));
  return clampToByte(raw);
}

uint8_t VehicleState::fuelToObdRaw() const {
  const float normalized = fuelPercent * 255.0f / 100.0f;
  const int raw = static_cast<int>(lroundf(normalized));
  return clampToByte(raw);
}

uint8_t VehicleState::ignitionAdvanceToObdRaw() const {
  const int raw = static_cast<int>(lroundf((ignitionAdvanceDeg + 64.0f) * 2.0f));
  return clampToByte(raw);
}
