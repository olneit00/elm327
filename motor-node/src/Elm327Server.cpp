#include "Elm327Server.h"
#include "log/LogTail.h"

#include <string.h>

#include <vector>

namespace {
// "v1.5" was never an official ELM datasheet revision and is a common
// clone/tester red flag; "v1.4b" is a real ELM327 firmware identifier.
const char* kElmVersion = "ELM327 v1.4b";

// Central vehicle identity constants (SAE J1979 Mode 09). The historical
// chassis number is intentionally not a 17-character VIN and must be
// transmitted verbatim, not padded or reformatted.
constexpr char kChassisNumber[] = "10-58090";
constexpr char kVehicleDesignation[] = "Opel 1,2 ltr.";

// Single source of truth for which Mode 01 PIDs this emulator actually answers.
// Supported-PID bitmasks (0100/0120/0140/...) are derived from this list instead
// of being copied from a reference vehicle, so a PID is only ever reported as
// "supported" when a matching handler exists in buildMode01Response().
const uint8_t kSupportedPids[] = {0x01, 0x04, 0x05, 0x0C, 0x0D, 0x0E, 0x11, 0x2F, 0x4D};

// Mode 09 PIDs this emulator answers (VIN / Calibration ID).
const uint8_t kSupportedMode09Pids[] = {0x02, 0x04};

bool isPidInList(uint8_t pid, const uint8_t* pids, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (pids[i] == pid) {
      return true;
    }
  }
  return false;
}

bool isPidSupported(uint8_t pid) {
  return isPidInList(pid, kSupportedPids, sizeof(kSupportedPids));
}

// Builds the 32-bit "supported PIDs" bitmask for the range
// (rangeBasePid + 1) .. (rangeBasePid + 0x20), as returned by PIDs 00/20/40/...
// The lowest bit (representing rangeBasePid + 0x20 itself) is set whenever a
// PID beyond that range is supported, signalling that the next range query
// carries useful information.
uint32_t computeSupportedMaskFromList(uint8_t rangeBasePid, const uint8_t* pids, size_t count) {
  uint32_t mask = 0;
  for (int offset = 1; offset <= 31; ++offset) {
    if (isPidInList(static_cast<uint8_t>(rangeBasePid + offset), pids, count)) {
      mask |= (1UL << (32 - offset));
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (pids[i] > rangeBasePid + 0x20) {
      mask |= 1UL;
      break;
    }
  }
  return mask;
}

uint32_t computeSupportedMask(uint8_t rangeBasePid) {
  return computeSupportedMaskFromList(rangeBasePid, kSupportedPids, sizeof(kSupportedPids));
}

uint32_t computeMode09SupportedMask() {
  return computeSupportedMaskFromList(0x00, kSupportedMode09Pids, sizeof(kSupportedMode09Pids));
}

// A single ISO 15765-2 (ISO-TP) CAN frame: the raw CAN data bytes (including
// the PCI byte for First/Consecutive frames) plus how many of them are
// meaningful, i.e. the value ELM327 shows as the frame's "DLC".
struct IsoTpFrame {
  uint8_t data[8] = {0};
  uint8_t length = 0;
};

// Generic ISO-TP encoder: fragments an arbitrary OBD/UDS payload into the
// CAN frames a real ELM327 would report. Payloads that fit into a single
// classic CAN frame (<= 7 bytes) use a Single Frame, where the ELM "DLC"
// equals the payload length (i.e. the SF PCI value) and only the payload
// bytes are shown, matching real ELM327 single-line output. Longer payloads
// use a zero-padded First Frame + Consecutive Frames, each reported with the
// full 8 raw CAN bytes (PCI included), as a real adapter would.
std::vector<IsoTpFrame> buildIsoTpFrames(const uint8_t* payload, size_t length) {
  std::vector<IsoTpFrame> frames;

  if (length <= 7) {
    IsoTpFrame frame;
    frame.length = static_cast<uint8_t>(length);
    memcpy(frame.data, payload, length);
    frames.push_back(frame);
    return frames;
  }

  IsoTpFrame firstFrame;
  firstFrame.data[0] = static_cast<uint8_t>(0x10U | ((length >> 8U) & 0x0FU));
  firstFrame.data[1] = static_cast<uint8_t>(length & 0xFFU);
  size_t offset = 6;
  memcpy(firstFrame.data + 2, payload, offset);
  firstFrame.length = 8;
  frames.push_back(firstFrame);

  uint8_t sequenceNumber = 1;
  while (offset < length) {
    const size_t remaining = length - offset;
    const size_t chunk = remaining < 7 ? remaining : 7;

    IsoTpFrame consecutiveFrame;
    consecutiveFrame.data[0] = static_cast<uint8_t>(0x20U | (sequenceNumber & 0x0FU));
    memcpy(consecutiveFrame.data + 1, payload + offset, chunk);
    consecutiveFrame.length = 8;
    frames.push_back(consecutiveFrame);

    offset += chunk;
    sequenceNumber = static_cast<uint8_t>((sequenceNumber + 1) & 0x0FU);
  }
  return frames;
}
}  // namespace

