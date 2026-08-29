#include "TemperatureSensor.h"

namespace {

const char* statusName(TemperatureSensorStatus s) {
  switch (s) {
    case TemperatureSensorStatus::OK: return "OK";
    case TemperatureSensorStatus::OPEN_CIRCUIT: return "OPEN_CIRCUIT";
    case TemperatureSensorStatus::SHORT_CIRCUIT: return "SHORT_CIRCUIT";
    case TemperatureSensorStatus::OUT_OF_RANGE: return "OUT_OF_RANGE";
  }
  return "?";
}

}  // namespace

TemperatureSensor::TemperatureSensor(uint8_t adcPin, float pullupOhm, float supplyVoltage)
    : adcPin_(adcPin), pullupOhm_(pullupOhm), supplyVoltage_(supplyVoltage) {}

void TemperatureSensor::begin() {
  // GPIO34 (ADC1) is input-only; it has no internal pull-up, which is exactly
  // what the external 510-ohm divider requires. 11 dB attenuation -> ~0..3.3 V
  // full-scale; analogReadMilliVolts() returns calibrated mV.
  analogReadResolution(12);
  analogSetPinAttenuation(adcPin_, ADC_11db);
  LOG.printf("[TEMP] ADC pin=%u pullup=%.1fohm supply=%.2fV\n",
             adcPin_, pullupOhm_, supplyVoltage_);
}

void TemperatureSensor::update() {
  // Accumulate a batch of raw + mV samples per call. update() is called every
  // loop() iteration, so a batch collects within a few iterations -> several
  // fixes per second, EMA-smoothed over a long time constant (slow temp).
  sampleAccumMv_ += analogReadMilliVolts(adcPin_);
  sampleAccumRaw_ += analogRead(adcPin_);
  sampleCount_++;

  if (sampleCount_ < kSamplesPerBatch) return;

  const float avgMv = static_cast<float>(sampleAccumMv_) / sampleCount_;
  const uint16_t avgRaw = static_cast<uint16_t>(sampleAccumRaw_ / sampleCount_);
  sampleAccumMv_ = 0;
  sampleAccumRaw_ = 0;
  sampleCount_ = 0;
  rawAdcAvg_ = avgRaw;

  const float avgV = avgMv / 1000.0f;
  if (filterInitialized_) {
    filteredVoltage_ = kEmaAlpha * avgV + (1.0f - kEmaAlpha) * filteredVoltage_;
  } else {
    filteredVoltage_ = avgV;
    filterInitialized_ = true;
  }

  evaluate();

  // Compact commissioning output, ~1 fix/sec (EMA + batch => ~a few Hz max,
  // throttled further to 1 s).
  const uint32_t now = millis();
  if (now - lastDebugMs_ >= 1000) {
    lastDebugMs_ = now;
    LOG.printf(
        "[TEMP] raw=%u voltage=%.3fV resistance=%.1fohm temp=%.1fC status=%s\n",
        rawAdcAvg_, filteredVoltage_, resistance_, temperature_, statusName(status_));
  }
}

void TemperatureSensor::evaluate() {
  status_ = classifyVoltage(filteredVoltage_, pullupOhm_, supplyVoltage_);
  if (status_ == CoolantSensorStatus::OK) {
    resistance_ = voltageToResistance(filteredVoltage_, pullupOhm_, supplyVoltage_);
    temperature_ = resistanceToTemperature(resistance_);
  }
  // On a fault we deliberately leave resistance_/temperature_ at their last
  // good values (or 0 before the first fix); isValid() flags the fault so the
  // caller must not treat the value as a live reading.
}

float TemperatureSensor::getTemperatureC() const { return temperature_; }
float TemperatureSensor::getResistanceOhm() const { return resistance_; }
float TemperatureSensor::getVoltage() const { return filteredVoltage_; }
uint16_t TemperatureSensor::getRawAdc() const { return rawAdcAvg_; }
bool TemperatureSensor::isValid() const { return status_ == CoolantSensorStatus::OK; }
TemperatureSensorStatus TemperatureSensor::getStatus() const { return status_; }