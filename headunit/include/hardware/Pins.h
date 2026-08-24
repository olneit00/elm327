#pragma once

#include <stdint.h>

//
// Central pin / parameter definitions for the GC9A01 display on the head unit.
// Keep every hardware constant in one place so wiring changes never require
// editing driver code.
//
namespace pins {

// --- SPI bus (GC9A01) ---
constexpr int SCL_PIN = 18;  // SPI Clock (SCK)
constexpr int SDA_PIN = 23;  // SPI Data (MOSI)
constexpr int CS_PIN = 5;    // Chip Select
constexpr int DC_PIN = 27;   // Data / Command select
constexpr int RST_PIN = 33;  // Reset

// --- I2C bus (Si4703 FM/RDS Tuner) ---
constexpr int I2C_SDA_PIN = 21;  // I2C Data (ESP32 default SDA)
constexpr int I2C_SCL_PIN = 22;  // I2C Clock (ESP32 default SCL)

// --- Si4703 control pins ---
constexpr int RADIO_RST_PIN = 4;   // Hardware reset (active LOW)
constexpr int RADIO_SEN_PIN = -1;  // Not connected (tied to 3.3V on breakout)

// --- CAN bus (for future GALA integration) ---
constexpr int CAN_TX_PIN = 25;  // TWAI TX
constexpr int CAN_RX_PIN = 26;  // TWAI RX

// --- Panel geometry ---
constexpr uint16_t SCREEN_WIDTH = 240;
constexpr uint16_t SCREEN_HEIGHT = 240;

// GC9A01 modules are commonly shipped with inverted color output; keep the
// toggle here so a wiring-only difference does not hide in driver code.
constexpr bool INVERT = true;

}  // namespace pins