class Elm327Server::BleCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit BleCallbacks(Elm327Server& server) : server_(server) {}

  // Runs on the NimBLE host task. Must not touch bleRxBuffer_ (an Arduino
  // String) directly - only enqueue raw bytes for the loop task to consume.
   void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
     const std::string value = characteristic->getValue();
     if (value.empty()) {
       return;
     }

#ifdef ELM_VERBOSE
     LOG.print(F("BLE RAW LEN="));
     LOG.println(value.length());
     LOG.print(F("BLE RAW ASCII=["));
     LOG.print(Elm327Server::escapeForLog(String(value.c_str())));
     LOG.println(F("]"));
     LOG.print(F("BLE RAW HEX="));
     for (size_t i = 0; i < value.length(); ++i) {
       char hex[4];
       snprintf(hex, sizeof(hex), "%02X ", static_cast<uint8_t>(value[i]));
       LOG.print(hex);
     }
     LOG.println();
#endif

     if (server_.bleRxQueue_ == nullptr) {
       return;
     }
     for (size_t i = 0; i < value.length(); ++i) {
       const uint8_t byteValue = static_cast<uint8_t>(value[i]);
       if (xQueueSend(server_.bleRxQueue_, &byteValue, 0) != pdTRUE) {
#ifdef ELM_VERBOSE
         LOG.println(F("BLE RX queue full, dropping byte"));
#endif
         break;
       }
     }
  }

 private:
  Elm327Server& server_;
};

// Detects BLE (re)connects/disconnects. Without this, a stale
// bleRxQueue_/bleRxBuffer_ from a previous connection could bleed into a new
// one, and if the peer drops the link uncleanly, advertising may never
// resume even with advertiseOnDisconnect(true) - both would explain repeated
// full ELM re-inits and an eventual "can't connect anymore".
class Elm327Server::BleServerCallbacks : public NimBLEServerCallbacks {
 public:
  explicit BleServerCallbacks(Elm327Server& server) : server_(server) {}

   void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
#ifdef ELM_VERBOSE
     LOG.println(F("BLE CONNECTED"));
#endif
     server_.bleRxBuffer_ = "";
     if (server_.bleRxQueue_ != nullptr) {
       xQueueReset(server_.bleRxQueue_);
     }
     server_.resetAdapterState();
   }

   void onDisconnect(NimBLEServer* bleServer, NimBLEConnInfo&, int reason) override {
#ifdef ELM_VERBOSE
     LOG.print(F("BLE DISCONNECTED reason="));
     LOG.println(reason);
#endif
     server_.bleRxBuffer_ = "";
     if (server_.bleRxQueue_ != nullptr) {
       xQueueReset(server_.bleRxQueue_);
     }
     // Explicit restart as a safety net in case advertiseOnDisconnect() does
     // not fire for this disconnect reason.
     bleServer->startAdvertising();
   }

 private:
  Elm327Server& server_;
};

Elm327Server::Elm327Server(VehicleState& vehicleState, const char* ssid, const char* password, uint16_t tcpPort)
    : vehicleState_(vehicleState),
      ssid_(ssid),
      password_(password),
      tcpPort_(tcpPort),
      server_(tcpPort) {}

Elm327Server::~Elm327Server() {
  // begin() is only ever called once at boot in practice (see main.cpp), so
  // this destructor never actually runs today - but without it, a future
  // caller that re-constructs/reinitializes Elm327Server would leak the BLE
  // callback objects and the RX queue on every re-init.
  delete bleCallbacks_;
  delete bleServerCallbacks_;
  if (bleRxQueue_ != nullptr) {
    vQueueDelete(bleRxQueue_);
  }
}

void Elm327Server::begin() {
  resetAdapterState();

  bleRxQueue_ = xQueueCreate(256, sizeof(uint8_t));

  LOG.printf("[STATE] VehicleState address=%p\n", static_cast<void*>(&vehicleState_));

  WiFi.mode(WIFI_AP);
  if (password_ != nullptr && strlen(password_) >= 8) {
    WiFi.softAP(ssid_, password_);
  } else {
    WiFi.softAP(ssid_);
  }

  server_.begin();
  server_.setNoDelay(true);

  NimBLEDevice::init("ELM327 Opel1935");
  NimBLEServer* bleServer = NimBLEDevice::createServer();
  bleServer->advertiseOnDisconnect(true);
  bleServerCallbacks_ = new BleServerCallbacks(*this);
  bleServer->setCallbacks(bleServerCallbacks_);
  NimBLEService* bleService = bleServer->createService("FFE0");
  bleTxCharacteristic_ = bleService->createCharacteristic(
      "FFE1", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE |
                 NIMBLE_PROPERTY::WRITE_NR);
  bleCallbacks_ = new BleCallbacks(*this);
  bleTxCharacteristic_->setCallbacks(bleCallbacks_);
  bleTxCharacteristic_->setValue(">");
  // NimBLE 2.x starts services together with the server; the historical
  // bleService->start() call is deprecated and has no effect.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName("ELM327 Opel1935");
  advertising->addServiceUUID("FFE0");
  advertising->enableScanResponse(true);
  advertising->setMinInterval(32);
  advertising->setMaxInterval(64);
  advertising->start();

  LOG.println(F("WiFi AP started"));
  LOG.print(F("SSID: "));
  LOG.println(ssid_);
  LOG.print(F("IP: "));
  LOG.println(WiFi.softAPIP());
  LOG.print(F("TCP server listening on port "));
  LOG.println(tcpPort_);
  LOG.println(F("BLE device: ELM327 Opel1935"));
}

