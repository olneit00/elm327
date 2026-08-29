#include <unity.h>
#include <math.h>

#include "TemperatureLookup.h"

void setUp() {}
void tearDown() {}

// Exact table points: resistance -> temperature.
void test_exact_table_points() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, resistanceToTemperature(3004.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, resistanceToTemperature(1868.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, resistanceToTemperature(1198.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, resistanceToTemperature(789.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, resistanceToTemperature(522.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, resistanceToTemperature(368.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, resistanceToTemperature(260.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, resistanceToTemperature(187.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, resistanceToTemperature(137.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 110.0f, resistanceToTemperature(102.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, resistanceToTemperature(79.2f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 130.0f, resistanceToTemperature(59.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 140.0f, resistanceToTemperature(46.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 150.0f, resistanceToTemperature(36.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 160.0f, resistanceToTemperature(29.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 170.0f, resistanceToTemperature(23.0f));
}

// Linear interpolation between two adjacent points.
// Between 80 C (260 ohm) and 90 C (187 ohm): 223.5 ohm -> 85 C.
void test_interpolated_between_80_and_90() {
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 85.0f, resistanceToTemperature(223.5f));
}

// Another interpolation check, e.g. between 40 C (1198) and 50 C (789):
// ~40+50% -> 993.5 ohm -> ~45 C.
void test_interpolated_between_40_and_50() {
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 45.0f, resistanceToTemperature(993.5f));
}

// Out-of-table values: clamped to edges, flagged not-in-range.
void test_below_coldest_edge_flag() {
  TEST_ASSERT_TRUE(tempResistanceInRange(3004.0f));
  TEST_ASSERT_FALSE(tempResistanceInRange(5000.0f));   // colder than 20 C
  TEST_ASSERT_EQUAL_FLOAT(20.0f, resistanceToTemperature(5000.0f));  // clamped
}

void test_above_hottest_edge_flag() {
  TEST_ASSERT_TRUE(tempResistanceInRange(23.0f));
  TEST_ASSERT_FALSE(tempResistanceInRange(10.0f));     // hotter than 170 C
  TEST_ASSERT_EQUAL_FLOAT(170.0f, resistanceToTemperature(10.0f));   // clamped
}

// Voltage-divider math: R = R_pullup * V / (V_supply - V).
void test_voltage_to_resistance_midpoint() {
  // V = V_supply/2 -> R = R_pullup
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 510.0f, voltageToResistance(1.65f, 510.0f, 3.3f));
}

void test_voltage_to_resistance_known_sensor() {
  // At ~80 C the sensor is 260 ohm -> V = 3.3 * 260/(260+510) = 3.3*0.3377
  // = 1.1143 V. Compute resistance from that voltage and expect ~260.
  float v = 3.3f * 260.0f / (260.0f + 510.0f);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 260.0f, voltageToResistance(v, 510.0f, 3.3f));
}

// Fault classification from ADC voltage.
void test_short_circuit_near_zero_volts() {
  TEST_ASSERT_EQUAL(CoolantSensorStatus::SHORT_CIRCUIT, classifyVoltage(0.001f, 510.0f, 3.3f));
}

void test_open_circuit_near_supply_volts() {
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OPEN_CIRCUIT, classifyVoltage(3.299f, 510.0f, 3.3f));
}

void test_ok_inside_range() {
  // ~80 C sensor = 260 ohm -> V ~= 1.114 V -> OK
  float v = 3.3f * 260.0f / (260.0f + 510.0f);
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OK, classifyVoltage(v, 510.0f, 3.3f));
}

void test_out_of_range_when_too_cold() {
  // Resistance > 3004 (colder than 20 C): V > 2.82 V but not near supply.
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OUT_OF_RANGE, classifyVoltage(3.0f, 510.0f, 3.3f));
}

void test_out_of_range_when_too_hot() {
  // Resistance < 23 (hotter than 170 C): V below ~0.14 V but above short thresh.
  TEST_ASSERT_EQUAL(CoolantSensorStatus::OUT_OF_RANGE, classifyVoltage(0.10f, 510.0f, 3.3f));
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