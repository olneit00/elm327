# NEO-8M "Kein Fix" - Troubleshooting-Checkliste (vorab)

## Symptom
- `satellitesInView = 0`
- `fixQuality = 0`
- `fixMode = 0`
- **Aber:** Bytes kommen auf UART2 an (NMEA-Parser aktiv)
- Letzter GPS-Snapshot bleibt auf Nullen

## Sofort-Checks (vor Recherche)

### 1. Bytes / NMEA-Struktur
**Test:** Raw-Bytes vom Serial2 auf Monitorausgeben (hex + ASCII)
```
[DIAG] RX 86 bytes: 24 47 50 47 47 41 2c 2c 2c ... 
       ASCII: $ G P G G A , , , ...
```
- **Wenn 0 bytes:** Module sendert nicht → UART-Wiring, Power, Baud
- **Wenn bytes aber leere Felder:** Module läuft aber kein Fix → Antenne/Region

### 2. Module Power + Status
- Breakout **LED**: PPS-Pin
  - Dauerlicht = kein Fix (sucht noch)
  - Blinkt (900ms/100ms) = Fix da
  - **Falls dunkel/nie blinkt:** Modul startet nicht oder kein Strom

### 3. Antenne + Outdoor
- NEO-8M braucht **klare Himmelssicht** (min. 30° Elevation)
- Indoor/Fenster oft kein Fix
- Antenne sollte **nach oben zeigen**, nicht horizontal

### 4. Region / SBAS
- ⚠️ **Hyperwahrscheinlichkeit:** AliExpress-Clone hat Regional-Lock oder SBAS deaktiviert
- Norden Europa: SBAS = EGNOS (Satellites 120–158)
- Testen: Mit **UBX-Befehl** Region/Constellation explizit setzen (research wartet)

### 5. Baud-Rate / UART
- NEO-8M Default: **9600 Baud, 8N1**
- Code: `Serial2.begin(9600, SERIAL_8N1, 16, 17)` ✓ korrekt
- **Wenn Baud falsch:** NMEA-Zeilen unleserlich (aber raw bytes könnten ankommen)

### 6. Cold-Start TTFF
- **Ohne Backup-Batterie:** cold-start = 30–60 s
- **Mit V_BCKP (z.B. Kondensator):** warm-start = 5–10 s
- Erste Messung kann lange dauern — **warten!**

### 7. Parser / Snapshot-Mutex
- GpsReceiver::processByte() → aktualisiert snap_ mit Mutex
- Wenn snapshot() **nie updated**: Mutex-Deadlock oder Parser-Bug
- **Test:** Verschiedenes Ausgabe-Format (siehe DIAGNOSTIC_GPS_NO_FIX.cpp)

## Nächster Schritt
Recherche-Ergebnis abwarten → UBX-Config, Region-Bits, explizite Search-Mode-Kommandos
