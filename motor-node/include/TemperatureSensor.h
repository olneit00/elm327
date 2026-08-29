#pragma once
//
// TemperatureSensor — ESP32 analog ECT (engine coolant temperature) sensor.
//
// Wiring:
//     3.3 V -- 510 R ----+--- ADC pin (default GPIO34, input-only) --- sensor
//                        |                                            |
//                       GND (sensor case / engine ground)
//
// The external 510-ohm pull-up is the top leg of the divider; the sensor
// resistance is the bottom leg. GPIO34 has no internal pull-up, which is what
// we want (the divider must be the only load).
//
// Pipeline: raw ADC samples -> running average -> slow EMA -> voltage ->
// resistance (pressure-divider) -> temperature (TEMP_TABLE lookup) -> status.
// The math itself (table / interpolation / classification) lives in
// TemperatureLookup.h/.cpp so it is host-testable without ADC hardware.
//
// Fault handling: OPEN_CIRCUIT (V near supply), SHORT_CIRCUIT (V near GND)
// and OUT_OF_RANGE (resistance outside the 20..170 C table) are reported via
// getStatus()/isValid(); the component does NOT hand a fabricated temperature
// out as plausible. Between faults it holds the last good filtered reading so
// consumers can fall back to "last known value".
//
#include <Arduino.h>
#include <stdint.h>

#include "TemperatureLookup.h"
#include "log/LogTail.h"

// Status type alias so callers can use the name suggested by the component
// spec while the mapping itself lives in the hardware-free module.
using TemperatureSensorStatus = CoolantSensorStatus;

class TemperatureSensor {
 public:
  TemperatureSensor(uint8_t adcPin = 34, float pullupOhm = 510.0f,
                    float supplyVoltage = 3.3f);

  void begin();   // ADC setup + priming
  void update();  // non-blocking sample/process step, call from loop()

  float getTemperatureC() const;   // last filtered (may be stale during fault)
  float getResistanceOhm() const;  // filtered resistance (last good during fault)
  float getVoltage() const;        // filtered ADC voltage
  uint16_t getRawAdc() const;      // last averaged raw ADC count
  bool isValid() const;            // getStatus() == OK
  TemperatureSensorStatus getStatus() const;

  // Calibration hook: the measured 3.3-V rail may deviate from the nominal
  // value; V_supply is configurable at construction or via this setter.
  void setSupplyVoltage(float voltage) { supplyVoltage_ = voltage; }

 private:
  void evaluate();  // recompute resistance/temp/status from filteredVoltage_

  uint8_t adcPin_;
  float pullupOhm_;
  float supplyVoltage_;

  // sampling state
  uint32_t sampleAccumMv_ = 0;
  uint32_t sampleAccumRaw_ = 0;
  uint8_t sampleCount_ = 0;
  bool filterInitialized_ = false;

  // filtered/current values
  float filteredVoltage_ = 0.0f;
  float resistance_ = 0.0f;
  float temperature_ = 0.0f;  // 0 until the first valid fix
  uint16_t rawAdcAvg_ = 0;
  TemperatureSensorStatus status_ = CoolantSensorStatus::SHORT_CIRCUIT;  // safe default

  uint32_t lastDebugMs_ = 0;

  static constexpr uint8_t kSamplesPerBatch = 16;
  static constexpr float kEmaAlpha = 0.05f;  // slow, ~1 Hz fix rate
};