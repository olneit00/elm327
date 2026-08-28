// Diagnostic sketch for NEO-8M "no fix" troubleshooting
// motor-node: serial monitor output to identify the bottleneck

#include <Arduino.h>

void diagnosticLoop() {
  Serial.begin(115200);  // host PC monitoring
  Serial2.begin(9600, SERIAL_8N1, 16, 17);  // NEO-8M
  
  unsigned long lastLog = millis();
  
  while (true) {
    // (1) Log raw UART2 bytes every 5 seconds
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      
      int rxCount = 0, txCount = 0;
      unsigned char buffer[256];
      while (Serial2.available() && rxCount < sizeof(buffer)) {
        buffer[rxCount++] = Serial2.read();
      }
      
      Serial.printf("[DIAG] RX %d bytes @ %lu ms: ", rxCount, millis());
      for (int i = 0; i < rxCount && i < 32; i++) {
        Serial.printf("%02x ", buffer[i]);
      }
      Serial.println();
      
      // (2) Log GPS snapshot state
      auto snap = gpsReceiver.snapshot();
      Serial.printf("[GPS] inView=%d qual=%d mode=%d sats=%d hdop=%.2f lat=%.6f lon=%.6f\n",
                    snap.satellitesInView, snap.fixQuality, snap.fixMode, snap.satellitesInUse,
                    snap.hdop, snap.latitude, snap.longitude);
      
      // (3) Check PPS pin (if available)
      // GPIO pin TBD; PPS is typically on a separate pin (e.g., GPIO2)
      // If always low/high: module not running or timing issue
      
      // (4) UART framing check
      Serial.printf("[UART] baud=9600, config=8N1, available=%d\n", Serial2.available());
    }
    
    // Forward loop: keep parsing
    gpsReceiver.update();
    delay(10);
  }
}

/* Expected output patterns:

WORKING (satellites found):
[DIAG] RX 86 bytes: 24 47 50 47 47 41 2c ... (= "$GPGGA,...")
[GPS] inView=11 qual=1 mode=3 sats=8 hdop=0.90 lat=48.117300 lon=11.516667

NO FIX (AliExpress NEO-M8N clone issue):
[DIAG] RX 0 bytes
[GPS] inView=0 qual=0 mode=0 sats=0 hdop=0.00 lat=0.000000 lon=0.000000
→ Module not outputting anything. Check power, UART wiring, baud rate.

BYTES ARRIVE BUT NO SATS:
[DIAG] RX 86 bytes: 24 47 50 47 47 41 2c 2c 2c ... (= "$GPGGA,,,...") ← empty fields
[GPS] inView=0 qual=0 mode=0 sats=0 hdop=0.00
→ Module running but no fix. Check antenna, outdoor location, region settings.

PARSING WORKS BUT SNAPSHOT NEVER UPDATES:
[DIAG] RX 86 bytes: 24 47 50 47 47 41 2c...
[GPS] inView=0 qual=0 mode=0 (STAYS ZERO)
→ Parser bug or snapshot mutex issue. Check GpsReceiver::snapshot() mutex lock.
*/