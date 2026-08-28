# NEO-8M Quick Troubleshooting Flowchart

## Symptom: satellitesInView=0, fixQuality=0, fixMode=0

### STEP 1: Verify Communication (< 2 min)
```
[ ] Capture UART2 bytes for 10 seconds
    → Use logic analyzer or serial terminal (minicom/screen)
    → At what baud rate?
       • 9600  → NMEA output (expected default)
       • 115200 → Binary UBX (non-standard config)
       • Nothing → Serial path broken

[ ] Expected NMEA frames:
    $GPRMC,hhmmss.ss,V,0000.0000,N,00000.0000,W,0.00,0.00,DDMMYY,,,N*hh
                      ↑
                  "V" = Invalid (no fix yet)
                  "A" = Valid (fix acquired)

ACTION:
  ❌ No bytes → check UART2 wiring, TX/RX swapped, or wrong serial port
  ✓ Bytes at 9600 with "V" frames → module searching (NORMAL, proceed to STEP 2)
  ✓ Bytes at 115200 → non-standard baud; verify CFG-PRT via UBX
```

### STEP 2: Query Firmware State via UBX NAV-PVT (< 1 min)
```
Send: [B5 62] [01 07] [00 00] [08 19]

Parse response (byte 24-27):
  numSV = 0x00 → No satellites acquired
  numSV > 0x03 → Satellites locked; check gpsFix (byte 19)
    - gpsFix=0 → Not enough satellites
    - gpsFix=2 → 2D fix (latitude/longitude valid)
    - gpsFix=3 → 3D fix (lat/lon/altitude valid)

ACTION:
  ❌ numSV=0, waiting >30 min outdoors → STEP 3 (antenna issue)
  ⚠️  numSV=1-2 → Weak signal; move to open sky
  ✓ numSV≥3, gpsFix=2-3 → Fix acquired! (parser error in your app)
```

### STEP 3: Antenna & Signal Path (5-10 min)
```
[ ] Visual inspection
    • Antenna connected securely to SMA/u.FL connector?
    • Any bent pins or corrosion on connector?
    • Is antenna type active (powered) or passive?
      ✓ Active antenna needs +5V bias; check power supply
      ❌ Passive antenna should NOT be powered
      
[ ] Continuity test
    • Use multimeter to test resistance between antenna connector center and ground
    • Expect: Open circuit (∞ Ω) for antenna, <1 Ω for ground plane

[ ] Test location
    • Move outdoors, away from large metal structures
    • Clear view of southern sky (hemisphere facing equator)
    • Wait 10-15 minutes for initial fix acquisition

[ ] Backup test
    • Borrow reference NEO-8M module with known-good antenna
    • Test in same location → fixes the antenna, or does it NOT fix numSV=0?
    • If reference works → your antenna is bad
    • If reference ALSO shows numSV=0 → environmental/firmware issue

ACTION:
  ✓ Reference module acquires fix → replace antenna & test
  ❌ Reference module ALSO fails → proceed to STEP 4
```

### STEP 4: Backup Battery & Cold-Start Timing (15-45 min patience)
```
[ ] Check V_BCKP pin
    • Is there a CR2032 battery holder on your board?
    • If YES and empty → install battery, power-cycle module, wait 30 min
    • If NO → expect 15-30 min cold-start (no ephemeris cache)

[ ] Verify firmware version
    • Send UBX: [B5 62] [0A 04] [00 00] [0E 34]
    • Response: MON-VER (check hwVersion, swVersion fields)
    • Compare against u-blox website for your region

[ ] Power supply ripple
    • Check VDDIO/VDD voltage with oscilloscope
    • Should be stable 3.3V ±5% with <100 mV peak-to-peak noise
    • Noisy power can prevent RF acquisition

ACTION:
  ✓ After 30 min outdoors with backup battery → go to STEP 5
  ⚠️  Still numSV=0 after 45 min → likely antenna or region mismatch
```

### STEP 5: Constellation & Region Settings (5-10 min)
```
Query CFG-GNSS:
  Send: [B5 62] [06 3E] [00 00] [44 A2]
  Response: [B5 62] [06 3E] [3C 00] <42 bytes>
  
  Parse GNSS blocks (each 8 bytes):
  - Byte 0 = GNSS ID:  0=GPS, 2=GLONASS, 3=QZSS, 5=Galileo, 6=BeiDou
  - Byte 4 (flags) & 0x01 = enabled?
  
Expected for NEO-8M:
  [ ] GPS (ID=0) enabled?
  [ ] GLONASS (ID=2) enabled?
  [ ] Galileo (ID=5) likely disabled (N/A for NEO-8M)

ACTION:
  ❌ GPS disabled → enable GPS: send CFG-GNSS with ID=0, flags |= 0x01
  ⚠️  GLONASS only, no GPS → region mismatch; enable GPS
  ✓ Both GPS & GLONASS enabled → proceed to STEP 6
```

