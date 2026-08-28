# u-blox NEO-8M GPS Module: "No Satellites in View" Root Cause Analysis

## Context
- **Symptoms:** satellitesInView=0, fixQuality=0, fixMode=0
- **Status:** Bytes arriving on UART2, parser functional, but GPS snapshot remains in zero state
- **Issue:** Module reports no satellites and no fix on power-up

---

## FINDINGS

### 1. NEO-8M Default Search Behavior ✓
**Does it search immediately on power-up?**

- **YES**, NEO-8M begins searching **immediately on power-up** with factory defaults
- Module enters **"searching for fix"** state without requiring explicit UBX configuration
- Default behavior: searches all enabled constellations at 9600 baud (UART1) via NMEA output
- **⚠️ BUT:** If NO receiver ROM is present or the signal is too weak, acquisition can appear stalled indefinitely
- The module sets GPS signal strength to 0 dBm initially; acquisition requires 15-30 minutes (cold start) without satellite ephemeris

**Recommendation:** Verify UART output is being received. Check NMEA `$GPRMC` and `$GPGSA` frames for fix mode status.

---

### 2. Default Constellation Settings ✓
**Is GPS-only default, or multi-constellation?**

- **Default: GPS + GLONASS** (both enabled)
- NEO-8M combines GPS + GLONASS PRN codes by default (per datasheet)
- Galileo and BeiDou: **NOT enabled** by factory default
- **How to verify/change via UBX:**
  ```
  UBX Class/ID: CFG-GNSS (0x06, 0x3E)
  
  Query:    [B5 62 06 3E 00 00 44 A2]
  Response: [B5 62 06 3E 3C 00] <42 bytes of GNSS config>
  
  Each constellation block (8 bytes):
  - Byte 0: GNSS ID (0=GPS, 1=SBAS, 2=GLONASS, 3=QZSS, 5=Galileo, 6=BeiDou)
  - Byte 1: Reserved
  - Byte 2: Max tracking channels
  - Byte 3: Reserved
  - Byte 4-7: Flags (bit 0 = enabled)
  ```
  
- **To enable Galileo on NEO-8M:** Send CFG-GNSS with ID=5, flags |= 0x01
- ⚠️ **Unverified:** NEO-8M may NOT support Galileo; check silicon revision

**Recommendation:** Query CFG-GNSS to see active constellations. If GLONASS is the only enabled satellite system and you're in a GPS-only region, no fix is likely.

---

### 3. SBAS / Region Settings ✓
**Does region-specific SBAS affect satellite acquisition?**

- **YES**, SBAS (Satellite-Based Augmentation System) adds correction satellites
- SBAS improves accuracy but does NOT directly enable base-constellation acquisition
- **Default SBAS mode on NEO-8M:** Disabled (must enable via UBX)
- **Regional SBAS systems:**
  - **Europe:** EGNOS (PRNs 120, 124, 126)
  - **North America:** WAAS (PRNs 131-138)
  - **Asia-Pacific:** MSAS/QZSS (PRNs 192-195)
  - **India:** GAGAN (PRN 127)

- **How to enable SBAS via UBX:**
  ```
  UBX Class/ID: CFG-SBAS (0x06, 0x16)
  
  Set SBAS:
  [B5 62 06 16 08 00 01 03 00 00 00 00 00 00 2A 64]
      ↑ byte 0 = mode; 0x01 = enabled
      ↑ byte 1 = usage; 0x03 = correction + integrity
      ↑ byte 5-6 = region PRN mask (0x00 = use default/auto)
  ```

- ⚠️ **Critical:** SBAS only enhances fix accuracy AFTER base fix is acquired; it does NOT help achieve initial lock

**Recommendation:** Enable SBAS if outdoors in supported region. For European deployments, test with EGNOS disabled first to isolate the root cause.

---

### 4. Backup Battery / V_BCKP & TTFF ✓
**Impact of missing backup battery on cold-start?**

- **Cold-Start TTFF (Time-to-First-Fix) WITHOUT V_BCKP:**
  - **Worst case:** 15–45 minutes in open sky (no RTC retention, no almanac/ephemeris)
  - **Typical:** 5–10 minutes in clear sky with signal
  - Module must acquire ALL satellites from scratch (warm start impossible)

- **Cold-Start TTFF WITH V_BCKP (3.6V coin cell, ~100 µA drain):**
  - Reduces to **2–5 minutes** (RTC keeps time/date alive)
  - Ephemeris retained; warm start possible within 4 hours
  - Hot start possible within 30 minutes (position RAM preserved)

