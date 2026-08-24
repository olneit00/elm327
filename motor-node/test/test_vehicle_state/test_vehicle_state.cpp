#include <unity.h>

#include "VehicleState.h"

void setUp() {}
void tearDown() {}

// Test 1 + 2: defaults match the documented vehicle state and its conversion.
void test_default_coolant_temperature_is_82() {
  VehicleState state;
  TEST_ASSERT_EQUAL_FLOAT(82.0f, state.coolantTemperature);
}

void test_default_coolant_raw_is_0x7A() {
  VehicleState state;
  TEST_ASSERT_EQUAL_UINT8(0x7A, state.coolantToObdRaw());
}

// Test 4/5/6: OBD-II PID 05 formula (A - 40) at known reference points.
void test_zero_degrees_maps_to_0x28() {
  VehicleState state;
  state.coolantTemperature = 0.0f;
  TEST_ASSERT_EQUAL_UINT8(0x28, state.coolantToObdRaw());
}

void test_minus40_degrees_maps_to_0x00() {
  VehicleState state;
  state.coolantTemperature = -40.0f;
  TEST_ASSERT_EQUAL_UINT8(0x00, state.coolantToObdRaw());
}

void test_100_degrees_maps_to_0x8C() {
  VehicleState state;
  state.coolantTemperature = 100.0f;
  TEST_ASSERT_EQUAL_UINT8(0x8C, state.coolantToObdRaw());
}

// Test 8: changing the field changes the raw output, proving there is no
// hardcoded/cached temperature response anywhere in the conversion path.
void test_changed_temperature_is_reflected_in_raw_value() {
  VehicleState state;
  state.coolantTemperature = 50.0f;
  TEST_ASSERT_EQUAL_UINT8(0x5A, state.coolantToObdRaw());
}

// Fuel level default (PID 2F): raw = fuelPercent * 255 / 100.
void test_default_fuel_percent_is_30() {
  VehicleState state;
  TEST_ASSERT_EQUAL_FLOAT(30.0f, state.fuelPercent);
}

void test_default_fuel_raw_is_0x4D() {
  VehicleState state;
  TEST_ASSERT_EQUAL_UINT8(0x4D, state.fuelToObdRaw());
}

// Timing advance (PID 0E): raw = (deg + 64) * 2.
void test_default_ignition_advance_is_10_degrees() {
  VehicleState state;
  TEST_ASSERT_EQUAL_FLOAT(10.0f, state.ignitionAdvanceDeg);
}

void test_default_ignition_advance_raw_is_0x94() {
  VehicleState state;
  TEST_ASSERT_EQUAL_UINT8(0x94, state.ignitionAdvanceToObdRaw());
}

void test_ignition_advance_zero_degrees_maps_to_0x80() {
  VehicleState state;
  state.ignitionAdvanceDeg = 0.0f;
  TEST_ASSERT_EQUAL_UINT8(0x80, state.ignitionAdvanceToObdRaw());
}

void test_ignition_advance_minus10_degrees_maps_to_0x6C() {
  VehicleState state;
  state.ignitionAdvanceDeg = -10.0f;
  TEST_ASSERT_EQUAL_UINT8(0x6C, state.ignitionAdvanceToObdRaw());
}

void test_ignition_advance_20_degrees_maps_to_0xA8() {
  VehicleState state;
  state.ignitionAdvanceDeg = 20.0f;
  TEST_ASSERT_EQUAL_UINT8(0xA8, state.ignitionAdvanceToObdRaw());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_default_coolant_temperature_is_82);
  RUN_TEST(test_default_coolant_raw_is_0x7A);
  RUN_TEST(test_zero_degrees_maps_to_0x28);
  RUN_TEST(test_minus40_degrees_maps_to_0x00);
  RUN_TEST(test_100_degrees_maps_to_0x8C);
  RUN_TEST(test_changed_temperature_is_reflected_in_raw_value);
  RUN_TEST(test_default_fuel_percent_is_30);
  RUN_TEST(test_default_fuel_raw_is_0x4D);
  RUN_TEST(test_default_ignition_advance_is_10_degrees);
  RUN_TEST(test_default_ignition_advance_raw_is_0x94);
  RUN_TEST(test_ignition_advance_zero_degrees_maps_to_0x80);
  RUN_TEST(test_ignition_advance_minus10_degrees_maps_to_0x6C);
  RUN_TEST(test_ignition_advance_20_degrees_maps_to_0xA8);
  return UNITY_END();
}