### STEP 6: SBAS & Regional Augmentation (5 min)
```
[ ] Check SBAS enabled
    Send: [B5 62] [06 16] [00 00] [22 DC] (query)
    Response byte 0 = mode; 0x01 = enabled, 0x00 = disabled

[ ] If outdoor in Europe/USA/Asia, enable SBAS
    Send: [B5 62] [06 16] [08 00] [01 03 00 00 00 00 00 00 2A 64]
                                  ↑ mode=1 (enabled), usage=3 (correction+integrity)

[ ] Supported SBAS systems
    • Europe: EGNOS (PRNs 120, 124, 126)
    • USA: WAAS (PRNs 131-138)
    • Japan: MSAS (PRNs 192-195)
    • India: GAGAN (PRN 127)

ACTION:
  ✓ Enable SBAS if outdoors → re-test fix acquisition
  ⚠️  SBAS does NOT enable base GPS; only improves accuracy after 3D fix acquired
```

---

## Decision Tree

```
                        ┌─ Start: numSV=0, fixQuality=0, fixMode=0
                        │
        ┌───────────────┴────────────────┐
        ▼                                 ▼
    UART Bytes?                    No bytes?
        │                              │
     YES│                              │NO
        ▼                              ▼
   Baud 9600?                   CHECK UART2:
        │                      - TX/RX wired?
     YES│                      - Baud rate?
        ▼                      - Electrical levels (3.3V)?
    Query NAV-PVT                   │
        │                          ▼
        ├─ numSV>3 ──→ Fix acquired  CHECK LOGIC ANALYZER
        │              (parser error?)    for timing/protocol
        │
        └─ numSV=0
           │
           ├─ Indoor, <5 min
           │  └─→ NORMAL (cold start)
           │       Move outdoors, wait 20-30 min
           │
           └─ Outdoor, >30 min
              │
              ├─ Antenna visible/connected?
              │  └─ NO ──→ Reseat/replace antenna
              │  └─ YES ──→ Continuity test
              │             (expect ∞ Ω, not shorted)
              │
              ├─ V_BCKP battery installed?
              │  └─ NO ──→ Install CR2032, retry after 30 min
              │  └─ YES ──→ Query CFG-GNSS
              │
              └─ CFG-GNSS shows GPS disabled?
                 └─ YES ──→ Enable GPS, wait 30 min retry
                 └─ NO ──→ Contact support (likely hardware defect)
```

---

## Common Issues & Fixes

| **Symptom** | **Most Likely Cause** | **Fix** |
|-----------|-----------|--------|
| numSV=0, indoor | Cold start (normal) | Move outdoors, wait 15-30 min |
| numSV=0, outdoor >30 min | Antenna disconnected/shorted | Test continuity, reseat connector |
| numSV=0, V_BCKP empty | Extended cold start | Install backup battery, power-cycle |
| numSV=0, no UART bytes | Serial port wiring error | Check TX/RX polarity, voltage levels |
| numSV=0, wrong baud rate | UART configuration mismatch | Query CFG-PRT, set to 9600 |
| numSV=3 but no 3D fix | Weak vertical geometry | Move to open sky away from buildings |
| numSV=0, GPS disabled | Constellation misconfiguration | Enable GPS via CFG-GNSS |
| numSV=0, power supply noisy | RF noise injection | Add 100µF + 10µF capacitors near VDD |

---

## UBX Command Reference (Quick Paste)

```
Query NAV-PVT (position/time/velocity):
  [B5 62 01 07 00 00 08 19]

Query CFG-GNSS (constellations):
  [B5 62 06 3E 00 00 44 A2]

Query CFG-SBAS (augmentation):
  [B5 62 06 16 00 00 22 DC]

Query MON-VER (firmware version):
  [B5 62 0A 04 00 00 0E 34]

Enable GPS in CFG-GNSS:
  [B5 62 06 3E 3C 00 00 00 08 10 00 00 00 00 00 01 01 01 00 00 00 00 01 01 01 00 00 00 00 01 03 08 00 00 00 00 00 01 01 01 00 00 00 00 00 00 00 00 00 00 00 00 01 01 A8 24]
  (Note: bytes 4-35 = GPS constellation enable, adjust as needed)

Enable SBAS:
  [B5 62 06 16 08 00 01 03 00 00 00 00 00 00 2A 64]
```

---

**Last Updated:** 2026-08-28  
**Status:** Companion to NEO8M_GPS_RESEARCH.md  
**Use:** When fielding GPS acquisition failures
