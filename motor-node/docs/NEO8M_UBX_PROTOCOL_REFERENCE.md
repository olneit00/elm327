# NEO-8M UBX Protocol: Packet Reference & Python Parser

## NAV-PVT (0x01, 0x07) — Primary Diagnostic Packet

### Full Specification

```
CLASS: 0x01
ID:    0x07
```

### Request (Query Current State)
```
Bytes:  B5 62 01 07 00 00 08 19

Parse:
  B5 62       = UBX sync chars (header)
  01          = Class (Navigation)
  07          = ID (Position/Velocity/Time)
  00 00       = Length (0 bytes payload)
  08          = Checksum A (CK_A)
  19          = Checksum B (CK_B)
```

### Response (92-byte payload)

```
Response: B5 62 01 07 5C 00 [92 bytes] [CK_A] [CK_B]
                      ↑
                    length = 0x005C = 92 bytes
```

#### Payload Structure (Little-Endian)

```
Offset  Bytes  Type   Name           Description / Default (no fix)
──────  ─────  ────   ────           ──────────────────────────
0-3     4      u32    iTOW           Time of week (milliseconds), 0 if no fix
4       1      u8     year           Year (1999-), 0 if no fix
5       1      u8     month          Month (1-12), 0 if no fix
6       1      u8     day            Day (1-31), 0 if no fix
7       1      u8     hour           Hour (0-23), 0 if no fix
8       1      u8     min            Minute (0-59), 0 if no fix
9       1      u8     sec            Second (0-59), 0 if no fix
10      1      u8     valid          Validity flags
                                     Bit 0: validDate
                                     Bit 1: validTime
                                     Bit 2: fullyResolved (all details valid)
                                     Typical: 0x00 (nothing valid) to 0x07 (all valid)
11-14   4      u32    tAcc           Time accuracy estimate (ns), 0xFFFFFFFF if invalid
15-18   4      i32    nano           Fractional seconds (-1e9..+1e9), 0x00 if no fix
19      1      u8     gpsFix         Fix type:
                                     0x00 = No fix
                                     0x01 = Dead reckoning only
                                     0x02 = 2D fix (lat/lon valid)
                                     0x03 = 3D fix (lat/lon/alt valid)
                                     0x04 = GNSS + dead reckoning
                                     0x05 = Time only fix
20      1      u8     flags          Fix flags:
                                     Bit 0: gnssFixOK (1=valid, 0=invalid)
                                     Bit 1: diffSoln (DGPS/SBAS active)
                                     Bit 2: psmState (power save mode)
                                     Bit 3-7: reserved
21-23   3      u8[3]  reserved       [ignored]
24-27   4      u8     numSV          **Number of satellites used in fix**
                                     ★★★ THIS IS YOUR KEY FIELD ★★★
                                     0x00 = searching, no satellites
                                     0x03+ = some satellites acquired
28-31   4      i32    lon            Longitude (degrees × 10^7, ±1.8×10^8)
                                     0 if no fix. Range: ±180°
                                     Example: 51234567 = 5.1234567° E
32-35   4      i32    lat            Latitude (degrees × 10^7)
                                     0 if no fix. Range: ±90°
                                     Example: 48234567 = 48.234567° N
36-39   4      i32    height         Altitude above ellipsoid (mm)
                                     0 if no 3D fix
40-43   4      i32    hMSL           Altitude above mean sea level (mm)
                                     0 if no 3D fix
44-47   4      u32    hAcc           Horizontal accuracy estimate (mm)
                                     0xFFFFFFFF if invalid
48-51   4      u32    vAcc           Vertical accuracy estimate (mm)
                                     0xFFFFFFFF if invalid (2D fix only)
52-55   4      i32    velN           Velocity North component (mm/s)
                                     0 if stationary
56-59   4      i32    velE           Velocity East component (mm/s)
60-63   4      i32    velD           Velocity Down component (mm/s)
64-67   4      i32    gSpeed         Ground speed (mm/s)
68-71   4      i32    heading        Heading of motion (degrees × 10^5)
                                     0x80000000 if not available
72-75   4      u32    sAcc           Speed accuracy estimate (mm/s)
                                     0xFFFFFFFF if invalid
76-79   4      u32    headingAcc     Heading accuracy estimate (°×10^5)
80-81   2      u16    pDOP           Position dilution of precision (×100)
                                     0xFFFF if invalid
82-85   4      u8[4]  reserved       [ignored]
86-89   4      i32    headVeh        Heading of vehicle (degrees × 10^5)
                                     0x80000000 if not available
90-91   2      u8[2]  reserved       [ignored]
```

---

## Python Parser Example