void Elm327Server::update() {
   if (client_ && !client_.connected()) {
#ifdef ELM_VERBOSE
     LOG.println(F("Client disconnected"));
#endif
     client_.stop();
     rxBuffer_ = "";
   }

   acceptClientIfNeeded();

   processBleBytes();

   if (client_ && client_.connected()) {
     processClientBytes();
   }
}

void Elm327Server::acceptClientIfNeeded() {
   if (!server_.hasClient()) {
     return;
   }

   if (client_ && client_.connected()) {
     WiFiClient extraClient = server_.available();
     extraClient.stop();
     return;
   }

   client_ = server_.available();
   rxBuffer_ = "";

#ifdef ELM_VERBOSE
   LOG.print(F("Client connected: "));
   LOG.println(client_.remoteIP());
#endif

   // Real ELM327 adapters stay silent until the client sends the first
   // command; an unsolicited ">" here can desync a strict ELM parser.
}

void Elm327Server::processClientBytes() {
  while (client_.available() > 0) {
    const char ch = static_cast<char>(client_.read());

    if (ch == '\r' || ch == '\n') {
      if (!rxBuffer_.isEmpty()) {
        activeTransport_ = Transport::WiFi;
        processCommand(rxBuffer_);
        rxBuffer_ = "";
      }
      continue;
    }

    if (ch < 32 || ch > 126) {
      continue;
    }

     if (rxBuffer_.length() < 128) {
       rxBuffer_ += ch;
     } else {
#ifdef ELM_VERBOSE
       LOG.println(F("RX buffer overflow, dropping command"));
#endif
       rxBuffer_ = "";
       sendResponse("", "?");
     }
  }
}

void Elm327Server::processBleBytes() {
  // Drain bytes queued by the BLE task. From here on, bleRxBuffer_ is only
  // ever touched on the loop task, eliminating the cross-task String race.
  if (bleRxQueue_ != nullptr) {
    uint8_t byteValue = 0;
    bool drainedAny = false;
     while (xQueueReceive(bleRxQueue_, &byteValue, 0) == pdTRUE) {
       bleRxBuffer_ += static_cast<char>(byteValue);
       drainedAny = true;
       if (bleRxBuffer_.length() > 256) {
         bleRxBuffer_ = "";
#ifdef ELM_VERBOSE
         LOG.println(F("BLE RX buffer overflow, dropping command"));
#endif
       }
     }
#ifdef ELM_VERBOSE
     if (drainedAny) {
       LOG.print(F("COMMAND BUFFER=["));
       LOG.print(escapeForLog(bleRxBuffer_));
       LOG.println(F("]"));
     }
#endif
  }

  while (true) {
    const int separator = bleRxBuffer_.indexOf('\n');
    const int carriageReturn = bleRxBuffer_.indexOf('\r');
    int end = separator;
    if (end < 0 || (carriageReturn >= 0 && carriageReturn < end)) {
      end = carriageReturn;
    }
    if (end < 0) {
      return;
    }

    const String command = bleRxBuffer_.substring(0, end);
    bleRxBuffer_.remove(0, end + 1);
    if (!command.isEmpty()) {
      activeTransport_ = Transport::Ble;
      processCommand(command);
    }
  }
}

void Elm327Server::processCommand(const String& rawCommand) {
   const String normalizedCommand = normalizeCommand(rawCommand);
   if (normalizedCommand.isEmpty()) {
     sendResponse(rawCommand, "");
     return;
   }

#ifdef ELM_VERBOSE
   LOG.print(F("ELM COMMAND=["));
   LOG.print(normalizedCommand);
   LOG.println(F("]"));
#endif

   String response;
   if (normalizedCommand.startsWith("AT")) {
     response = handleAtCommand(normalizedCommand);
   } else {
     response = handleObdCommand(normalizedCommand);
   }

   if (response.isEmpty()) {
#ifdef ELM_VERBOSE
     LOG.print(F("UNKNOWN COMMAND: "));
     LOG.println(rawCommand);
#endif
     response = "?";
   }

   sendResponse(rawCommand, response);
}

