#include <Arduino.h>

#include "time/TimeProvider.h"

namespace {

TimeOfDay FromTotalSeconds(uint32_t totalSeconds) {
  TimeOfDay t;
  t.hour = static_cast<uint8_t>((totalSeconds / 3600) % 24);
  t.minute = static_cast<uint8_t>((totalSeconds / 60) % 60);
  t.second = static_cast<uint8_t>(totalSeconds % 60);
  return t;
}

uint32_t ToTotalSeconds(TimeOfDay t) {
  return static_cast<uint32_t>(t.hour) * 3600 +
         static_cast<uint32_t>(t.minute) * 60 + t.second;
}

}  // namespace

MillisClock::MillisClock(TimeOfDay seed) { setSeed(seed); }

void MillisClock::setSeed(TimeOfDay seed) { seed_ = seed; }

TimeOfDay MillisClock::now() const {
  // Elapsed seconds since boot, wrapped onto the seed. millis() wraps after
  // ~49 days; treating the wrap as a modulo break is harmless for a
  // development clock.
  uint32_t total = ToTotalSeconds(seed_) + millis() / 1000;
  return FromTotalSeconds(total);
}

FixedTimeSource::FixedTimeSource(TimeOfDay value) { set(value); }

void FixedTimeSource::set(TimeOfDay value) { value_ = value; }

TimeOfDay FixedTimeSource::now() const { return value_; }