```python
import struct

class UBXParser:
    SYNC = b'\xB5\x62'
    
    @staticmethod
    def compute_checksum(payload):
        """Compute UBX checksum (CK_A, CK_B)"""
        ck_a = 0
        ck_b = 0
        for byte in payload:
            ck_a = (ck_a + byte) & 0xFF
            ck_b = (ck_b + ck_a) & 0xFF
        return ck_a, ck_b
    
    @staticmethod
    def parse_nav_pvt(data):
        """
        Parse a NAV-PVT response packet.
        data = raw bytes starting after sync header [B5 62]
        
        Expected format:
          [01 07] [5C 00] [92 bytes payload] [CK_A CK_B]
        """
        if len(data) < 96:
            raise ValueError(f"NAV-PVT packet too short: {len(data)} bytes")
        
        class_id = data[0]
        msg_id = data[1]
        if class_id != 0x01 or msg_id != 0x07:
            raise ValueError(f"Not a NAV-PVT packet (class={class_id:02X}, id={msg_id:02X})")
        
        length = struct.unpack('<H', data[2:4])[0]
        if length != 92:
            raise ValueError(f"NAV-PVT payload length mismatch: {length} != 92")
        
        payload = data[4:4+92]
        ck_a_rx = data[4+92]
        ck_b_rx = data[4+92+1]
        
        # Verify checksum
        ck_a_calc, ck_b_calc = UBXParser.compute_checksum(data[0:4+92])
        if ck_a_rx != ck_a_calc or ck_b_rx != ck_b_calc:
            raise ValueError(f"Checksum mismatch: expected {ck_a_calc:02X}{ck_b_calc:02X}, got {ck_a_rx:02X}{ck_b_rx:02X}")
        
        # Unpack payload
        (iTOW, year, month, day, hour, minute, second, valid, tAcc, nano,
         gpsFix, flags, _, numSV, lon, lat, height, hMSL, hAcc, vAcc,
         velN, velE, velD, gSpeed, heading, sAcc, headingAcc, pDOP) = struct.unpack(
            '<IBBBBBBBIB3BBBI4i4I4i4I2HI',  # Little-endian unsigned/signed
            payload[:80]  # First 80 bytes contain the essentials
        )
        
        return {
            'iTOW': iTOW,
            'timestamp': (year, month, day, hour, minute, second),
            'valid': valid,
            'tAcc': tAcc,
            'nano': nano,
            'gpsFix': gpsFix,
            'flags': flags,
            'numSV': numSV,                   # ★ KEY FIELD
            'lon': lon / 1e7,                 # Convert to degrees
            'lat': lat / 1e7,                 # Convert to degrees
            'height': height / 1e3,           # Convert to meters
            'hMSL': hMSL / 1e3,              # Convert to meters
            'hAcc': hAcc / 1e3,              # Accuracy in meters
            'vAcc': vAcc / 1e3,              # Accuracy in meters
            'gSpeed': gSpeed / 1e3,           # Speed in m/s
            'heading': heading / 1e5,         # Heading in degrees
            'pDOP': pDOP / 100.0,            # PDOP value
        }
    
    @staticmethod
    def format_result(result):
        """Pretty-print NAV-PVT result"""
        print(f"{'='*60}")
        print(f"NAV-PVT Status:")
        print(f"{'='*60}")
        print(f"Satellites in view:  {result['numSV']:3d}")  # ★ KEY
        print(f"Fix type:            {['No fix', 'Dead reckon', '2D', '3D', 'GNSS+DR', 'Time only'][result['gpsFix']]}")
        print(f"Timestamp:           {result['timestamp']}")
        print(f"Position:            {result['lat']:.7f}°N, {result['lon']:.7f}°E")
        print(f"Altitude:            {result['height']:.2f} m (MSL: {result['hMSL']:.2f} m)")
        print(f"Horizontal Accuracy: {result['hAcc']:.2f} m")
        print(f"Vertical Accuracy:   {result['vAcc']:.2f} m")
        print(f"Ground Speed:        {result['gSpeed']:.2f} m/s")
        print(f"Heading:             {result['heading']:.1f}°")
        print(f"PDOP:                {result['pDOP']:.2f}")
        print(f"{'='*60}")
        
        # Diagnostic flag
        if result['numSV'] == 0:
            print("⚠️  WARNING: No satellites in view. Possible causes:")
            print("   1. Antenna disconnected or damaged")
            print("   2. Indoor location (requires outdoor, clear sky)")
            print("   3. Cold start (wait 15-30 min for first fix)")
            print("   4. GPS constellation disabled in configuration")
        elif result['gpsFix'] == 0:
            print(f"⚠️  {result['numSV']} satellites visible but no fix yet (weak geometry)")
        else:
            print(f"✓ Fix acquired ({result['numSV']} satellites)")


# Example: Send query and parse response
def query_neo8m_position(serial_port):
    """
    Send NAV-PVT query to NEO-8M and parse response.
    serial_port: pyserial Serial object (open at 9600 baud or UBX protocol baud)
    """
    query = b'\xB5\x62\x01\x07\x00\x00\x08\x19'
    
    # Send query
    serial_port.write(query)
    
    # Read response (sync + class/id + length + payload + checksum)
    # Expect: B5 62 01 07 5C 00 [92 bytes] [2 bytes checksum] = 100 bytes total
    response = serial_port.read(100)
    
    if response[:2] != b'\xB5\x62':
        raise ValueError("Sync header mismatch")
    
    result = UBXParser.parse_nav_pvt(response[2:])
    UBXParser.format_result(result)
    return result
```

