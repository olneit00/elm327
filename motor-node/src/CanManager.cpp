#include "CanManager.h"
#include "log/LogTail.h"

#include <driver/twai.h>
#include <math.h>
#include <string.h>

namespace {
// CAN IDs from elm327.dbc (repo root).
constexpr uint32_t kIdGpsPosition = 0x100;
constexpr uint32_t kIdGpsFix = 0x101;
constexpr uint32_t kIdGpsVelocity = 0x102;
constexpr uint32_t kIdGpsTime = 0x103;
constexpr uint32_t kIdGpsSatellites = 0x104;
constexpr uint32_t kIdGpsStatus = 0x105;

// Cycle times per elm327.dbc GenMsgCycleTime attributes.
constexpr uint32_t kCyclePositionMs = 2000;
constexpr uint32_t kCycleFixMs = 2000;
constexpr uint32_t kCycleVelocityMs = 2000;
constexpr uint32_t kCycleTimeMs = 30000;
constexpr uint32_t kCycleSatellitesMs = 5000;
constexpr uint32_t kCycleStatusMs = 5000;

// GPS_Satellites packs 3 satellites (PRN + SNR) per 8-byte frame; see the
// BO_ 260 comment in elm327.dbc for why elevation/azimuth are left out.
constexpr uint8_t kSatsPerFrame = 3;

inline void putI32LE(uint8_t* out, int32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void putU16LE(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void putI16LE(uint8_t* out, int16_t v) { putU16LE(out, static_cast<uint16_t>(v)); }

inline void putU32LE(uint8_t* out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFF);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// Clamp helper so a wild/overflowed reading never wraps into a bogus scaled
// value on the wire.
template <typename T>
inline T clampVal(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

CanManager::CanManager(uint8_t txPin, uint8_t rxPin, uint32_t bitrateBps)
    : txPin_(txPin), rxPin_(rxPin), bitrateBps_(bitrateBps) {}

bool CanManager::begin() {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(txPin_), static_cast<gpio_num_t>(rxPin_),
                                   TWAI_MODE_NORMAL);
  // Only 500 kBit/s (the automotive-standard bitrate used on this bus, see
  // elm327.dbc `BS_: 500000`) has a ready-made TWAI_TIMING_CONFIG_* macro;
  // any other value would need a hand-computed timing config, which nobody
  // requested, so fail loudly instead of silently mis-timing the bus.
  if (bitrateBps_ != 500000) {
    LOG.printf("[CAN] unsupported bitrate %u bps (only 500000 supported)\n",
               static_cast<unsigned>(bitrateBps_));
    return false;
  }
  twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    LOG.println(F("[CAN] driver_install failed (transceiver not wired / pins in use?)"));
    return false;
  }
  if (twai_start() != ESP_OK) {
    LOG.println(F("[CAN] twai_start failed"));
    twai_driver_uninstall();
    return false;
  }

  ready_ = true;
  LOG.printf("[CAN] TWAI ready: TX=GPIO%u RX=GPIO%u @ %u bps\n", txPin_, rxPin_,
             static_cast<unsigned>(bitrateBps_));
  return true;
}

bool CanManager::send(uint32_t id, const uint8_t* data, uint8_t len) {
  twai_message_t msg = {};
  msg.identifier = id;
  msg.data_length_code = len;
  msg.extd = 0;
  memcpy(msg.data, data, len);

  // Non-blocking: a stalled/unterminated bus must never hang the main
  // loop (issue #14 "Fehlerbehandlung: CAN-Bus-Fehler loggen, nicht
  // crashen").
  if (twai_transmit(&msg, 0) != ESP_OK) {
    txErrors_++;
    return false;
  }
  return true;
}

void CanManager::updateAndTransmit(const GpsSnapshot& snap) {
  if (!ready_) return;

  const uint32_t now = millis();
  if (now - lastPositionMs_ >= kCyclePositionMs) {
    lastPositionMs_ = now;
    transmitPosition(snap);
  }
  if (now - lastFixMs_ >= kCycleFixMs) {
    lastFixMs_ = now;
    transmitFix(snap);
  }
  if (now - lastVelocityMs_ >= kCycleVelocityMs) {
    lastVelocityMs_ = now;
    transmitVelocity(snap);
  }
  if (now - lastTimeMs_ >= kCycleTimeMs) {
    lastTimeMs_ = now;
    transmitTime(snap);
  }
  if (now - lastSatellitesMs_ >= kCycleSatellitesMs) {
    lastSatellitesMs_ = now;
    transmitSatellites(snap);
  }
  if (now - lastStatusMs_ >= kCycleStatusMs) {
    lastStatusMs_ = now;
    transmitStatus(snap);
  }
}

void CanManager::transmitPosition(const GpsSnapshot& snap) {
  uint8_t data[8] = {0};
  putI32LE(&data[0], static_cast<int32_t>(lround(snap.latitude * 1e7)));
  putI32LE(&data[4], static_cast<int32_t>(lround(snap.longitude * 1e7)));
  if (!send(kIdGpsPosition, data, sizeof(data))) {
    LOG.println(F("[CAN] TX GPS_Position failed"));
  }
}

void CanManager::transmitFix(const GpsSnapshot& snap) {
  uint8_t data[8] = {0};
  data[0] = snap.fixQuality;
  data[1] = snap.fixMode;
  data[2] = snap.satellitesInUse;
  data[3] = snap.satellitesInView;
  putU16LE(&data[4], static_cast<uint16_t>(clampVal(lround(snap.hdop * 100.0f), 0L, 65535L)));
  putU16LE(&data[6], static_cast<uint16_t>(clampVal(lround(snap.vdop * 100.0f), 0L, 65535L)));
  if (!send(kIdGpsFix, data, sizeof(data))) {
    LOG.println(F("[CAN] TX GPS_Fix failed"));
  }
}

void CanManager::transmitVelocity(const GpsSnapshot& snap) {
  uint8_t data[8] = {0};
  putU16LE(&data[0], static_cast<uint16_t>(clampVal(lround(snap.speedKmh * 100.0f), 0L, 65535L)));
  putU16LE(&data[2], static_cast<uint16_t>(clampVal(lround(snap.courseDeg * 100.0f), 0L, 65535L)));
  putI16LE(&data[4],
           static_cast<int16_t>(clampVal(lround(snap.altitudeMsl * 10.0f), -32768L, 32767L)));
  putU16LE(&data[6], static_cast<uint16_t>(clampVal(lround(snap.pdop * 100.0f), 0L, 65535L)));
  if (!send(kIdGpsVelocity, data, sizeof(data))) {
    LOG.println(F("[CAN] TX GPS_Velocity failed"));
  }
}

void CanManager::transmitTime(const GpsSnapshot& snap) {
  uint8_t data[8] = {0};
  data[0] = snap.utcHour;
  data[1] = snap.utcMin;
  data[2] = snap.utcSec;
  data[3] = snap.utcDay;
  data[4] = snap.utcMonth;
  putU16LE(&data[5], snap.utcYear);
  data[7] = snap.dateValid ? 1 : 0;
  if (!send(kIdGpsTime, data, sizeof(data))) {
    LOG.println(F("[CAN] TX GPS_Time failed"));
  }
}

void CanManager::transmitSatellites(const GpsSnapshot& snap) {
  // Send every active satellite, kSatsPerFrame per frame, back-to-back
  // (issue #14: "mehrere Txs nacheinander").
  uint8_t batch = 0;
  uint8_t data[8] = {0};
  uint8_t slotInBatch = 0;
  bool anySent = false;

  auto flush = [&]() {
    if (slotInBatch == 0) return;
    data[0] = batch;
    if (!send(kIdGpsSatellites, data, sizeof(data))) {
      LOG.println(F("[CAN] TX GPS_Satellites failed"));
    }
    anySent = true;
    batch++;
    slotInBatch = 0;
    memset(data, 0, sizeof(data));
  };

  for (uint8_t i = 0; i < kGpsMaxSatellites; i++) {
    const GpsSnapshot::Satellite& sat = snap.satellites[i];
    if (!sat.active) continue;
    const uint8_t base = 1 + slotInBatch * 2;  // byte 0 is BatchIndex
    data[base] = sat.prn;
    data[base + 1] = sat.snr;
    slotInBatch++;
    if (slotInBatch >= kSatsPerFrame) flush();
  }
  flush();

  // No satellites in view: still send a status heartbeat so the headunit
  // sees an empty batch rather than stale data from a previous cycle.
  if (!anySent) {
    memset(data, 0, sizeof(data));
    if (!send(kIdGpsSatellites, data, sizeof(data))) {
      LOG.println(F("[CAN] TX GPS_Satellites failed"));
    }
  }
}

void CanManager::transmitStatus(const GpsSnapshot& snap) {
  uint8_t data[8] = {0};
  data[0] = snap.valid ? 1 : 0;
  data[1] = snap.activeFix ? 1 : 0;
  putU16LE(&data[2],
           static_cast<uint16_t>(clampVal<uint32_t>(snap.checksumErrors, 0, 65535)));
  const uint32_t lineAgeMs = millis() - snap.lastLineMs;
  putU32LE(&data[4], lineAgeMs);
  if (!send(kIdGpsStatus, data, sizeof(data))) {
    LOG.println(F("[CAN] TX GPS_Status failed"));
  }
}
