#include <unity.h>
#include <math.h>

#include "TemperatureLookup.h"

void setUp() {}
void tearDown() {}

// Exact table points: resistance -> temperature (β≈3174 NTC, 20 C=550 Ω).
void test_exact_table_points() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, resistanceToTemperature(550.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, resistanceToTemperature(384.8f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, resistanceToTemperature(275.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, resistanceToTemperature(201.3f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, resistanceToTemperature(149.9f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, resistanceToTemperature(113.6f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, resistanceToTemperature(87.4f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, resistanceToTemperature(68.3f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, resistanceToTemperature(54.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 110.0f, resistanceToTemperature(43.3f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, resistanceToTemperature(35.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 130.0f, resistanceToTemperature(28.7f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 140.0f, resistanceToTemperature(23.7f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 150.0f, resistanceToTemperature(19.8f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 160.0f, resistanceToTemperature(16.6f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 170.0f, resistanceToTemperature(14.1f));
}

// Linear interpolation between two adjacent points.
// Between 80 C (87.4 ohm) and 90 C (68.3 ohm): midpoint 77.85 ohm -> 85 C.
void test_interpolated_between_80_and_90() {
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 85.0f, resistanceToTemperature(77.85f));
}

// Another interpolation check, between 40 C (275.5) and 50 C (201.3):
// ~45 C -> ~238.4 ohm.
void test_interpolated_between_40_and_50() {
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 45.0f, resistanceToTemperature(238.4f));
}

// Out-of-table values: clamped to edges, flagged not-in-range.
void test_below_coldest_edge_flag() {
  TEST_ASSERT_TRUE(tempResistanceInRange(550.0f));
  TEST_ASSERT_FALSE(tempResistanceInRange(1000.0f));   // colder than 20 C
  TEST_ASSERT_EQUAL_FLOAT(20.0f, resistanceToTemperature(1000.0f));  // clamped
}

void test_above_hottest_edge_flag() {
  TEST_ASSERT_TRUE(tempResistanceInRange(14.1f));
  TEST_ASSERT_FALSE(tempResistanceInRange(10.0f));     // hotter than 170 C
  TEST_ASSERT_EQUAL_FLOAT(170.0f, resistanceToTemperature(10.0f));   // clamped
}

// Voltage-divider math: R = R_pullup * V / (V_supply - V), pull-up = 510 Ω.
void test_voltage_to_resistance_midpoint() {
  // V = V_supply/2 -> R = R_pullup
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 510.0f, voltageToResistance(1.65f, 510.0f, 3.3f));
}

void test_voltage_to_resistance_known_sensor() {
  // At ~80 C the sensor is 87.4 ohm -> V = 3.3 * 87.4 / (87.4+510) = 0.481 V.
  // Compute resistance from that voltage and expect ~87.4.
  float v = 3.3f * 87.4f / (87.4f + 510.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 87.4f, voltageToResistance(v, 510.0f, 3.3f));
}

// Fault classification from ADC voltage.
void test_short_circuit_near_zero_volts() {
  TEST_ASSERT_EQUAL(CoolantSensorStatus::SHORT_CIRCUIT, classifyVoltage(0.001f, 510.0f, 3.3f));
}

void test_open_circuit_near_supply_volts() {
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OPEN_CIRCUIT, classifyVoltage(3.299f, 510.0f, 3.3f));
}

void test_ok_inside_range() {
  // ~80 C sensor = 87.4 ohm -> V ~= 0.481 V -> OK
  float v = 3.3f * 87.4f / (87.4f + 510.0f);
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OK, classifyVoltage(v, 510.0f, 3.3f));
}

void test_out_of_range_when_too_cold() {
  // Resistance > 550 (colder than 20 C): V > 1.71 V but not near supply.
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OUT_OF_RANGE, classifyVoltage(2.0f, 510.0f, 3.3f));
}

void test_out_of_range_when_too_hot() {
  // Resistance < 14.1 (hotter than 170 C): V below ~0.089 V but above short thresh.
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OUT_OF_RANGE, classifyVoltage(0.06f, 510.0f, 3.3f));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_exact_table_points);
  RUN_TEST(test_interpolated_between_80_and_90);
  RUN_TEST(test_interpolated_between_40_and_50);
  RUN_TEST(test_below_coldest_edge_flag);
  RUN_TEST(test_above_hottest_edge_flag);
  RUN_TEST(test_voltage_to_resistance_midpoint);
  RUN_TEST(test_voltage_to_resistance_known_sensor);
  RUN_TEST(test_short_circuit_near_zero_volts);
  RUN_TEST(test_open_circuit_near_supply_volts);
  RUN_TEST(test_ok_inside_range);
  RUN_TEST(test_out_of_range_when_too_cold);
  RUN_TEST(test_out_of_range_when_too_hot);
  return UNITY_END();
}