String Elm327Server::normalizeCommand(const String& rawCommand) const {
  String normalized;
  normalized.reserve(rawCommand.length());

  for (size_t i = 0; i < rawCommand.length(); ++i) {
    const char ch = rawCommand[i];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      continue;
    }
    normalized += static_cast<char>(toupper(static_cast<unsigned char>(ch)));
  }

  // Some OBD applications append a semicolon to setup commands.
  while (normalized.endsWith(";")) {
    normalized.remove(normalized.length() - 1);
  }

  return normalized;
}

String Elm327Server::handleAtCommand(const String& normalizedCommand) {
  if (normalizedCommand == "ATZ" || normalizedCommand == "ATWS") {
    resetAdapterState();
    return kElmVersion;
  }

  if (normalizedCommand == "ATI") {
    return kElmVersion;
  }

  if (normalizedCommand == "ATE0") {
    echoEnabled_ = false;
    return "OK";
  }

  if (normalizedCommand == "ATE1") {
    echoEnabled_ = true;
    return "OK";
  }

  if (normalizedCommand == "ATL0") {
    linefeedsEnabled_ = false;
    return "OK";
  }

  if (normalizedCommand == "ATL1") {
    linefeedsEnabled_ = true;
    return "OK";
  }

  if (normalizedCommand == "ATS0") {
    spacesEnabled_ = false;
    return "OK";
  }

  if (normalizedCommand == "ATS1") {
    spacesEnabled_ = true;
    return "OK";
  }

  if (normalizedCommand == "ATH0") {
    headersEnabled_ = false;
    return "OK";
  }

  if (normalizedCommand == "ATH1") {
    headersEnabled_ = true;
    return "OK";
  }

  if (normalizedCommand == "ATD") {
    resetAdapterState();
    return "OK";
  }

  if (normalizedCommand == "ATD0") {
    displayDlcEnabled_ = false;
    LOG.println(F("STATE: displayDlc=false"));
    return "OK";
  }

  if (normalizedCommand == "ATD1") {
    displayDlcEnabled_ = true;
    LOG.println(F("STATE: displayDlc=true"));
    return "OK";
  }

  if (normalizedCommand == "ATM0") {
    memoryEnabled_ = false;
    return "OK";
  }

  if (normalizedCommand == "ATM1") {
    memoryEnabled_ = true;
    return "OK";
  }

  if (normalizedCommand == "ATAT0") {
    adaptiveTiming_ = 0;
    return "OK";
  }

  if (normalizedCommand == "ATSP") {
    return "OK";
  }

  if (normalizedCommand.startsWith("ATSP") && normalizedCommand.length() == 5) {
    const char protocolChar = normalizedCommand[4];
    if (protocolChar >= '0' && protocolChar <= '9') {
      const uint8_t requestedProtocol = static_cast<uint8_t>(protocolChar - '0');
      if (requestedProtocol == 0) {
        // Auto-select: the actual protocol is only known once the first OBD
        // request has been answered (see handleObdCommand()).
        autoProtocol_ = true;
        protocolDetected_ = false;
      } else {
        // This virtual ECU only ever exists on ISO 15765-4 CAN 11/500; a
        // request for any other protocol number is acknowledged (so the
        // handshake doesn't stall) but must not actually switch the
        // formatter to a framing (e.g. KWP2000's "48 6B 10" header) we do
        // not really implement, which previously produced undecodable
        // responses and an endless 0100 retry loop in Car Scanner.
        if (requestedProtocol != 6) {
          LOG.print(F("ATSP: requested protocol "));
          LOG.print(requestedProtocol);
          LOG.println(F(" is not emulated, staying on CAN 11/500"));
        }
        autoProtocol_ = false;
        protocol_ = 6;
        protocolDetected_ = true;
      }
      return "OK";
    }
    return "OK";
  }

  if (normalizedCommand == "ATPC" || normalizedCommand == "ATCAF0" ||
      normalizedCommand == "ATCAF1" || normalizedCommand == "ATCFC0" || normalizedCommand == "ATCFC1" ||
      normalizedCommand == "ATAL" || normalizedCommand == "ATSW05" || normalizedCommand == "ATCEA" ||
      normalizedCommand == "ATIB10" || normalizedCommand == "ATIB96" || normalizedCommand == "ATIIA13" ||
      normalizedCommand == "ATST00" || normalizedCommand == "ATST20" || normalizedCommand == "ATWM221201" ||
      normalizedCommand == "ATFCSD30000000" || normalizedCommand == "ATFCSM1") {
    return "OK";
  }

  if (normalizedCommand.startsWith("ATSH") || normalizedCommand.startsWith("ATFCSH") ||
      normalizedCommand.startsWith("ATFCSD") || normalizedCommand.startsWith("ATFCSM") ||
      normalizedCommand.startsWith("ATWM") || normalizedCommand.startsWith("ATIB") ||
      normalizedCommand.startsWith("ATCRA") || normalizedCommand.startsWith("ATIIA")) {
    return "OK";
  }

  if (normalizedCommand == "AT@1") {
    return "Opel1935 ELM327 Emulator";
  }

  if (normalizedCommand == "AT@2") {
    return "OK";
  }

  if (normalizedCommand == "ATDP") {
    return protocolDescription();
  }

  if (normalizedCommand == "ATDPN") {
    return protocolNumber();
  }

  if (normalizedCommand == "ATAT1") {
    adaptiveTiming_ = 1;
    return "OK";
  }

  if (normalizedCommand == "ATAT2") {
    adaptiveTiming_ = 2;
    return "OK";
  }

  if (normalizedCommand.startsWith("ATST") && normalizedCommand.length() == 6) {
    const String hexValue = normalizedCommand.substring(4);
    char* endPtr = nullptr;
    const unsigned long parsed = strtoul(hexValue.c_str(), &endPtr, 16);
    if (endPtr != nullptr && *endPtr == '\0' && parsed <= 0xFFUL) {
      timeout_ = static_cast<uint8_t>(parsed);
      return "OK";
    }
    return "?";
  }

  // Unrecognized AT commands (e.g. a deliberately invalid "ATZZ" from a
  // conformance tester) must be rejected like a real ELM327 does, not
  // acknowledged with "OK". Every command Car Scanner is known to send
  // during initialization is matched explicitly above.
  LOG.print(F("UNKNOWN COMMAND: "));
  LOG.println(normalizedCommand);
  return "?";
}