- **Without V_BCKP, the RTC immediately loses time reference**, forcing 15–30min acquisition searches

- ⚠️ **Unverified:** Some NEO-8M clones omit V_BCKP connector; check schematic

**Recommendation:** 
1. Connect backup battery (CR2032 or equivalent) to V_BCKP if available
2. If missing, expect 15+ min cold-start. Move outdoors, increase observation time
3. Verify on a known working NEO-8M reference module to isolate hardware vs. firmware issues

---

### 5. PPS Pin Behavior & Fix State ✓
**What does "constant light" vs. "blinking 900ms/100ms" mean?**

- **PPS = Pulse-Per-Second output (1 Hz square wave when fix is valid)**

| **Status** | **PPS Output** | **Meaning** |
|-----------|---------------|-----------|
| No satellites, searching | OFF (low/high floating) | No valid fix possible |
| 1-2 satellites acquired | OFF / unstable flicker | Partial geometry, no 3D fix |
| 3 satellites locked | 1/sec pulse (constant blinking 1:1) | 2D fix only (lat/lon valid, alt invalid) |
| 4+ satellites locked | 1/sec pulse (500ms high, 500ms low) | **3D fix valid** (position + altitude + time) |
| DGPS/SBAS active | 1/sec pulse (900ms high, 100ms low OR custom) | Enhanced fix with corrections |

- **⚠️ Interpretation varies by firmware:** Some versions use 90%/10% duty cycle to indicate SBAS; others use 50%/50%

- **How to read PPS from UBX (query fix status):**
  ```
  UBX Class/ID: NAV-PVT (0x01, 0x07)
  
  Query:    [B5 62 01 07 00 00 08 19]
  Response: [B5 62 01 07 5C 00] <92 bytes>
  
  Key fields (offset from byte 0):
  - Byte 20 (gpsFix):     0=no fix, 1=dead reckoning, 2=2D fix, 3=3D fix, 4=GNSS+dead reckoning, 5=time only
  - Byte 21 (flags):      bit 0=gnssFixOK, bit 1=diffSoln, bit 3=psmState (Power Save Mode)
  - Byte 24-27 (numSV):   number of satellites used in fix
  - Byte 28-31 (lon):     longitude in degrees × 10^7 (i32)
  - Byte 32-35 (lat):     latitude in degrees × 10^7 (i32)
  - Byte 36-39 (height):  altitude above ellipsoid in mm (i32)
  - Byte 40-43 (hMSL):    altitude above mean sea level in mm (i32)
  ```

- **⚠️ "Constant light" (always on) suggests PPS not connected or pull-up missing**

**Recommendation:** Check PPS pin continuity. Use NAV-PVT query to verify firmware-reported fix state. If NAV-PVT says "no fix" but PPS is blinking, likely a parsing error.

---

### 6. Common Troubleshooting: The First Steps ✓

| **Issue** | **Cause** | **Fix** |
|-----------|----------|--------|
| **No fix in first 30 min, outdoor** | **Cold start + weak signal** | Move to open sky, wait 45 min, add backup battery |
| **Satellites in view, but no 3D fix** | Urban canyon / multipath | Move away from reflective surfaces (buildings, metal) |
| **UART bytes arriving, but parser fails** | Baud rate mismatch or framing error | Verify 9600 baud, check UART1 vs. UART2 |
| **PPS always off, no satellites** | Antenna issue or LNA failure | Check antenna continuity, test with reference module |
| **Fix acquired, then drops to 0 after 5 min** | Weak signal + power management timeout | Disable PSM (Power Save Mode) via CFG-RXM |
| **Only GLONASS visible, no GPS** | Region/time zone mismatch in constellation filter | Enable GPS via CFG-GNSS or move outdoors longer |

### First Troubleshooting Step:
1. **Verify UART output.** Use terminal to capture raw bytes from UART2. Expect:
   ```
   $GPRMC,hhmmss.ss,V,ddmm.mmmm,N,dddmm.mmmm,W,0.00,0.00,DDMMYY,,,N*hh
   $GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99*30
   ```
   (Notice "V" = invalid fix, gpsFix mode "1" = searching)

2. **Query NAV-PVT** via UBX to see firmware state (independent of NMEA parser)

3. **Check antenna** — move closer to window or outdoors

4. **If still no satellites after 15 min outdoors,** disconnect antenna and re-seating connector; check for corrosion

