#pragma once
//
// CanBusReceiver — reads GPS data sent by the motor-node's CanManager over
// the vehicle CAN bus (ESP32 TWAI controller + external transceiver) and
// exposes it as a thread-safe snapshot for the UI. Frame IDs/signals follow
// elm327.dbc (repo root).
//
// Thread-safety: poll() runs on the Arduino loop task and drains the TWAI
// RX queue there (the ESP-IDF TWAI driver itself buffers incoming frames in
// an ISR, so no separate interrupt handler is needed here). getLatestGpsData()
// takes a short mutex, mirroring motor-node's GpsReceiver/GpsSnapshot
// pattern, so any other task (e.g. a future web handler) can read a
// consistent copy.
//
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

// GPS data decoded off the CAN bus. Mirrors motor-node's GpsSnapshot fields
// that are actually carried on the bus (see elm327.dbc); satellite detail is
// limited to PRN/SNR because GPS_Satellites only carries that much per
// frame.
struct CanGpsData {
  bool valid = false;      // GPS_Status.Valid: motor-node ever had a full sentence
  bool activeFix = false;  // GPS_Status.ActiveFix: RMC status A

  double latitude = 0.0;   // deg, +N
  double longitude = 0.0;  // deg, +E

  uint8_t fixQuality = 0;
  uint8_t fixMode = 0;
  uint8_t satellitesInUse = 0;
  uint8_t satellitesInView = 0;
  float hdop = 0.0f;
  float vdop = 0.0f;
  float pdop = 0.0f;

  float speedKmh = 0.0f;
  float courseDeg = 0.0f;
  float altitudeMsl = 0.0f;

  uint8_t utcHour = 0, utcMin = 0, utcSec = 0;
  uint8_t utcDay = 0, utcMonth = 0;
  uint16_t utcYear = 0;
  bool dateValid = false;

  uint16_t checksumErrors = 0;
  uint32_t lineAgeMs = 0;  // motor-node-side GPS line age at last GPS_Status frame

  // millis() (headunit-local clock) of the last CAN frame received for any
  // GPS message; used by the UI to detect a stale/disconnected bus.
  uint32_t lastUpdateMs = 0;
};

class CanBusReceiver {
 public:
  CanBusReceiver(uint8_t txPin, uint8_t rxPin, uint32_t bitrateBps = 500000);
  ~CanBusReceiver();

  // Installs and starts the TWAI driver. Logs and returns false on failure
  // (e.g. no transceiver wired yet); the caller can keep running without
  // CAN, just like motor-node's CanManager.
  bool begin();

  // Call once per loop() iteration; drains any pending CAN frames
  // (non-blocking) and updates the internal snapshot. No-op if begin()
  // failed or was not called.
  void poll();

  // Thread-safe copy of the latest decoded GPS data.
  CanGpsData getLatestGpsData() const;

  bool isReady() const { return ready_; }
  uint32_t rxErrors() const { return rxErrors_; }

 private:
  void handleFrame(uint32_t id, const uint8_t* data, uint8_t len);

  uint8_t txPin_, rxPin_;
  uint32_t bitrateBps_;
  bool ready_ = false;
  uint32_t rxErrors_ = 0;

  CanGpsData data_;  // guarded by mutex_
  mutable SemaphoreHandle_t mutex_ = nullptr;
};
