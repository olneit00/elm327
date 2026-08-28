// UBX Protocol Helper: configure NEO-8M search mode, constellation, SBAS
// Ready to use once research provides the exact UBX command bytes

#include <Arduino.h>

namespace UbxHelper {

// UBX frame structure: SYNC $B5 $62 | CLASS | ID | LENGTH(2) | PAYLOAD | CHECKSUM(2)
// Checksum = Fletcher-16 (CK_A, CK_B) over CLASS-ID-LENGTH-PAYLOAD

void sendUbxCommand(const uint8_t* cmd, size_t len) {
  // SYNC: $B5 $62
  Serial2.write(0xB5);
  Serial2.write(0x62);
  Serial2.write(cmd, len);
  
  // Calculate checksum
  uint8_t ck_a = 0, ck_b = 0;
  for (size_t i = 0; i < len; i++) {
    ck_a += cmd[i];
    ck_b += ck_a;
  }
  Serial2.write(ck_a);
  Serial2.write(ck_b);
}

// UBX-CFG-GNSS: Enable/disable constellations
// (research will provide exact payload)
void configConstellation(bool gps, bool glonass, bool galileo, bool beidou, bool sbas) {
  // Placeholder: research will fill in the exact structure
  // Format: CLASS=0x06, ID=0x3E, payload = constellation enable bits
  Serial.println("[UBX] Placeholder: UBX-CFG-GNSS - await research for exact bytes");
}

// UBX-CFG-SBAS: Enable/disable SBAS and region
void configSBAS(bool enabled, uint8_t region) {
  // region: 0=worldwide, 1=DGPS, 2=SBAS, 3=RTCM, etc.
  // (research will provide exact structure)
  Serial.println("[UBX] Placeholder: UBX-CFG-SBAS - await research for exact bytes");
}

// UBX-NAV-PVT: Query current fix status, satellite count, position
void querySolutionStatus() {
  // CLASS=0x01, ID=0x07
  // No payload needed (query-only)
  uint8_t cmd[] = { 0x01, 0x07 };
  sendUbxCommand(cmd, sizeof(cmd));
  Serial.println("[UBX] Sent NAV-PVT query");
}

// UBX-NAV-SVINFO: Query satellite info (PRN, elevation, azimuth, SNR per sat)
void querySatelliteInfo() {
  // CLASS=0x01, ID=0x30
  uint8_t cmd[] = { 0x01, 0x30 };
  sendUbxCommand(cmd, sizeof(cmd));
  Serial.println("[UBX] Sent NAV-SVINFO query");
}

// Parse UBX frame from Serial2 (simplified)
// Returns true if a complete frame was received
bool parseUbxFrame(uint8_t* buffer, size_t buflen, size_t& frameLen) {
  // Look for SYNC marker $B5 $62
  while (Serial2.available() >= 2) {
    if (Serial2.peek() == 0xB5) {
      Serial2.read();  // consume $B5
      if (Serial2.peek() == 0x62) {
        Serial2.read();  // consume $62
        // Read CLASS, ID, LENGTH(2 bytes LE)
        if (Serial2.available() < 4) return false;
        buffer[0] = Serial2.read();  // CLASS
        buffer[1] = Serial2.read();  // ID
        uint16_t len = Serial2.read() | (Serial2.read() << 8);  // LENGTH LE
        if (len + 8 > buflen) {
          Serial.println("[UBX] Frame too long, skipping");
          return false;
        }
        // Read payload + checksum
        if (Serial2.available() < len + 2) return false;
        for (uint16_t i = 0; i < len + 2; i++) {
          buffer[2 + i] = Serial2.read();
        }
        frameLen = len + 8;
        return true;
      }
    } else {
      Serial2.read();  // skip garbage byte
    }
  }
  return false;
}

}  // namespace UbxHelper

// Usage example in main loop:
/*
void loop() {
  // Send queries every 10 seconds
  static unsigned long lastQuery = 0;
  if (millis() - lastQuery > 10000) {
    lastQuery = millis();
    UbxHelper::querySolutionStatus();
    delay(100);
    UbxHelper::querySatelliteInfo();
  }
  
  // Parse UBX responses
  uint8_t ubxBuf[256];
  size_t frameLen = 0;
  if (UbxHelper::parseUbxFrame(ubxBuf, sizeof(ubxBuf), frameLen)) {
    Serial.printf("[UBX] Received frame: CLASS=0x%02x ID=0x%02x LEN=%u\n",
                  ubxBuf[0], ubxBuf[1], frameLen - 8);
  }
}
*/