String Elm327Server::handleObdCommand(const String& normalizedCommand) {
  // OBD service (mode) is always the first two hex digits; dispatch on it
  // instead of hardcoding individual requests like "0902".
  if (normalizedCommand.startsWith("09")) {
    return handleMode09Command(normalizedCommand);
  }
  if (normalizedCommand.startsWith("01")) {
    return handleMode01Command(normalizedCommand);
  }
  return "";
}

String Elm327Server::handleMode01Command(const String& normalizedCommand) {
  if (normalizedCommand.length() < 4) {
    return "";
  }

  const String pidString = normalizedCommand.substring(2, 4);
  char* endPtr = nullptr;
  const unsigned long pidRaw = strtoul(pidString.c_str(), &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || pidRaw > 0xFFUL) {
    return "";
  }
  const uint8_t pid = static_cast<uint8_t>(pidRaw);

#ifdef ELM_VERBOSE
  LOG.print(F("OBD MODE=01 PID="));
  LOG.println(formatByte(pid));
  LOG.printf("[STATE] VehicleState address=%p\n", static_cast<void*>(&vehicleState_));
  LOG.printf("[ELM STATE] headers=%s dlc=%s spaces=%s\n", headersEnabled_ ? "true" : "false",
                 displayDlcEnabled_ ? "true" : "false", spacesEnabled_ ? "true" : "false");
#endif

  // The first Mode 01 request after ATSP0 (auto) triggers the protocol
  // search. We only ever emulate ISO 15765-4 CAN 11/500 (protocol 6), so the
  // search always "finds" it and a normal, valid response follows right away
  // instead of the usual multi-protocol probing a real adapter performs.
  String prefix;
  if (autoProtocol_ && !protocolDetected_) {
    protocolDetected_ = true;
    protocol_ = 6;
    prefix = "SEARCHING..." + lineEnding();
  }

  const String payload = buildMode01Response(pid);
  if (payload.isEmpty()) {
    return prefix + "NO DATA";
  }
  return prefix + payload;
}

String Elm327Server::handleMode09Command(const String& normalizedCommand) {
  if (normalizedCommand.length() < 4) {
    return "";
  }

  // A trailing response-count digit (e.g. "09021") is not part of the PID.
  const String pidString = normalizedCommand.substring(2, 4);
  char* endPtr = nullptr;
  const unsigned long pidRaw = strtoul(pidString.c_str(), &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || pidRaw > 0xFFUL) {
    return "";
  }
  const uint8_t pid = static_cast<uint8_t>(pidRaw);

#ifdef ELM_VERBOSE
  LOG.print(F("OBD MODE=09 PID="));
  LOG.println(formatByte(pid));
#endif

  if (pid == 0x00) {
    const uint32_t mask = computeMode09SupportedMask();
    const uint8_t data[] = {
        static_cast<uint8_t>((mask >> 24U) & 0xFFU), static_cast<uint8_t>((mask >> 16U) & 0xFFU),
        static_cast<uint8_t>((mask >> 8U) & 0xFFU), static_cast<uint8_t>(mask & 0xFFU)};
    return makeMode09Response(0x00, data, sizeof(data));
  }

  if (pid == 0x02) {
    return makeMode09TextResponse(0x02, kChassisNumber);
  }

  if (pid == 0x04) {
    return makeMode09TextResponse(0x04, kVehicleDesignation);
  }

  return "NO DATA";
}

