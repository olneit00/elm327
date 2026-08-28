# Konzept: GPS-Daten von motor-node auf CAN-Bus
## Automotive CAN-Architektur für elm327

### Überblick
Der **motor-node** (ESP32 mit NEO-8M GPS) soll Positionsdaten auf den **CAN-Bus** legen.
Die **headunit** (mit ELM327-Modul über BLE) liest diese Daten vom CAN-Bus.
Der ELM327 fungiert als "OBD-Gateway" — simuliert / proxyt die CAN-Kommunikation.

### 1. Automotive CAN-Architektur (DBC-Standard)

#### 1.1 CAN-Bus-Topologie (elm327-Projekt)
```
┌─────────────────────────────────────────┐
│       Vehicle CAN-Bus (ISO 11898)       │ ← physikalisch (2x twisted pair)
│         Baud: 500 kBit/s (typisch)      │
└─────────────────────────────────────────┘
         ↓                    ↓
  ┌─────────────┐      ┌──────────────┐
  │ motor-node  │      │ headunit     │
  │ (CAN-TX)    │      │ (CAN-RX)     │
  │ GPS → CAN   │      │ CAN → LVGL9  │
  └─────────────┘      └──────────────┘
         ↓ BLE              ↓ WiFi-AP
   (optional: ELM327-emulator simuliert auch Abfragen)
```

#### 1.2 DBC-Datei (CAN-Signal-Definition)
Standard im Automotive: `elm327.dbc` (oder `gps.dbc` als Subset)

**Nachricht-IDs und Zyklzeiten:**
| ID | Name | Zyklus | Inhalte |
|-----|------|--------|---------|
| 0x100 | GPS_Position | 2 s | Latitude + Longitude (1e-7 °) |
| 0x101 | GPS_Fix | 2 s | Fix-Quality, Fix-Mode, Sat-Zählungen, DOPs |
| 0x102 | GPS_Velocity | 2 s | Speed (km/h), Course (°), Altitude MSL (m) |
| 0x103 | GPS_Time | 30 s | UTC Hour/Min/Sec, Day/Month/Year |
| 0x104 | GPS_Satellites | 5 s | PRN, Elevation, Azimuth, SNR (1. Satellit; weitere IDs für weitere Sats) |
| 0x105 | GPS_Status | 5 s | Valid, ActiveFix, PPS-Lock, ChecksumErrors, LineAge |

### 2. motor-node CAN-TX-Implementation

#### 2.1 Schnittstellenänderungen
**Neuer Code:** `motor-node/include/CanManager.h` + `motor-node/src/CanManager.cpp`
- **CAN-Initialisierung** (ESP32 GPIO4/5 = CAN-TX/RX oder beliebig per Konfiguration)
- **Periodischer Tx-Loop** (aktueller GPS-Snapshot via `GpsReceiver::snapshot()` → CAN-Nachrichten)

#### 2.2 Codestub (motor-node/main.cpp Änderung)
```cpp
#include "CanManager.h"

CanManager canMgr(4, 5);  // TX=GPIO4, RX=GPIO5; baud=500kbps default

void setup() {
  canMgr.begin();  // init CAN bus
  gpsReceiver.begin();
  gpsWebServer.begin();
}

void loop() {
  // ... existing code (Elm327Server, WiFi AP, NimBLE) ...
  
  // NEW: update GPS from UART2
  gpsReceiver.update();
  
  // NEW: send GPS data on CAN bus (periodisch)
  canMgr.updateAndTransmit(gpsReceiver.snapshot());
}
```

### 3. headunit CAN-RX-Implementation

#### 3.1 Schnittstellenänderungen
**Neuer Code:** `headunit/include/CanBusReceiver.h` + `headunit/src/CanBusReceiver.cpp`
- **CAN-Initialisierung** (gleiche GPIO wie motor-node oder andere, je nach Wiring)
- **RX-Interrupt-Handler** (auf CAN-Messages lauschen, GPS-Struktur in VehicleState eintragen)

#### 3.2 VehicleState-Erweiterung
```cpp
struct GpsData {
  double latitude, longitude;
  uint8_t fixQuality, fixMode;
  uint8_t satellitesInUse, satellitesInView;
  float hdop, vdop;
  float speedKmh, courseDeg, altitudeMsl;
  uint8_t utcHour, utcMin, utcSec;
  uint8_t day, month;
  uint16_t year;
  bool valid, activeFix;
  uint32_t lastUpdateMs;
};

class VehicleState {
public:
  GpsData gpsData;  // new field
  // ... existing fields ...
};
```

#### 3.3 LVGL9-UI-Integration
**Ziel:** GPS-Anzeige auf headunit (z.B. "Position", "Satellites", "Fix-Qualität")
- Separate GPS-Seite in der UI (oder Widget in bestehendem Dashboard)
- Polling vom `VehicleState.gpsData` (Thread-safe Copy)
- Live-Update alle 2–5 s

### 4. ELM327-Protokoll-Integration (optional)

#### 4.1 OBD-ähnliche GPS-Abfragen
Der **ELM327** (motor-node-Seite, in `Elm327Server.h`) könnte auch GPS-Daten "simuliert" abfragen:
```
AT+GPS?                   // Anfrage: "GPS valid?"
AT+GPOS?                  // "Position in Dezimalgraden?"
AT+GSATS?                 // "Satellitenzahl?"
```

Die headunit (über BLE) könnte diese Abfragen machen, erhält dann GPS-Daten ohne CAN zu benötigen.
**Zweck:** Redundanz + Test-Fallback, falls CAN-Bus ausfällt.

### 5. Hardware-Wiring (CAN)

#### 5.1 ESP32 CAN (Standard: GPIO 4/5)
```
motor-node:
  GPIO4  (TWAI_TX)  → CAN-H (via transceiver TJA1050 / MCP2551)
  GPIO5  (TWAI_RX)  ← CAN-H (via transceiver)
  GND    → CAN-L
  
headunit:
  GPIO4  (TWAI_TX)  → CAN-H (same bus)
  GPIO5  (TWAI_RX)  ← CAN-H (same bus)
  GND    → CAN-L
  
CAN-Transceiver (z.B. TJA1050, SN65HVD230):
  D (Driver out)   → CAN-H
  GND              → CAN-L (Referenz)
  RX (Receiver in) ← CAN-H
```

#### 5.2 Termination
- CAN-Bus braucht **zwei 120 Ω Widerstände** an den Enden (z.B. auf motor-node + headunit)

### 6. Roadmap-Phasen

**Phase 1 (aktuell):** GPS im motor-node auslesen + Web-Seite ✅ (PR #12, gemergt)

**Phase 2 (nächste Issues):** 
- Issue #7: CAN-Bus-Architektur / DBC definieren
- Issue #8: motor-node GPS → CAN
- Issue #9: headunit CAN → UI
- Issue #10: ELM327 GPS-Abfragen (optional)
- Issue #11: Hardware-Wiring / Schaltplan

**Phase 3 (später):** CAN-Fehlerbehandlung, UI-Anzeige-Optimierung

---

## Automotive-Standards (Referenz)

- **ISO 11898-1:** CAN 2.0 Spezifikation (500 kBps typisch in Fahrzeugen)
- **DBC-Format:** Vehicle Communication Toolbox (Vector, open-source support in `cantools`)
- **OBD-2:** On-Board Diagnostics (PID, CAN-basiert); GPS ist nicht Standard, aber vendor-specific nutzbar
- **ELM327:** OBD-to-Serial/BLE Gateway; kann auch Roh-CAN-Frames relayed