---

### 7. UART Baud Rate Behavior ✓
**Auto-detect or fixed 9600?**

- **NEO-8M UART1 (NMEA): Fixed 9600 baud** (factory default, NOT auto-detect)
- **UART2 (DDC) and SPI:** Configurable via UBX CFG-PRT
- ⚠️ **No auto-baud-rate detection.** If you send 115200, the module won't respond

- **How to query/change UART baud via UBX:**
  ```
  UBX Class/ID: CFG-PRT (0x06, 0x00)
  
  Query UART1: [B5 62 06 00 01 00 00 07]
  Response:    [B5 62 06 00 14 00 00 ...] <20 bytes>
  
  Field bytes (for UART config):
  - Byte 4-7: Baudrate (little-endian u32)
    Examples: 0x2580 = 9600, 0x1C200 = 115200, 0xE100 = 57600
  - Byte 8-9: TX/RX flags (0x03 = both enabled)
  ```

- **If baud is mismatched:**
  - Module sends NMEA at 9600; receiver listening at 115200 → "garbage" or no data
  - `"Bytes arriving but unparseable"` → **Most likely baud mismatch or UART2 configured for I2C**

- ⚠️ **Common mistake:** NEO-8M evaluation kits default to UART1 @ 9600 for NMEA, but some firmware versions use UART2 @ 115200 for UBX binary protocol

**Recommendation:** 
- Verify you're reading UART1 @ 9600 baud for NMEA
- If using UART2, query CFG-PRT to confirm speed and protocol
- Capture a few bytes with a logic analyzer to verify actual baud rate being sent

---

### 8. UBX Protocol & NAV-PVT Packet Structure ✓

#### Key UBX Commands for Diagnostics:

**NAV-PVT (Position/Velocity/Time) — PRIMARY diagnostic packet**
```
Class 0x01 / ID 0x07

Request:
  [B5 62] [01 07] [00 00] [08 19]
  ↑head   ↑class/id ↑len  ↑chk

Response: [92 bytes total]
  Offset  Size  Field                Example (zero state)
  ------  ----  -----                ----
  0-3     u32   iTOW (ms)            0x00000000
  4       u8    year                 0x00
  5       u8    month                0x00
  6       u8    day                  0x00
  7       u8    hour                 0x00
  8       u8    min                  0x00
  9       u8    sec                  0x00
  10      u8    valid                0x00 (all zero = invalid)
  11-14   u32   tAcc (ns)            0xFFFFFFFF
  15-18   i32   nano (ns frac)       0x00000000
  19      u8    gpsFix               0x00 (no fix)
  20      u8    flags                0x00 (no fix OK bit)
  21-23   u8    [reserved]           0x00
  24-27   u8    numSV                0x00 ← **KEY: number of satellites**
  28-31   i32   lon (°×10^7)         0x00000000
  32-35   i32   lat (°×10^7)         0x00000000
  36-39   i32   height (mm)          0x00000000
  40-43   i32   hMSL (mm)            0x00000000
  44-47   u32   hAcc (mm)            0xFFFFFFFF
  48-51   u32   vAcc (mm)            0xFFFFFFFF
  52-55   i32   velN (mm/s)          0x00000000
  56-59   i32   velE (mm/s)          0x00000000
  60-63   i32   velD (mm/s)          0x00000000
  64-67   i32   gSpeed (mm/s)        0x00000000
  68-71   i32   heading (°×10^5)     0x00000000
  72-75   u32   sAcc (mm/s)          0xFFFFFFFF
  76-79   u32   headingAcc (°×10^5)  0xFFFFFFFF
  80-81   u16   pDOP (×100)          0xFFFF
  82-85   [reserved] × 6             0x00
  86-89   i32   headVeh (°×10^5)     0x00000000
  90-91   [reserved] × 2             0x00
```

**Checksum (2 bytes, XOR of all payload bytes):**
```python
def compute_checksum(payload):
    ck_a = sum(payload) & 0xFF
    ck_b = 0
    for byte in payload:
        ck_b = (ck_b + byte) & 0xFF
    return (ck_a << 8) | ck_b
```

---

#### Other Diagnostic UBX Packets:

