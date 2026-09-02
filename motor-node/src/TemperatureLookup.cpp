#include "TemperatureLookup.h"

bool tempResistanceInRange(float resistanceOhm) {
  return resistanceOhm >= TEMP_TABLE[TEMP_TABLE_COUNT - 1].resistanceOhm &&
         resistanceOhm <= TEMP_TABLE[0].resistanceOhm;
}

float resistanceToTemperature(float resistanceOhm) {
  // Outside the covered range: clamp to the nearest table edge (no
  // uncontrolled extrapolation; validity is signalled separately).
  if (resistanceOhm >= TEMP_TABLE[0].resistanceOhm) {
    return TEMP_TABLE[0].temperatureC;
  }
  if (resistanceOhm <= TEMP_TABLE[TEMP_TABLE_COUNT - 1].resistanceOhm) {
    return TEMP_TABLE[TEMP_TABLE_COUNT - 1].temperatureC;
  }

  // Table is descending in resistance; find the segment
  // [i, i+1] with TEMP_TABLE[i].r >= R >= TEMP_TABLE[i+1].r and interpolate
  // linearly between the two adjacent temperatures (running with resistance:
  // R = hi -> tHi, R = lo -> tLo).
  for (size_t i = 0; i + 1 < TEMP_TABLE_COUNT; ++i) {
    const float hi = TEMP_TABLE[i].resistanceOhm;
    const float lo = TEMP_TABLE[i + 1].resistanceOhm;
    if (resistanceOhm <= hi && resistanceOhm >= lo) {
      const float tHi = TEMP_TABLE[i].temperatureC;
      const float tLo = TEMP_TABLE[i + 1].temperatureC;
      const float span = lo - hi;  // negative
      const float frac = (span != 0.0f) ? (resistanceOhm - hi) / span : 0.0f;
      return tHi + frac * (tLo - tHi);
    }
  }

  return TEMP_TABLE[0].temperatureC;  // unreachable
}

float voltageToResistance(float voltageV, float pullupOhm, float supplyVoltageV) {
  const float denom = supplyVoltageV - voltageV;
  if (voltageV >= supplyVoltageV - 0.001f) return 1e6f;  // open-ish sentinel
  if (voltageV <= 0.0f) return 0.0f;                     // shorted sentinel
  if (denom <= 0.0f) return 1e6f;
  return pullupOhm * voltageV / denom;
}

CoolantSensorStatus classifyVoltage(float voltageV, float pullupOhm, float supplyVoltageV) {
  if (voltageV > supplyVoltageV * 0.98f) {
    return CoolantSensorStatus::OPEN_CIRCUIT;
  }
  if (voltageV < 0.05f) {
    return CoolantSensorStatus::SHORT_CIRCUIT;
  }
  return tempResistanceInRange(voltageToResistance(voltageV, pullupOhm, supplyVoltageV))
             ? CoolantSensorStatus::OK
             : CoolantSensorStatus::OUT_OF_RANGE;
}