String Elm327Server::buildMode01Response(uint8_t pid) const {
  if (pid == 0x00) {
    return makeSupportedPidResponse(0x00);
  }
  if (pid == 0x20) {
    return makeSupportedPidResponse(0x20);
  }
  if (pid == 0x40) {
    return makeSupportedPidResponse(0x40);
  }

  if (pid == 0x05) {
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] coolantTemperature before conversion=%.2f C\n", vehicleState_.coolantTemperature);
#endif
    // Real ECT sensor: when it currently reports a fault (open/short/out of
    // range) we still answer with the last known good temperature rather than
    // "NO DATA", so the ELM stream stays responsive, but we surface the fault
    // on the log + ./vehicle page (vehicleState_.coolantTemperatureValid).
    if (!vehicleState_.coolantTemperatureValid) {
      LOG.println(F("[OBD] ECT sensor fault -> serving last known coolant temperature"));
    }
    const uint8_t raw = vehicleState_.coolantToObdRaw();
    const uint8_t data[] = {raw};
    const String response = makeMode01Response(0x05, data, sizeof(data));
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] coolant raw decimal=%u hex=0x%02X\n", raw, raw);
    LOG.print(F("[OBD] payload=41 05 "));
    LOG.println(formatByte(raw));
#endif
    return response;
  }

  if (pid == 0x01) {
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
    return makeMode01Response(0x01, data, sizeof(data));
  }

  if (pid == 0x04) {
    const uint8_t data[] = {0x00};
    return makeMode01Response(0x04, data, sizeof(data));
  }

  if (pid == 0x11) {
    const uint8_t data[] = {0x00};
    return makeMode01Response(0x11, data, sizeof(data));
  }

  if (pid == 0x0C) {
    const uint16_t rpmRaw = vehicleState_.rpmToObdRaw();
    const uint8_t data[] = {static_cast<uint8_t>((rpmRaw >> 8) & 0xFFU), static_cast<uint8_t>(rpmRaw & 0xFFU)};
    return makeMode01Response(0x0C, data, sizeof(data));
  }

  if (pid == 0x0D) {
    const uint8_t data[] = {vehicleState_.speedToObdRaw()};
    return makeMode01Response(0x0D, data, sizeof(data));
  }

  if (pid == 0x0E) {
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] ignitionAdvanceDeg=%.2f\n", vehicleState_.ignitionAdvanceDeg);
#endif
    const uint8_t raw = vehicleState_.ignitionAdvanceToObdRaw();
    const uint8_t data[] = {raw};
    const String response = makeMode01Response(0x0E, data, sizeof(data));
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] raw=%u / 0x%02X\n", raw, raw);
    LOG.print(F("[OBD] payload=41 0E "));
    LOG.println(formatByte(raw));
#endif
    return response;
  }

  if (pid == 0x2F) {
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] fuelPercent=%.2f\n", vehicleState_.fuelPercent);
#endif
    const uint8_t raw = vehicleState_.fuelToObdRaw();
    const uint8_t data[] = {raw};
    const String response = makeMode01Response(0x2F, data, sizeof(data));
#ifdef ELM_VERBOSE
    LOG.printf("[OBD] raw=%u / 0x%02X\n", raw, raw);
    LOG.print(F("[OBD] payload=41 2F "));
    LOG.println(formatByte(raw));
#endif
    return response;
  }

  // PID 0x4D: run time since engine start, in minutes (2 bytes).
  if (pid == 0x4D) {
    const uint16_t minutes = vehicleState_.runtimeToObdMinutes();
    const uint8_t data[] = {static_cast<uint8_t>((minutes >> 8) & 0xFFU), static_cast<uint8_t>(minutes & 0xFFU)};
    return makeMode01Response(0x4D, data, sizeof(data));
  }

  return "";
}

String Elm327Server::makeSupportedPidResponse(uint8_t basePid) const {
  const uint32_t bitmask = computeSupportedMask(basePid);
  const uint8_t data[] = {
      static_cast<uint8_t>((bitmask >> 24U) & 0xFFU), static_cast<uint8_t>((bitmask >> 16U) & 0xFFU),
      static_cast<uint8_t>((bitmask >> 8U) & 0xFFU), static_cast<uint8_t>(bitmask & 0xFFU)};
  return makeMode01Response(basePid, data, sizeof(data));
}

String Elm327Server::makeMode01Response(uint8_t pid, const uint8_t* data, size_t len) const {
  uint8_t payload[2 + 32];
  payload[0] = 0x41;
  payload[1] = pid;
  const size_t copyLen = len > sizeof(payload) - 2 ? sizeof(payload) - 2 : len;
  memcpy(payload + 2, data, copyLen);
  return formatIsoTpResponse(0x7E8, payload, copyLen + 2);
}

