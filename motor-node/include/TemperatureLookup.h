#pragma once
//
// TemperatureLookup — hardware-independent math for the engine coolant
// temperature (ECT) sensor: resistance characteristic + voltage-divider
// helpers + fault classification.
//
// Kept free of any Arduino/ADC dependency so the conversion can be unit
// tested on the host (native env). The ESP32-dependent ADC sampling lives in
// TemperatureSensor.cpp; it only *consumes* these pure functions.
//
// Sensor wiring:
//     3.3 V -- 510 R ----+--- ADC pin ---- ECT sensor ---- GND
//                       (measure point)
//   R_sensor = R_pullup * V_adc / (V_supply - V_adc)
//
#include <stddef.h>

// ECT resistance characteristic, measured on the actual sensor(s):
//   20 C -> 550 Ω,  100 C -> 54 Ω  (both measured)
// Fits an NTC with β ≈ 3174 (550 Ω @ 20 C, 54 Ω @ 100 C). All other points
// are derived from that β so the whole table is consistent; the 5 % spec
// tolerance applies per point. Resistance falls monotonically as temperature
// rises.
struct TemperaturePoint {
  float temperatureC;
  float resistanceOhm;
};

constexpr TemperaturePoint TEMP_TABLE[] = {
    {20.0f, 550.0f},   {30.0f, 384.8f},  {40.0f, 275.5f}, {50.0f, 201.3f},
    {60.0f, 149.9f},   {70.0f, 113.6f},  {80.0f, 87.4f},  {90.0f, 68.3f},
    {100.0f, 54.0f},   {110.0f, 43.3f},  {120.0f, 35.0f}, {130.0f, 28.7f},
    {140.0f, 23.7f},   {150.0f, 19.8f},  {160.0f, 16.6f}, {170.0f, 14.1f},
};
constexpr size_t TEMP_TABLE_COUNT = sizeof(TEMP_TABLE) / sizeof(TEMP_TABLE[0]);

// Temperature covered by TEMP_TABLE (20..170 C).
constexpr float TEMP_TABLE_MIN_C = TEMP_TABLE[0].temperatureC;
constexpr float TEMP_TABLE_MAX_C = TEMP_TABLE[TEMP_TABLE_COUNT - 1].temperatureC;

// Fault / validity states for the sensor reading.
enum class CoolantSensorStatus {
  OK,
  OPEN_CIRCUIT,    // ADC near V_supply (sensor disconnected / wire broken)
  SHORT_CIRCUIT,   // ADC near GND (sensor shorted to ground)
  OUT_OF_RANGE     // resistance outside the covered 20..170 C table range
};

// True when the resistance lies inside the table's covered range
// (TEMP_TABLE[last].resistanceOhm <= ohms <= TEMP_TABLE[0].resistanceOhm),
// i.e. it can be mapped to a temperature in 20..170 C.
bool tempResistanceInRange(float resistanceOhm);

// Resistance -> temperature by linear interpolation between the two
// surrounding table points (characteristic is monotonic falling, so look for
// the interval with TEMP_TABLE[i].r >= R >= TEMP_TABLE[i+1].r).
//
// Out-of-table values are NOT extrapolated: they are clamped to the nearest
// table edge (20 C for R > 550, 170 C for R < 14.1). Callers must check
// tempResistanceInRange() (or the sensor's status) before treating the result
// as a plausible reading.
float resistanceToTemperature(float resistanceOhm);

// Voltage-divider math (independent of the ADC implementation):
//   R_sensor = R_pullup * V_adc / (V_supply - V_adc)
// Degenerate inputs (V_adc at/above supply, at/below 0) are pinned to safe
// sentinel values so the caller can rely on classifyVoltage() instead.
float voltageToResistance(float voltageV, float pullupOhm, float supplyVoltageV);

// Classify an ADC voltage into a sensor fault status.
//   V near supply      -> OPEN_CIRCUIT
//   V near GND         -> SHORT_CIRCUIT
//   otherwise:
//     resistance in table range -> OK
//     resistance outside table  -> OUT_OF_RANGE
CoolantSensorStatus classifyVoltage(float voltageV, float pullupOhm, float supplyVoltageV);