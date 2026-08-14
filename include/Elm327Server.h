#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "VehicleState.h"

class Elm327Server {
 public:
  Elm327Server(VehicleState& vehicleState, const char* ssid, const char* password, uint16_t tcpPort);

  void begin();
  void update();

 private:
  class BleCallbacks;
  class BleServerCallbacks;

  void acceptClientIfNeeded();
  void processClientBytes();
  void processBleBytes();
  void processCommand(const String& rawCommand);

  String normalizeCommand(const String& rawCommand) const;
  String handleAtCommand(const String& normalizedCommand);
  String handleObdCommand(const String& normalizedCommand);
  String handleMode01Command(const String& normalizedCommand);
  String handleMode09Command(const String& normalizedCommand);
  String buildMode01Response(uint8_t pid) const;
  String makeSupportedPidResponse(uint8_t basePid) const;
  String makeMode01Response(uint8_t pid, const uint8_t* data, size_t len) const;
  String makeMode09Response(uint8_t pid, const uint8_t* data, size_t len) const;
  String makeMode09TextResponse(uint8_t pid, const char* text) const;
  String formatObdResponse(uint16_t canId, const uint8_t* payload, size_t len) const;
  String formatIsoTpResponse(uint16_t canId, const uint8_t* payload, size_t len) const;
  String protocolDescription() const;
  String protocolNumber() const;
  String formatByte(uint8_t value) const;
  String lineEnding() const;
  void resetAdapterState();
  void sendResponse(const String& rawCommand, const String& response);
  void bleWriteChunked(const String& text);
  static String escapeForLog(const String& text);

  VehicleState& vehicleState_;
  const char* ssid_;
  const char* password_;
  uint16_t tcpPort_;

  WiFiServer server_;
  WiFiClient client_;
  String rxBuffer_;
  String bleRxBuffer_;
  NimBLECharacteristic* bleTxCharacteristic_ = nullptr;
  BleCallbacks* bleCallbacks_ = nullptr;
  BleServerCallbacks* bleServerCallbacks_ = nullptr;

  // The NimBLE stack invokes onWrite() from its own FreeRTOS task, not the
  // Arduino loop task. The callback only ever pushes raw bytes into this
  // queue; bleRxBuffer_ (an Arduino String, not safe for concurrent
  // cross-task mutation) is exclusively read/written from processBleBytes(),
  // which always runs on the loop task via update().
  QueueHandle_t bleRxQueue_ = nullptr;

  enum class Transport { None, WiFi, Ble };
  Transport activeTransport_ = Transport::None;

  bool echoEnabled_ = true;
  bool linefeedsEnabled_ = true;
  bool spacesEnabled_ = true;
  bool headersEnabled_ = false;
  // ATD0/ATD1: whether the ELM-CAN DLC field is printed after the CAN ID
  // (only meaningful when headersEnabled_ is true). Distinct from ATH0/ATH1,
  // which control the header/DLC block as a whole.
  bool displayDlcEnabled_ = true;
  bool memoryEnabled_ = false;
  bool autoProtocol_ = true;
  // Set once the (auto-detected or explicitly selected) protocol is ready to use.
  bool protocolDetected_ = false;
  uint8_t protocol_ = 6;
  uint8_t adaptiveTiming_ = 1;
  uint8_t timeout_ = 0x32;

  // Conservative chunk size for BLE notifications: safe even at the default
  // (un-negotiated) ATT MTU of 23 bytes (20 usable payload bytes).
  static constexpr size_t kBleChunkSize = 20;
};