---

## Other Essential UBX Packets

### CFG-GNSS (0x06, 0x3E) — Enable/Disable Constellations

**Query:**
```
[B5 62] [06 3E] [00 00] [44 A2]
```

**Response:** 
```
[B5 62] [06 3E] [3C 00] [GNSS blocks × 8] [CK_A] [CK_B]
                 ↑
               length = 0x003C = 60 bytes
```

**GNSS Block Structure (8 bytes each):**
```
Offset  Bytes  Field               Description
──────  ─────  ─────               ──────────────────────
0       1      gnssId              0=GPS, 1=SBAS, 2=GLONASS, 3=QZSS, 5=Galileo, 6=BeiDou
1       1      resTrkCh            Reserved
2       1      maxTrkCh            Max tracking channels for this constellation
3       1      reserved            Reserved
4-7     4      flags               Bit 0: enabled (0x01), Bit 4: sigCfgMask (ignore)
```

**Example: NEO-8M default (GPS + GLONASS enabled)**
```python
# Query response parsing
gnss_blocks = [
    {'id': 0, 'name': 'GPS', 'enabled': True},
    {'id': 2, 'name': 'GLONASS', 'enabled': True},
    {'id': 3, 'name': 'QZSS', 'enabled': False},
    {'id': 5, 'name': 'Galileo', 'enabled': False},
    {'id': 6, 'name': 'BeiDou', 'enabled': False},
]
```

---

### CFG-SBAS (0x06, 0x16) — SBAS/DGPS Augmentation

**Enable SBAS for Europe (EGNOS):**
```
[B5 62] [06 16] [08 00] [01 03 00 00 00 00 00 00] [CK_A] [CK_B]
                         ↑  ↑
                      mode=1 (enabled)
                      usage=3 (correction + integrity)
```

**Payload Structure:**
```
Offset  Bytes  Name                Value
──────  ─────  ────                ──────
0       1      mode                0x00=disabled, 0x01=enabled
1       1      usage               0x01=integrity, 0x02=corrections, 0x03=both
2       1      maxSBAS             Max SBAS satellites (default 3)
3       1      scanMode            Scanning mode (0=auto)
4-7     4      scanmask1           PRN scanning mask (bits 0-31)
```

---

### MON-VER (0x0A, 0x04) — Firmware Version & Chip ID

**Query:**
```
[B5 62] [0A 04] [00 00] [0E 34]
```

**Response (typical):**
```
Software version: 8.20 (NEO-8M series)
Hardware ID:      00800000 (u-blox 8 core)
```

---

## UART Configuration (CFG-PRT)

**Query UART baud rate:**
```
[B5 62] [06 00] [01 00] [00] [07 31]  ← portID=0x00 (UART1)
```

**Response format:**
```
Offset  Bytes  Field                Example (9600 baud)
──────  ─────  ─────                ───────────────────
0       1      portID               0x00 (UART1)
1       1      reserved             0x00
2-3     2      txReady              0x0000
4-7     4      baudRate (LE)        0x002580 (9600 decimal)
8-9     2      inProtoMask          0x0001 (UBX), 0x0002 (NMEA), 0x0004 (RTCM)
10-11   2      outProtoMask         0x0002 (NMEA by default)
12-13   2      flags                0x0000
```

**Common baud rates:**
```
9600:     0x002580
19200:    0x004B00
38400:    0x009600
57600:    0x00E100
115200:   0x1C200
230400:   0x38400
```

---

## Troubleshooting Checklist: UBX Queries

| **Query** | **Command** | **Key Response** |
|-----------|-----------|--------|
| **Position/Fix** | `[B5 62 01 07 00 00 08 19]` | numSV (offset 24), gpsFix (offset 19) |
| **Constellations** | `[B5 62 06 3E 00 00 44 A2]` | flags byte (offset 4) for each GNSS block |
| **SBAS Status** | `[B5 62 06 16 00 00 22 DC]` | mode byte (offset 0) = 0x01 if enabled |
| **UART Config** | `[B5 62 06 00 01 00 00 07 31]` | baudRate field (bytes 4-7) |
| **Firmware** | `[B5 62 0A 04 00 00 0E 34]` | Software/Hardware version strings |

---

**Document Version:** 1.0  
**Reference:** u-blox M8 Receiver Description and Protocol Specification (UBX-13003221)  
**Note:** ⚠️ These fields apply to NEO-8M (M8 gen); older NEO-7 has different packet structures.
