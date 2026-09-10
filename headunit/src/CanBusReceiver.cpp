#include "CanBusReceiver.h"
#include "log/LogTail.h"

#include <driver/twai.h>
#include <string.h>

namespace {
// CAN IDs from elm327.dbc (repo root) — must match motor-node/CanManager.
constexpr uint32_t kIdGpsPosition = 0x100;
constexpr uint32_t kIdGpsFix = 0x101;
constexpr uint32_t kIdGpsVelocity = 0x102;
constexpr uint32_t kIdGpsTime = 0x103;
constexpr uint32_t kIdGpsSatellites = 0x104;
constexpr uint32_t kIdGpsStatus = 0x105;

inline int32_t getI32LE(const uint8_t* in) {
  return static_cast<int32_t>(static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                               (static_cast<uint32_t>(in[2]) << 16) |
                               (static_cast<uint32_t>(in[3]) << 24));
}

inline uint16_t getU16LE(const uint8_t* in) {
  return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

inline int16_t getI16LE(const uint8_t* in) { return static_cast<int16_t>(getU16LE(in)); }

inline uint32_t getU32LE(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}
}  // namespace

CanBusReceiver::CanBusReceiver(uint8_t txPin, uint8_t rxPin, uint32_t bitrateBps)
    : txPin_(txPin), rxPin_(rxPin), bitrateBps_(bitrateBps) {
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_) xSemaphoreGive(mutex_);
}

CanBusReceiver::~CanBusReceiver() {
  if (mutex_) vSemaphoreDelete(mutex_);
}

bool CanBusReceiver::begin() {
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(txPin_), static_cast<gpio_num_t>(rxPin_),
                                   TWAI_MODE_NORMAL);
  // See CanManager::begin() (motor-node) for why only 500 kBit/s is
  // supported here (elm327.dbc `BS_: 500000`).
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

void CanBusReceiver::poll() {
  if (!ready_) return;

  // Drain whatever arrived since the last poll() without blocking the loop
  // task; the ESP-IDF TWAI driver buffers frames internally (interrupt
  // driven), so this is a plain non-blocking dequeue.
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    if (msg.extd || msg.rtr) continue;  // only standard data frames are defined on this bus
    handleFrame(msg.identifier, msg.data, msg.data_length_code);
  }

  // Surface bus-off/error-passive conditions without crashing (issue #15:
  // "Fehlerbehandlung ... nicht crashen" mirrors motor-node's CanManager).
  twai_status_info_t status;
  if (twai_get_status_info(&status) == ESP_OK && status.state == TWAI_STATE_BUS_OFF) {
    rxErrors_++;
    twai_initiate_recovery();
  }
}

void CanBusReceiver::handleFrame(uint32_t id, const uint8_t* d, uint8_t len) {
  if (len < 8) {
    rxErrors_++;
    return;
  }
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return;

  switch (id) {
    case kIdGpsPosition:
      data_.latitude = getI32LE(&d[0]) * 1e-7;
      data_.longitude = getI32LE(&d[4]) * 1e-7;
      break;
    case kIdGpsFix:
      data_.fixQuality = d[0];
      data_.fixMode = d[1];
      data_.satellitesInUse = d[2];
      data_.satellitesInView = d[3];
      data_.hdop = getU16LE(&d[4]) / 100.0f;
      data_.vdop = getU16LE(&d[6]) / 100.0f;
      break;
    case kIdGpsVelocity:
      data_.speedKmh = getU16LE(&d[0]) / 100.0f;
      data_.courseDeg = getU16LE(&d[2]) / 100.0f;
      data_.altitudeMsl = getI16LE(&d[4]) / 10.0f;
      data_.pdop = getU16LE(&d[6]) / 100.0f;
      break;
    case kIdGpsTime:
      data_.utcHour = d[0];
      data_.utcMin = d[1];
      data_.utcSec = d[2];
      data_.utcDay = d[3];
      data_.utcMonth = d[4];
      data_.utcYear = getU16LE(&d[5]);
      data_.dateValid = d[7] != 0;
      break;
    case kIdGpsSatellites:
      // Per-satellite PRN/SNR detail is not surfaced in CanGpsData today
      // (the UI only shows satellite counts from GPS_Fix); accepted here
      // so the frame counts as valid traffic for lastUpdateMs.
      break;
    case kIdGpsStatus:
      data_.valid = d[0] != 0;
      data_.activeFix = d[1] != 0;
      data_.checksumErrors = getU16LE(&d[2]);
      data_.lineAgeMs = getU32LE(&d[4]);
      break;
    default:
      xSemaphoreGive(mutex_);
      return;  // frame from a message ID we don't know about
  }
  data_.lastUpdateMs = millis();
  xSemaphoreGive(mutex_);
}

CanGpsData CanBusReceiver::getLatestGpsData() const {
  CanGpsData out;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    out = data_;
    xSemaphoreGive(mutex_);
  }
  return out;
}
