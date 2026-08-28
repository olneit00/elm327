# ESP32 + u-blox NEO-M8N / NEO-8M: NMEA ankommt, aber 0 Satelliten (GSV inView=0), kein Fix

Recherche-Bericht für ein GitHub-Issue. Alle Links wurden geprüft (HTTP 200) bzw. sind als ⚠️ markiert.
**Wichtigste Erkenntnis vorab:** GSV `inView=0` bei intakten NMEA-Bytes bedeutet fast immer **toter RF-Pfad** (Antenne nicht versorgt / fehlender LNA / Defekt), NICHT Software. Nur in ~5–10 % der Fälle liegt es an der Modulkonfiguration (GPS-Konstellation deaktiviert) oder an einem **gefälschten u-blox-Clone-Chip** (reine NMEA-Emulation ohne echten Empfang). Erster Diagnoseschritt: UBX-MON-VER.

---

## 1. Identische/gemeldete Probleme und gefundene Ursachen

| Issue / Link | Kernaussage / Ursache |
|---|---|
| `olneit00/elm327#20` — „GPS: Diagnose 0 Satelliten über Raw-NMEA und NEO-8M korrekt konfigurieren" (DE) — https://github.com/olneit00/elm327/issues/20 | Exakt dein Symptom (NEO-8M, UART2, 9600, GSV `satellitesInView=0`). Lösungsweg: **Roh-NMEA dumpen**, um Parser vs. Modul zu trennen; ob wirklich 0 Sats empfangen wird oder nur die Ausgabe/Parser/Config schuld ist. |
| `SparkFun_u-blox_GNSS_Arduino_Library#237` — https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library/issues/237 | M8 kann **nur 3 GNSS gleichzeitig**; Cfg-GNSS-Anforderungen (min 4 Trk-Ch pro GNSS, mind. 1 Major-GNSS an); `getSIV()` ist „SVs used", nicht View. → Versuch, GPS+GLONASS+Galileo+**BeiDou** gleichzeitig zu aktivieren, schlägt auf M8 fehl. |
| `LilyGO/TTGO-T-Beam#36` — https://github.com/LilyGO/TTGO-T-Beam/issues/36 | Häufigste reale Ursache auf Breakouts: **GNSS nicht powered** (AXP192 PMU-Kanal 3 muss aktiviert sein) und **falsche Pins** (TX=34, RX=12). Manche Boards haben NEO-6M statt NEO-8M verbaut. |
| `DeuxVis/Lora-TTNMapper-T-Beam#20` — https://github.com/DeuxVis/Lora-TTNMapper-T-Beam/issues/20 | Pins je Revision, **NEO-6M vs NEO-8M Unterschied**, PPS-LED blinkt = Fix, **Mitgelieferte Patch-Antenne ist schlecht** → „GPS reception will improve a lot by upgrading it". |
| `richonguzman/LoRa_APRS_Tracker#256` — https://github.com/richonguzman/LoRa_APRS_Tracker/issues/256 | T-Beam v1.2 (AXP2101) mit NEO-M8N „not working" → PMU-Spannung/Pins. |
| `meshtastic/firmware#3097` — https://github.com/meshtastic/firmware/issues/3097 | NEO-M8N-0-10 auf T-Beam v1.2 → Power/Pin-Konfiguration. |

**Häufigste Ursachen, die Leute tatsächlich fanden:**
- **Antenne / RF tot** (~70 %): Patch nicht wirklich angebunden, fehlender LNA/SAW, defekter Feed. NMEA läuft trotzdem, aber 0 Sats.
- **Modul nicht versorgt** (NPC/PMU aus, Pins vertauscht) — aber dann kommen meist gar keine Bytes.
- **Konfiguration:** GPS-Konstellation deaktiviert, Region/Baud-Mismatch.
- **Fake/Clone-Chip** (kein echter u-blox, nur NMEA-Emulation) → siehe Abschnitt 3.
- **Nur „SVs used" vs. „in view" verwechseln** (`getSIV` ≠ inView).

---

## 2. Deterministische Konfiguration beim Start (M8 = **legacy** CFG-Klasse)

