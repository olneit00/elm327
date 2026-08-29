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

// Known ECT resistance characteristic, manufacturer spec ~= 5 % tolerance.
// Resistance falls monotonically as temperature rises.
struct TemperaturePoint {
  float temperatureC;
  float resistanceOhm;
};

constexpr TemperaturePoint TEMP_TABLE[] = {
    {20.0f, 3004.0f},  {30.0f, 1868.0f},  {40.0f, 1198.0f}, {50.0f, 789.0f},
    {60.0f, 522.0f},   {70.0f, 368.0f},   {80.0f, 260.0f},  {90.0f, 187.0f},
    {100.0f, 137.0f},  {110.0f, 102.0f},  {120.0f, 79.2f},  {130.0f, 59.0f},
    {140.0f, 46.0f},   {150.0f, 36.0f},   {160.0f, 29.0f},  {170.0f, 23.0f},
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
// table edge (20 C for R > 3004, 170 C for R < 23). Callers must check
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