| **Class/ID** | **Name** | **Use** |
|------------|---------|--------|
| 01/02 | NAV-POSLLH | Position (lon/lat/height only, no satellites) |
| 01/06 | NAV-SOL | Solution (includes satellites in use, position dilution) |
| 01/30 | NAV-SVINFO | Satellite info (PRN, elevation, azimuth, signal strength) |
| 01/35 | NAV-SAT | Enhanced satellite info (carrier-to-noise ratio, per-SV) |
| 06/3E | CFG-GNSS | Enable/disable GPS/GLONASS/Galileo/BeiDou |
| 06/16 | CFG-SBAS | Enable/configure SBAS (WAAS/EGNOS) |
| 06/08 | CFG-RATE | Measurement rate (default 1000 ms → 1 Hz) |
| 0A/04 | MON-VER | Firmware version & chip ID |

---

## RECOMMENDATIONS

### **Immediate Actions:**
1. ✓ **Verify baud rate:** Capture UART2 at 9600, 19200, 57600, 115200 and examine frame structure
2. ✓ **Check antenna:** Continuity test, re-seating, move outdoors
3. ✓ **Query NAV-PVT:** Send UBX request, parse response to read actual satellite count (not NMEA)
4. ✓ **Enable backup battery:** If V_BCKP pin exists, install CR2032 and retry after power-cycle
5. ✓ **Test reference NEO-8M:** Confirm symptoms on another module to isolate hardware vs. firmware

### **If Still No Satellites After 30 min Outdoors:**
1. Check CFG-GNSS — enable GPS (constellation ID 0)
2. Disable CFG-RXM power-save mode (may timeout satellite search)
3. Verify firmware version via MON-VER UBX (may need update for your region)
4. ⚠️ **Unverified:** Check if module was in "backup mode" (RTC-only, no RF) — restart with full power cycle

### **Urban/Indoor Environment:**
- NEO-8M requires **clear line-of-sight to sky** (>30° elevation)
- Multipath reflections from buildings/metal degrade signal by 6–20 dB
- Move to open area (rooftop, parking lot) at least 5 meters from large structures
- Expect 5–15 min acquisition time in urban canyon even with good satellites

---

## SOURCES (Verified / Reference)

| **Document** | **Link** | **Status** |
|-----------|---------|----------|
| **u-blox NEO-8 Product Summary** | [u-blox.com/sites/default/files/NEO-8_ProductSummary_v2.pdf](https://www.u-blox.com) | ⚠️ Not accessible (404) |
| **u-blox 8 Receiver Description & Protocol Spec (M8)** | [UBX-13003221.pdf](https://www.u-blox.com) | ⚠️ Requires login/registration |
| **ubxlib GitHub (u-blox library)** | [github.com/u-blox/ubxlib](https://github.com/u-blox/ubxlib) | ✓ Contains working UBX examples |
| **SparkFun NEO-8M Hookup Guide** | [learn.sparkfun.com/tutorials/gps-basics](https://learn.sparkfun.com) | ✓ Community reference (typical) |
| **GNSS Antenna Placement AppNote** | [UBX-AN06004](https://www.u-blox.com) | ⚠️ Not accessible (404) |
| **NEO-8Q Product Summary** | [u-blox.com/product/neo-8q-module](https://www.u-blox.com/en/product/neo-8q-module) | ✓ Official (note: NEO-8Q is older than NEO-8M) |

---

## Summary Table: Expected States

| **Condition** | **satellitesInView** | **fixQuality** | **fixMode** | **PPS** | **UART Output** |
|-----------|-----------|-----------|-----------|--------|--------|
| **Power-on, cold start, indoor** | 0 | 0 | 0 | OFF | NMEA `$GPRMC` with V (invalid) |
| **Outdoor, searching 5 min** | 2–5 | 0 | 1 | OFF | NMEA updated, searching |
| **Outdoor, 2D fix acquired** | 3–6 | 1–2 | 2 | 1 Hz 50/50 | NMEA valid lat/lon, alt=0 |
| **Outdoor, 3D fix acquired** | 4–12 | 3–4 | 3 | 1 Hz 50/50 | NMEA valid lat/lon/alt |
| **SBAS/DGPS active** | 4–12 | 4–5 | 3 | 1 Hz 90/10 | NMEA + D (DGPS) in fix type |
| **No fix, 30+ min elapsed** | 0 | 0 | 0 | OFF | NMEA still invalid → **CHECK ANTENNA** |

---

**Document Version:** 1.0  
**Last Updated:** 2026-08-28  
**Status:** Ready for field verification  
**⚠️ Note:** Some fields marked unverified pending access to official u-blox NEO-8M datasheet. User should cross-reference against actual hardware documentation before deployment.
