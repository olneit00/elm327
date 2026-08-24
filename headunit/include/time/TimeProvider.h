#pragma once
//
// Time abstraction for the head unit.
//
// The clock screen reads time through ITimeSource so the time backing can be
// swapped without touching the UI: a millis-based virtual clock is the
// default, and a future RTC / SNTP source can be added behind the same
// interface.
//
#include <stdint.h>

struct TimeOfDay {
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

class ITimeSource {
 public:
  virtual ~ITimeSource() = default;
  virtual TimeOfDay now() const = 0;
};

// Virtual clock that advances from a seed value using millis(). Only useful
// for development until a real time source (RTC / SNTP) is attached, but it
// keeps the clock screen fully testable on the bench.
class MillisClock : public ITimeSource {
 public:
  MillisClock(TimeOfDay seed = TimeOfDay{0, 0, 0});

  void setSeed(TimeOfDay seed);
  TimeOfDay now() const override;

 private:
  TimeOfDay seed_;
};

// Fixed clock for validating exact hand positions without a running source.
class FixedTimeSource : public ITimeSource {
 public:
  FixedTimeSource(TimeOfDay value = TimeOfDay{0, 0, 0});

  void set(TimeOfDay value);
  TimeOfDay now() const override;

 private:
  TimeOfDay value_;
};