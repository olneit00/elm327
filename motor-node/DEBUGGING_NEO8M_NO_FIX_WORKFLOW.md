# NEO-8M GPS „Kein Fix" — Debugging-Workflow

## Schnelle Diagnose (vor Forschung abwarten)

### Schritt 1: PPS-LED überprüfen
- **Breakout-LED (PPS-Pin):**
  - Dauerlicht = kein Fix (Modul sucht noch)
  - Blinkt (900 ms an / 100 ms aus) = **Fix vorhanden**
  - Dunkel = Modul startet nicht oder Stromversorgung-Problem

### Schritt 2: Raw-Bytes auf Serial Monitor
Laden Sie `DIAGNOSTIC_GPS_NO_FIX.cpp` (sketches folder) als **alternative `main()`**-Funktion hoch:

```bash
# Aktuell: motor-node/src/main.cpp lädt normalerweise
# Zum Debuggen: main.cpp temporär auskommentieren, DIAGNOSTIC_GPS_NO_FIX.cpp aktivieren

# Serial Monitor (115200 baud):
[DIAG] RX 86 bytes @ 5000 ms: 24 47 50 47 47 41 2c ...
[GPS] inView=11 qual=1 mode=3 sats=8 hdop=0.90 lat=48.117300 lon=11.516667
[UART] baud=9600, config=8N1, available=12
```

**Interpretation:**
- **RX=0 bytes** → UART-Wiring falsch, Baud-Rate stimmt nicht, oder Modul nicht an Strom
- **RX=bytes, aber inView=0** → Modul läuft, aber kein Fix (Antenne, Region, SBAS deaktiviert)
- **RX=bytes, snapshots nie updated** → Parser-Mutex-Deadlock oder Snapshot-Bug

### Schritt 3: Antenne + Outdoor-Check
- Antenne **nach oben zeigen** (nicht horizontal)
- **Klarer Himmel nötig** (min. 30° Elevation)
- Indoor / Fenster oft zu wenig Signal

### Schritt 4: Warten auf Forschungs-Ergebnis
Recherche wird konkrete Maßnahmen liefern:
- **UBX-Befehle** zum Aktivieren von Constellation/SBAS/Region
- **Firmware-Update** falls notwendig (AliExpress-Clone Bugs)
- **Hardware-Workaround** (z.B. Kondensator auf V_BCKP)

---

## Nach Forschungs-Ergebnis

Issue wird automatisch erstellt mit:
1. **Findings** (was das Problem ist)
2. **UBX-Config-Code** (vorgefertigt in `UbxHelper.h`)
3. **Hardware-Mods** (falls nötig)
4. **Schrittemit Bildern / CLI-Output**

### Dann: Issue durcharbeiten
```bash
# Issue #XX wird angelegt, z.B.
# „NEO-8M GPS: Aktivieren von Region/SBAS für Europa"
# oder
# „NEO-8M: Firmware-Update für AliExpress-Clone"

# Schritte der Issue:
1. UBX-Befehle via UbxHelper::configSBAS() senden
2. Neustart + 60 s warten (TTFF - Time-To-First-Fix)
3. Diagnostics erneut laufen lassen
4. Feedback in Issue posten
```

---

## Hilfreiche Dateien

- `DIAGNOSTIC_GPS_NO_FIX.cpp` — Sketc zum Auslesen der raw UART2-Bytes
- `UbxHelper.h` — UBX-Befehle (wird mit Forschungs-Ergebnissen gefüllt)
- `TROUBLESHOOTING_GPS_NO_FIX.md` — Kurze Sofort-Checks
- Forschungs-Issue (wird angelegt) — Konkrete Schritte + UBX-Payload