String Elm327Server::makeMode09Response(uint8_t pid, const uint8_t* data, size_t len) const {
  uint8_t payload[2 + 64];
  payload[0] = 0x49;
  payload[1] = pid;
  const size_t copyLen = len > sizeof(payload) - 2 ? sizeof(payload) - 2 : len;
  memcpy(payload + 2, data, copyLen);
  return formatIsoTpResponse(0x7E8, payload, copyLen + 2);
}

// Mode 09 text fields (VIN, Calibration ID, ...) additionally carry a
// message/record-index byte (0x01 for a single-item response) per SAE J1979,
// followed by the raw ASCII bytes - transmitted verbatim, never padded.
String Elm327Server::makeMode09TextResponse(uint8_t pid, const char* text) const {
  uint8_t payload[3 + 64];
  payload[0] = 0x49;
  payload[1] = pid;
  payload[2] = 0x01;
  const size_t textLen = strlen(text);
  const size_t copyLen = textLen > sizeof(payload) - 3 ? sizeof(payload) - 3 : textLen;
  memcpy(payload + 3, text, copyLen);
  return formatIsoTpResponse(0x7E8, payload, copyLen + 3);
}

// Central ELM/CAN response formatter: turns a CAN id + raw payload bytes into
// the exact text an ELM327 emulates, honoring the current ATH/ATS/protocol
// state. This is the only place that assembles OBD response text, so every
// PID and every state combination (headers on/off, spaces on/off, KWP vs
// CAN11) is handled consistently.
String Elm327Server::formatObdResponse(uint16_t canId, const uint8_t* payload, size_t len) const {
  String response;
  bool needSeparator = false;

  auto appendToken = [&](const String& token) {
    if (needSeparator && spacesEnabled_) {
      response += " ";
    }
    response += token;
    needSeparator = true;
  };

  if (headersEnabled_) {
    if (protocol_ == 4 || protocol_ == 5) {
      appendToken("48");
      appendToken("6B");
      appendToken("10");
    } else {
      char idBuffer[4];
      snprintf(idBuffer, sizeof(idBuffer), "%03X", static_cast<unsigned>(canId & 0x7FFU));
      appendToken(String(idBuffer));
      // Car Scanner's own real init sequence sends "ATD0" as boilerplate
      // (before ATH1, never followed by "ATD1"), yet its CAN11 decoder
      // still requires the DLC field to be present - gating this on
      // displayDlcEnabled_ silently broke value decoding in practice. The
      // DLC is therefore always shown while headers are on, regardless of
      // ATD0/ATD1 (which are still accepted/tracked for AT-command
      // conformance, just without affecting this real-world-critical output).
      appendToken(formatByte(static_cast<uint8_t>(len)));
    }
  }

  for (size_t i = 0; i < len; ++i) {
    appendToken(formatByte(payload[i]));
  }

  return response;
}

// Fragments an OBD payload via the generic ISO-TP encoder (buildIsoTpFrames)
// and renders every resulting CAN frame through formatObdResponse(), one
// frame per line, honoring ATH/ATS/ATL exactly like single-frame responses.
// Payloads that fit a Single Frame produce the same output as before
// (no behavior change for existing Mode 01 responses).
String Elm327Server::formatIsoTpResponse(uint16_t canId, const uint8_t* payload, size_t len) const {
  const std::vector<IsoTpFrame> frames = buildIsoTpFrames(payload, len);

#ifdef ELM_VERBOSE
  if (frames.size() > 1) {
    LOG.print(F("ISO-TP: TOTAL LENGTH="));
    LOG.println(len);
  }
#endif

  String result;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (i > 0) {
      result += lineEnding();
    }
    const String frameText = formatObdResponse(canId, frames[i].data, frames[i].length);
#ifdef ELM_VERBOSE
    if (frames.size() > 1) {
      LOG.print(i == 0 ? F("FIRST FRAME=") : F("CONSECUTIVE FRAME "));
      if (i > 0) {
        LOG.print(i);
        LOG.print(F("="));
      }
      LOG.println(frameText);
    }
#endif
    result += frameText;
  }
  return result;
}

String Elm327Server::protocolDescription() const {
  if (autoProtocol_ && !protocolDetected_) {
    return "AUTO";
  }
  if (protocol_ == 4) {
    return "ISO 14230-4 (KWP FAST INIT)";
  }
  if (protocol_ == 5) {
    return "ISO 14230-4 (KWP 5 BAUD INIT)";
  }
  if (protocol_ == 6) {
    return autoProtocol_ ? "AUTO, ISO 15765-4 (CAN 11/500)" : "ISO 15765-4 (CAN 11/500)";
  }
  return "AUTO";
}

String Elm327Server::protocolNumber() const {
  if (autoProtocol_) {
    return protocolDetected_ ? ("A" + String(protocol_)) : "A0";
  }
  return String(protocol_);
}