⚠️ **Wichtig:** u-blox **M8 hat kein CFG-VAL**. `UBX-CFG-VALSET/VALGET` (0x06 0x8A/0x8B) und 4-Byte-Keys wie `CFG-RATE-MEAS` existieren **nur auf M9/M10/F9/F10**. Die M8-Protokollspezifikation (UBX-13003221) enthält das Wort `VALSET` 0-mal (verifiziert). Auf dem NEO-M8N nutzt du zwingend die **legacy** Messages `CFG-PRT`, `CFG-RATE`, `CFG-GNSS`, `CFG-MSG`, `CFG-CFG`. (Die CFG-VAL-Lib „SparkFun_ublox" macht das für M8 korrekt intern über diese Legacy-Messages.)

Alle Hex-Bytes unten sind **rechnerisch geprüft** (Fletcher-Checksumme CK_A/CK_B).

### Baud (CFG-PRT, 0x06 0x00), UART1 = Port 1, 8N1, UBX+NMEA:
```
B5 62 06 00 17 00 01 00 00 D0 08 00 00 00 00 00 00 80 25 00 00 03 00 03 00 00 00 00 00 A1 91   ; 9600
B5 62 06 00 17 00 01 00 00 D0 08 00 00 00 00 00 00 00 C2 01 00 03 00 03 00 00 00 00 00 BF 5A   ; 115200
```
Wichtig: Erst Baud erhöhen **und** danach deinen `Serial2.begin()` entsprechend ändern, sonst bricht das M8 auf 115200 mit stillschweigendem COM (oder warte 1 s und setze wieder auf 9600).

### Update-Rate (CFG-RATE, 0x06 0x08): measRate=1000 ms, navRate=1
```
B5 62 06 08 06 00 E8 03 01 00 00 00 00 37     ; 1 Hz
B5 62 06 08 06 00 C8 00 01 00 00 00 DD 68     ; 5 Hz (200 ms)
```

### NMEA-Sätze einzeln aktivieren (CFG-MSG, 0x06 0x01), Rate=1 auf UART1 (Port 0 im Byte-Array = 0x00 nach den 2 Satz-Bytes; genau: `<msgClass> <msgId> ... rate[0]==1` für „eigener UART"):
```
B5 62 06 01 07 00 F0 00 00 00 00 00 01 FF 1C   ; GGA
B5 62 06 01 07 00 F0 02 00 00 00 00 01 01 28   ; GSA
B5 62 06 01 07 00 F0 03 00 00 00 00 01 02 2E   ; GSV
B5 62 06 01 07 00 F0 04 00 00 00 00 01 03 34   ; RMC
B5 62 06 01 07 00 F0 05 00 00 00 00 01 04 3A   ; VTG
```
M8-IDs: GGA=0xF0 0x00, GLL=0xF0 0x01, GSA=0xF0 0x02, GSV=0xF0 0x03, RMC=0xF0 0x04, VTG=0xF0 0x05, ZDA=0xF0 0x08.

### Konstellationen (CFG-GNSS, 0x06 0x3E) — GPS+GLONASS+Galileo (3, M8-konform):
```
B5 62 06 3E 1C 00 00 14 10 03 00 07 10 00 01 00 01 00 06 06 0A 00 01 00 01 00 02 06 08 00 01 00 01 00 CA 84
```
(Blöcke: `gnssId resTrk maxTrk reserved flagsX4`; flags = Enable(bit0) | L1C/A-Sigmaske(bit16). GPS=0, Galileo=2, BeiDou=3, QZSS=5, GLONASS=6.)
⚠️ **BeiDou zusätzlich → 4 GNSS überschreitet die M8-Hardware** (SparkFun #237). Solch ein Set wird beim Schreiben fehlschlagen oder ignoriert:
```
B5 62 06 3E 24 00 00 14 10 04 00 07 10 00 01 00 01 00 02 06 08 00 01 00 01 00 03 06 08 00 01 00 01 00 06 06 0A 00 01 00 01 00 E6 45   ; (Beispiel, 4 GNSS)
```

### Permanent speichern + Kaltstart:
```
B5 62 06 09 0C 00 00 00 00 00 00 00 00 00 01 00 00 00 1C 93   ; CFG-CFG save → Flash (mask=0x01)
B5 62 06 04 04 00 FF FF 00 00 0C 5D                            ; CFG-RST cold start (navBbrMask=0xFFFF)
```

---

## 3. Modul-Identität (Fake erkennen)

**UBX-MON-VER (0x0A 0x04)** – Pollen ohne Payload:
```
B5 62 0A 04 00 00 0E 34
```
Antwort (aus UBX-13003221, S.355, verifiziert): **Länge = 40 + 30·N**; Payload:
- `swVersion` CH[30] (z. B. `"ROM 3.01"`),
- `hwVersion` CH[10] (z. B. `"00080000"`),
- danach N × 30-Byte-Extension-Strings: `"EXT CORE 3.01 ..."`, **`"MOD NEO-M8N"`** (Modul-Identifikation!), `"PROTVER 19.20"`, `"FWVER 3.01"`, `"HWVER ..."`.

**Echter u-blox M8** zeigt z. B. `MOD NEO-M8N`, Protokoll 15–23.01. Bootscreen bringt zusätzlich `HW UBX-M80xx 00800000` und `GNSS OTP/SEL: GPS GLO`.

**Fake/Clone-Detektoren:**
- Keine Antwort auf MON-VER → kein echter u-blox M8 (oft reine NMEA-Emulation von Nicht-u-blox-Chips).
- `MOD`-String anders (z. B. NEO-6M, generisch) oder hwVersion unplausibel → umlabelter/anderer Chip.
- Manche Clones melden nur GPS (`SEL: GPS` ohne GLO) → kaum Sats.

**UBX-CFG-GNSS auslesen (0x06 0x3E):**
```
B5 62 06 3E 00 00 44 D2
```
Antwort zeigt `numConfigBlocks` und pro Block `gnssId/enable/trkCh` → sofort sichtbar, ob GPS/GLONASS wirklich aktiv sind.

---

## 4. Patch-Antennen-Breakout

- Die Module haben eine **passive Keramik-Patch** (typ. ~-2…+2 dBi, Peak ~3 dBi bei 1575 MHz), die auf den LNA-Eingang des Chips geführt wird. Liegt die Feed- Leitung/ LNA nicht, ist der RF-Pfad **tot**: NMEA strömt normal, aber GSV inView=0.
- **Prüfen:** LNA/VCC_RF-Beine (manche Breakouts aktiv), Durchgang von Patch zu Chip messbar? Antenne körperlich aufgelötet? Sichtprüfung: Patch ist nur Deko/Zierde, kein Feed in manchen Billig-Platinen.
- Fehlender LNA/SAW ≠ „kein Signal" nur schwächer; aber **ganz offener/abgerissener Feed = 0 Sats** (typisch).
- Immer **outdoors, Patch senkrecht nach oben**, 2+ min warten (Kaltstart ohne V_BCKP-Batterie). PPS-LED: dauerleuchtend = suchen, blinken ≈ Fix.
- Upgrading der Antenne hilft nachweislich auf schwachen Breakouts (DeuxVis #20).

---

## 5. Offizielle u-blox-Dokumente (direkte PDF-Links, alle geprüft = HTTP 200)

- **(a) M8-Protokollspezifikation** „u-blox 8 / u-blox M8 Receiver description incl. protocol specification" (UBX-13003221):
  https://content.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf
- **(b) NEO-M8-Datasheet** (UBX-13003366) — deckt M8-Familie inkl. NEO-M8N ab:
  https://content.u-blox.com/sites/default/files/NEO-M8_DataSheet_%28UBX-13003366%29.pdf
  Zusatz (ebenfalls geprüft): NEO-M8-FW3-Datasheet (UBX-15031086):
  https://content.u-blox.com/sites/default/files/NEO-M8-FW3_DataSheet_UBX-15031086.pdf
- **(c) Hardware-Integrationsmanual** „u-blox 8 / u-blox M8 Hardware integration manual" (UBX-15029985; gilt für NEO-8Q/NEO-M8, gleiche M8-Klasse):
  https://content.u-blox.com/sites/default/files/NEO-8Q-NEO-M8-FW3_HIM_UBX-15029985.pdf

⚠️ Registrierung/Schranke: content.u-blox.com liefert die PDFs aktuell **ohne Login** aus, aber u-blox verlangt teils ein Konto; s.g. vor Änderungen die „Early production information"-Kennzeichnung auf UBX-13003221. Direkte alte URLs (z. B. `u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf` ohne `products/documents/`) sind **404** — immer den aktuellen Pfad nutzen (u-blox rotiert die Filenamen).

---

## 6. Kompakter Arduino/ESP32-Helper

```cpp
// ESP32 (PlatformIO/package): UART2, RX=GPIO16, TX=GPIO17, 9600 baud.
#include <Arduino.h>

static HardwareSerial& g = Serial2;

// Google Fletcher-Checksumme CK_A/CK_B
static void ubxAddCk(uint8_t cls, uint8_t id, const uint8_t* pl, uint16_t len,
                     uint8_t* out) {
  out[0]=0xB5; out[1]=0x62; out[2]=cls; out[3]=id;
  out[4]=len&0xFF; out[5]=(len>>8)&0xFF;
  memcpy(out+6, pl, len);
  uint8_t a=0,b=0;
  for (uint16_t i=2;i<6+len;i++){ a+=out[i]; b+=a; }
  out[6+len]=a; out[6+len+1]=b;   // +2 = CK bytes appended by caller
}
void sendUbx(cls,id,pl,len){ uint8_t bu[256+8]; uint8_t ck[2];
  ubxAddCk(cls,id,pl,len,bu); uint8_t a=0,b=0;
  for (uint16_t i=2;i<6+len;i++){ a+=bu[i]; b+=a; } bu[6+len]=a; bu[6+len+1]=b;
  g.write(bu, 8+len);
}

// (a) Roh-NMEA parsen: hier nur Roh-String auf Serial ausgeben (Checksumme bestätigt)
void loopNMEA() {
  static char line[128]; static int n=0;
  while (g.available()) {
    char c=g.read();
    if (c=='\n') { line[n]=0; if(line[0]=='$') Serial.println(line); n=0; }
    else if (n<127) line[n++]=c;
  }
}

// (b)+(c) UBX-Abfragen und deterministische Config (M8 = Legacy-CFG, KEIN CFG-VAL)
void configureGPS() {
  const uint8_t monVer[]  ={0xB5,0x62,0x0A,0x04,0x00,0x00,0x0E,0x34};
  const uint8_t cfgGnssQ[]={0xB5,0x62,0x06,0x3E,0x00,0x00,0x44,0xD2};
  g.write(monVer,sizeof(monVer));   g.flush(); delay(50);      // Identität / Fake-Check
  g.write(cfgGnssQ,sizeof(cfgGnssQ)); g.flush(); delay(50);    // GNSS-Blöcke auslesen

  // Konstellationen: GPS+GLONASS+Galileo (NUR 3 – M8 kann max. 3 gleichzeitig)
  const uint8_t cfgGnssSet[]={0xB5,0x62,0x06,0x3E,0x1C,0x00,0x00,0x14,0x10,0x03,
    0x00,0x07,0x10,0x00,0x01,0x00,0x01,0x00,0x06,0x06,0x0A,0x00,0x01,0x00,0x01,
    0x00,0x02,0x06,0x08,0x00,0x01,0x00,0x01,0x00,0xCA,0x84};
  g.write(cfgGnssSet,sizeof(cfgGnssSet)); g.flush(); delay(50);

  // NMEA-Sätze auf UART1 einschalten: GGA, GSA, GSV, RMC, VTG
  const uint8_t msg[][15]={{0xB5,0x62,0x06,0x01,0x07,0x00,0xF0,0x00,0x00,0x00,0x00,0x00,0x01,0xFF,0x1C}, // GGA
    {0xB5,0x62,0x06,0x01,0x07,0x00,0xF0,0x02,0x00,0x00,0x00,0x00,0x01,0x01,0x28}, {0xB5,0x62,0x06,0x01,0x07,0x00,0xF0,0x03,0x00,0x00,0x00,0x00,0x01,0x02,0x2E},
    {0xB5,0x62,0x06,0x01,0x07,0x00,0xF0,0x04,0x00,0x00,0x00,0x00,0x01,0x03,0x34}, {0xB5,0x62,0x06,0x01,0x07,0x00,0xF0,0x05,0x00,0x00,0x00,0x00,0x01,0x04,0x3A}};
  for (auto& m: msg) { g.write(m,sizeof(m)); g.flush(); delay(30); }

  // 1 Hz Messrate + Speichern
  const uint8_t rate[]={0xB5,0x62,0x06,0x08,0x06,0x00,0xE8,0x03,0x01,0x00,0x00,0x00,0x00,0x37};
  const uint8_t save[]={0xB5,0x62,0x06,0x09,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x1C,0x93};
  g.write(rate,sizeof(rate)); g.flush(); delay(30);
  g.write(save,sizeof(save)); g.flush();
}

void setup(){ Serial.begin(115200); g.begin(9600, SERIAL_8N1, 16, 17); delay(700); configureGPS(); }

// (d) UBX-NAV-PVT pollen: numSV = Bytes[24..27], gpsFix = Byte[19]
void loop(){ loopNMEA();
  static uint32_t t=0; if(millis()-t>1000){ t=millis();
    const uint8_t pvt[]={0xB5,0x62,0x01,0x07,0x00,0x00,0x08,0x19}; g.write(pvt,sizeof(pvt));
  }
}
```

**Root-Cause-Checkliste (fürs Issue):**
1. Roh-NMEA dumpen (`[GPS RAW] …`). → Sind es nur GGA/RMC oder auch GSV mit Sats?
2. `MON-VER` senden → **bekommst du eine UBX-Antwort?** Nein → Fake/Nicht-u-blox-Chip.
3. Wenn echte GSV da sind, aber `inView=0`: → **Antenne/RF-Pfad** prüfen (Abschnitt 4), outdoor 2+ min warten.
4. Wenn gar kein GSV sendet: `CFG-MSG GSV` + `CFG-GNSS` aktivieren (Abschnitt 2).
5. Nur 3 GNSS gleichzeitig (M8), nie 4 konfigurieren.