String Elm327Server::formatByte(uint8_t value) const {
  char out[3];
  snprintf(out, sizeof(out), "%02X", value);
  return String(out);
}

String Elm327Server::lineEnding() const {
  return linefeedsEnabled_ ? "\r\n" : "\r";
}

void Elm327Server::resetAdapterState() {
  echoEnabled_ = true;
  linefeedsEnabled_ = true;
  spacesEnabled_ = true;
  headersEnabled_ = false;
  // DLC printing defaults to on: this matches the behavior already
  // validated with Car Scanner's CAN11 auto-detect before ATD0/ATD1 existed
  // as a distinct flag, so ATZ/ATD alone must not change that output.
  displayDlcEnabled_ = true;
  memoryEnabled_ = false;
  autoProtocol_ = true;
  protocolDetected_ = false;
  protocol_ = 6;
  adaptiveTiming_ = 1;
  timeout_ = 0x32;
}

void Elm327Server::sendResponse(const String& rawCommand, const String& response) {
  const bool wifiAvailable = client_ && client_.connected();
  const bool bleAvailable = bleTxCharacteristic_ != nullptr;
  if (!wifiAvailable && !bleAvailable) {
    return;
  }

  // Build the entire reply (echo + payload + terminator + prompt) as a single
  // string. Sending it as one transport write is essential for BLE: issuing
  // several rapid, separate setValue()+notify() calls (one per fragment)
  // causes earlier fragments to be overwritten before they are actually sent
  // over the air, so only the last one (the bare ">") ever reaches the
  // client. Exactly one prompt, no orphaned intermediate replies.
  String full;
  if (echoEnabled_ && !rawCommand.isEmpty()) {
    full += rawCommand;
    full += lineEnding();
  }
  if (!response.isEmpty()) {
    full += response;
  }
  full += lineEnding();
  full += ">";

#ifdef ELM_VERBOSE
  LOG.print(F("ELM RESPONSE=["));
  LOG.print(Elm327Server::escapeForLog(response));
  LOG.println(F("]"));
#endif

  // Diagnostic-only: verify the actual TX stream matches the configured
  // ATL0/ATL1 line ending before it is ever sent. Never mutates `full`.
  // Kept active outside ELM_VERBOSE since it only prints on a genuine
  // protocol-framing bug, not on every response.
  if (!linefeedsEnabled_ && full.indexOf('\n') >= 0) {
    LOG.print(F("[ELM ERROR] LF found in TX while ATL0 is active! full=["));
    LOG.print(escapeForLog(full));
    LOG.println(F("]"));
  } else if (linefeedsEnabled_) {
    for (int i = 0; i < static_cast<int>(full.length()); ++i) {
      if (full[i] == '\n' && (i == 0 || full[i - 1] != '\r')) {
        LOG.print(F("[ELM ERROR] Lone LF (not preceded by CR) at index "));
        LOG.print(i);
        LOG.print(F(" in TX! full=["));
        LOG.print(escapeForLog(full));
        LOG.println(F("]"));
        break;
      }
    }
  }

#ifdef ELM_VERBOSE
  LOG.print(F("[ELM TX TEXT] \""));
  LOG.print(escapeForLog(full));
  LOG.println(F("\""));
  LOG.print(F("[ELM TX HEX] "));
  for (size_t i = 0; i < full.length(); ++i) {
    char hex[4];
    snprintf(hex, sizeof(hex), "%02X ", static_cast<uint8_t>(full[i]));
    LOG.print(hex);
  }
  LOG.println();
#endif

  if (activeTransport_ == Transport::WiFi && client_ && client_.connected()) {
    client_.print(full);
  } else if (activeTransport_ == Transport::Ble && bleTxCharacteristic_ != nullptr) {
#ifdef ELM_VERBOSE
    LOG.print(F("BLE TX=["));
    LOG.print(Elm327Server::escapeForLog(full));
    LOG.println(F("]"));
#endif
    bleWriteChunked(full);
  }
}

void Elm327Server::bleWriteChunked(const String& text) {
  size_t offset = 0;
  while (offset < text.length()) {
    const size_t chunkLen = min(kBleChunkSize, text.length() - offset);
    bleTxCharacteristic_->setValue(reinterpret_cast<const uint8_t*>(text.c_str()) + offset, chunkLen);
    bleTxCharacteristic_->notify();
    offset += chunkLen;
    if (offset < text.length()) {
      // Give the BLE stack time to actually transmit this chunk before its
      // value is overwritten by the next one.
      delay(15);
    }
  }
}

String Elm327Server::escapeForLog(const String& text) {
  String escaped;
  escaped.reserve(text.length());
  for (size_t i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    if (ch == '\r') {
      escaped += "\\r";
    } else if (ch == '\n') {
      escaped += "\\n";
    } else if (ch == '\t') {
      escaped += "\\t";
    } else {
      escaped += ch;
    }
  }
  return escaped;
}
