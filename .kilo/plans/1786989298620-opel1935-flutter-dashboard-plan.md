# Opel 1935 Digital-Zusatzinstrument – Flutter-App für Fire HD 10

## 0. Scope-Hinweis (wichtig)

Dieses Dokument ist ein **Implementierungsplan**, kein fertiger Quellcode. Er wurde im Plan-Modus erstellt (keine Datei-Edits erlaubt). Er enthält alle Fakten, Formeln, Algorithmen, Test-Vektoren und Architekturentscheidungen so präzise, dass eine Implementierungs-Session (Build/Code-Modus) daraus **ohne weitere Design-Entscheidungen** produktionsreifen, platzhalterfreien Dart-/Flutter-Code erzeugen kann. Nach Freigabe dieses Plans: In einen Implementierungs-Agenten wechseln und Punkt für Punkt abarbeiten.

Vom User bestätigte Leitentscheidungen (siehe Rückfragen dieser Session):
- Visuelles Design: allgemeines Vorkriegs-Wissen nutzen (keine Referenzbilder vorhanden) – Werte/Skalen sind daher **dokumentierte Annahmen**, keine exakten Nachbildungen.
- Transport: **TCP + BLE** von Anfang an (Transport-Abstraktion).
- Statische `VehicleState`-Werte: App wird voll funktionsfähig für Live-Daten gebaut; aktuelle Firmware-Limitierung wird nur dokumentiert, nicht behoben (Firmware-Änderung ist **out of scope**).
- Neues Flutter-Projekt `opel1935_dashboard` (statt Umbau von `obd_companion`).
- "Punkt 28"-Kontrollfragen aus dem Original-Prompt existieren nicht – werden ignoriert; Abschlussübersicht liefert eine normale Ergebnis-Checkliste.

---

## 1. Firmware-Protokoll-Referenz (Single Source of Truth)

Quelle: `src/Elm327Server.cpp`, `include/Elm327Server.h`, `include/VehicleState.h`, `src/VehicleState.cpp`, `src/main.cpp` (vollständig gelesen, Zeilenangaben beziehen sich auf den aktuellen Stand dieser Dateien).

### 1.1 Transport-Fakten

| Transport | Details |
|---|---|
| TCP | `WiFiServer` auf Port **35000** (`kTcpPort`, `main.cpp:7`). ESP32 ist **Access Point** (`WIFI_AP`), SSID **`Opel1935`**, **kein Passwort** (offenes Netz). Gateway-IP des AP (Standard-Espressif-Default) ist **`192.168.4.1`**. Genau **1 Client** gleichzeitig, weitere Verbindungsversuche werden sofort getrennt. Kein Server-seitiger Idle-Timeout. `setNoDelay(true)` gesetzt. |
| BLE | NimBLE, Device-Name **`ELM327 Opel1935`**, Service-UUID **`FFE0`**, Characteristic-UUID **`FFE1`** (READ/NOTIFY/WRITE/WRITE_NR – "HM-10"-kompatibles Muster). Ausgehende Antworten werden in **20-Byte-Chunks** (`kBleChunkSize`) mit **15 ms Pause** zwischen Chunks gesendet. Auch hier nur 1 aktive Verbindung sinnvoll unterstützt. |
| Beide | Werden im ESP32-`loop()` gleichzeitig bedient (`update()` alle ~2 ms). Es gibt **kein WebSocket** – falls das in früherer Session-Kommunikation anders erwähnt wurde, ist das falsch. |

### 1.2 AT-Initialisierungssequenz (für `Elm327Client.initializeAdapter()`)

Exakt auf die Parser-Fähigkeiten der Firmware abgestimmt. Reihenfolge verbindlich einhalten, jeden Schritt mit Timeout (empfohlen 2 s) bestätigen lassen, bevor der nächste gesendet wird:

| # | Befehl | Erwartete Antwort | Zweck |
|---|---|---|---|
| 1 | `ATZ` | Enthält `"ELM327"` (Banner `"ELM327 v1.4b"`) | Reset, `resetAdapterState()` |
| 2 | `ATE0` | `"OK"` | Echo aus (vereinfacht Framing) |
| 3 | `ATL0` | `"OK"` | **Nur `\r`** als Terminator (statt `\r\n`) – reduziert Frame-Rauschen |
| 4 | `ATS1` | `"OK"` | Leerzeichen zwischen Bytes **beibehalten** (Firmware-Default) |
| 5 | `ATH0` | `"OK"` | Header aus (Firmware-Default) – da nur 1 virtuelle ECU (`0x7E8`) existiert, wird kein Header/DLC-Präfix benötigt |
| 6 | `ATSP6` | `"OK"` | Protokoll **fest auf 6** (ISO 15765-4 CAN 11-bit/500 kbit/s) setzen – verhindert den `"SEARCHING..."`-Präfix und aggressives Protokoll-Scannen |

**Bewusste Abweichung von der Beispielsequenz aus der Aufgabenstellung:** Dort wurde `ATS0` als Beispiel genannt. Es wird stattdessen **`ATS1`** verwendet (Leerzeichen an), weil das robuste Tokenisieren der Hex-Bytes im Parser (Split by Whitespace) ohne Leerzeichen eine feste Byte-Breite pro PID voraussetzen würde – unnötige Fehlerquelle. `ATS1` ist zudem der Firmware-Default, verhält sich also identisch zu "nichts senden", macht die Absicht aber explizit.

### 1.3 Request-/Response-Framing

- Anfrage: `<PID-String>\r` (z. B. `"010C\r"`). Firmware normalisiert eingehende Befehle selbst (Leerzeichen/Tabs/CR/LF entfernt, Großschreibung, `;` am Ende entfernt) – die App muss also **nicht** übermäßig defensiv senden, sollte aber trotzdem stets mit `\r` terminieren.
- Antwort: `[Response-Text]` + Terminator (`\r`, da `ATL0`) + **`>`** (Prompt-Zeichen = Frame-Ende-Marker). Bei `ATE0` **kein** Echo des gesendeten Kommandos voran. Der Parser muss trotzdem defensiv gegen ein eventuelles Echo sein (siehe 1.6).
- Bei mehreren Datenzeilen (Multi-Frame, siehe 1.5) sind die Zeilen durch den Terminator (`\r`) getrennt, der Prompt `>` kommt erst nach der letzten Zeile.
- Fehlerfälle: unbekannter/fehlerhafter AT-Befehl → `"?"`. Bekannter Modus (01/09), aber nicht unterstützte PID → `"NO DATA"`. RX-Buffer-Overflow (TCP >128 Zeichen) → sofort `"?"`, Buffer geleert.

### 1.4 Unterstützte PIDs (vollständige, verbindliche Liste – keine weiteren erfinden)

**Mode 01:**

| PID (hex) | Name | Bytes (nach `41 PID`) | Dekodier-Formel (Ist-Wert aus Rohbytes) | Quelle in Firmware | Polling-Priorität |
|---|---|---|---|---|---|
| `00` | PIDs supported [01-20] | 4 | Bitmaske, siehe 1.7 | dynamisch berechnet | – (nur Debug/Diagnose, kein Gauge) |
| `01` | Monitor status | 4 | immer `00 00 00 00` (Stub) | Konstante | – (kein Gauge, evtl. Debug-Anzeige) |
| `04` | Motorlast | 1 | immer `00` (Stub) | Konstante | – (kein Gauge) |
| `05` | Kühlmitteltemperatur | 1 | `coolantC = raw - 40` | `VehicleState.coolantTemperature` | **Tier 3 (0,5–2 Hz)** |
| `0C` | Drehzahl | 2 | `rpm = ((byte0<<8)\|byte1) / 4` | `VehicleState.rpm` | **Tier 1 (5–10 Hz)** |
| `0D` | Geschwindigkeit | 1 | `speedKmh = raw` | `VehicleState.speedKmh` | **Tier 1 (5–10 Hz)** |
| `0E` | Zündvoreilung | 1 | `ignitionAdvanceDeg = raw/2 - 64` | `VehicleState.ignitionAdvanceDeg` | **Tier 2 (2–5 Hz)** |
| `11` | Drosselklappenposition | 1 | immer `00` (Stub) | Konstante | – (kein Gauge) |
| `20` | PIDs supported [21-40] | 4 | Bitmaske, siehe 1.7 | dynamisch berechnet | – |
| `2F` | Tankfüllstand | 1 | `fuelPercent = raw * 100 / 255` | `VehicleState.fuelPercent` | **Tier 3 (0,5–2 Hz)** |
| `40` | PIDs supported [41-60] | 4 | immer `00 00 00 00` | dynamisch berechnet | – |

**Mode 09:**

| PID (hex) | Name | Daten | Quelle |
|---|---|---|---|
| `00` | Supported PIDs | Bitmaske über `{02, 04}` | dynamisch |
| `02` | Fahrgestellnummer (VIN-Feld) | ASCII `"10-58090"` (Record-Index-Byte `0x01` voran) | `kChassisNumber` |
| `04` | Kalibrierungs-ID | ASCII `"Opel 1,2 ltr."` (Record-Index-Byte `0x01` voran) | `kVehicleDesignation` |

**Nur PIDs `05, 0C, 0D, 0E, 2F` bekommen ein Rundinstrument.** Die Stub-PIDs (`01, 04, 11`) werden vom Decoder unterstützt/getestet, aber nicht als Gauge angezeigt (liefern firmware-seitig ohnehin immer `0`). Mode-09-Daten (`02`, `04`) werden **einmalig** nach Verbindungsaufbau abgefragt und in einer "Fahrzeug-Info"-Anzeige (Settings/Debug-Screen) angezeigt, nicht zyklisch gepollt.

### 1.5 ISO-TP-Framing (kritisch für Mode-09-Text-Antworten)

Nur Antworten **> 7 Bytes** werden fragmentiert (alle Mode-01-Antworten sind ≤ 6 Bytes → immer Single Frame ohne PCI-Nibble). Beide Mode-09-Textantworten (VIN, Calibration-ID) sind länger und **werden** fragmentiert:

- **Single Frame** (`len ≤ 7`): Response-Bytes werden 1:1 ausgegeben, **kein** PCI-Präfix-Byte (Firmware-Eigenheit, bewusst vom Standard-ELM327-Verhalten übernommen).
- **First Frame** (`len > 7`): Byte0 = `0x10 | (len>>8 & 0x0F)`, Byte1 = `len & 0xFF`, Byte2–7 = erste 6 Payload-Bytes. Immer 8 Bytes lang.
- **Consecutive Frame**: Byte0 = `0x20 | sequenceNumber` (beginnend bei 1, wraps bei 16), Byte1–7 = nächste bis zu 7 Payload-Bytes, **rest-auffüllend mit `0x00`** falls letzter Chunk < 7 Bytes. Immer 8 Bytes lang (auch wenn nicht alle sinnvoll sind – die App muss anhand der `len` aus dem First Frame wissen, wo der echte Payload endet, und darf die Zero-Padding-Bytes am Ende NICHT als Nutzdaten übernehmen).
- Zeilen werden mit dem aktuellen Terminator (`\r` bei `ATL0`) verbunden.

**Exakte, code-verifizierte Test-Vektoren** (aus `buildIsoTpFrames`/`formatIsoTpResponse`/`formatObdResponse` nachgerechnet, mit `ATH0`+`ATS1`+`ATL0`):

**VIN-Abfrage (`0902` → Payload `49 02 01 31 30 2D 35 38 30 39 30`, 11 Bytes):**
```
Rohantwort (vor dem finalen Terminator+Prompt):
"10 0B 49 02 01 31 30 2D\r21 35 38 30 39 30 00 00"
Vollständiger TCP/BLE-Payload inkl. Abschluss:
"10 0B 49 02 01 31 30 2D\r21 35 38 30 39 30 00 00\r>"
```
Erwartetes Decoder-Ergebnis nach Reassemblierung (Payload auf `len=11` Bytes getrimmt): `49 02 01 31 30 2D 35 38 30 39 30` → Mode=0x49, PID=0x02, Record-Index=0x01, Text=`"10-58090"`.

**Calibration-ID-Abfrage (`0904` → Payload `49 04 01 4F 70 65 6C 20 31 2C 32 20 6C 74 72 2E`, 16 Bytes):**
```
"10 10 49 04 01 4F 70 65\r21 6C 20 31 2C 32 20 6C\r22 74 72 2E 00 00 00 00\r>"
```
Erwartetes Decoder-Ergebnis: Text=`"Opel 1,2 ltr."`.

### 1.6 Mode-01 Single-Frame Test-Vektoren (Firmware-Default-Werte, für Decoder-Tests)

| Anfrage | Rohantwort | Dekodiert |
|---|---|---|
| `010C` | `"41 0C 0B B8\r>"` | RPM = (0x0BB8=3000)/4 = **750** |
| `010D` | `"41 0D 2A\r>"` | Speed = 0x2A=42 = **42 km/h** |
| `0105` | `"41 05 7A\r>"` | Coolant = 0x7A=122 - 40 = **82 °C** |
| `010E` | `"41 0E 94\r>"` | Ignition = 0x94=148 / 2 - 64 = **10°** |
| `012F` | `"41 2F 4D\r>"` | Fuel = 0x4D=77 * 100/255 ≈ **30,2 %** (Toleranz ±0,5 wegen Rundung in Firmware) |
| `0100` | `"41 00 98 1C 80 01\r>"` | Supported-PID-Bitmaske (nur für Debug-Test, kein Gauge) |
| `010F` (nicht unterstützt) | `"NO DATA\r>"` | → `ParseResult.noData` |
| unbekannter AT-Befehl | `"?\r>"` | → `ParseResult.error` |

### 1.7 Bekannte Firmware-Limitierung (dokumentiert, nicht Teil dieses Auftrags)

`VehicleState` (`rpm=750`, `coolantTemperature=82.0`, `speedKmh=42.0`, `fuelPercent=30.0`, `ignitionAdvanceDeg=10.0`) wird **im gesamten Firmware-Lifecycle nie verändert** (kein Timer/Sensor/RNG in `main.cpp`/`Elm327Server.cpp`). Die App wird trotzdem so gebaut, als kämen Live-Werte (Polling, Animation, Reconnect) – funktional korrekt, zeigt aber aktuell dauerhaft dieselben Werte an. Dies wird im Debug-Screen und in der Abschlussübersicht explizit vermerkt.

---

## 2. Design-Konzept (Annahmen, da keine Referenzbilder vorliegen)

Da keine Bilder angehängt wurden, basiert das Design auf allgemein bekannten Merkmalen von Vorkriegs-Bordinstrumenten (VDO/Bosch-Rundinstrumente der 1930er) und ist **explizit als Annahme markiert** – über benannte Konstanten in `theme/opel1935_theme.dart` leicht später anpassbar/austauschbar.

| Merkmal | Festlegung |
|---|---|
| Zifferblatt | Elfenbein/Creme (`#F0E6D2`), leichte radiale Vignette zum Rand |
| Ziffern/Beschriftung | Schwarz (`#1A1A1A`), Schriftart `Oswald` (Google Fonts, condensed, gut lesbar bei kleiner Größe, häufig in Retro-Gauge-Nachbauten verwendet) |
| Branding-Schrift ("OPEL", Markenname) | `Cinzel` (Google Fonts, elegante Versalien) – als Platzhalter für eine später lizenzierbare, echte Art-Déco-Schrift |
| Zeiger | Dünn, rot (`#B5231A`), spitz zulaufend, kleine runde Nabe |
| Lünette | Chrom-Optik via `RadialGradient`/`SweepGradient` (hell `#E8E8E8` → dunkel `#4A4A4A`), dünner Glanzlicht-Bogen oben links (simulierte Glasreflexion, halbtransparent weiß) |
| Armaturenbrett-Hintergrund | Sehr dunkles Braun/Schwarz (`#0D0D0D`–`#1C1712`), matte Textur (einfacher `RadialGradient`, kein Bildasset nötig) |
| Farbzonen | Coolant: <60 °C blau ("K"), 60–100 °C grün, >100 °C rot ("H"); RPM: Redline-Zone 3500–5000 U/min rot markiert |

**Skalen (Annahmen, dokumentiert, über Konstanten austauschbar):**

| Gauge | Skala | Teilstriche |
|---|---|---|
| Drehzahl | 0–5000 U/min | alle 500 |
| Geschwindigkeit | 0–120 km/h | alle 20 |
| Kühlmitteltemperatur | 40–120 °C | alle 20, + Farbzonen |
| Tankfüllstand | 0–100 % | "L" (leer) / "V" (voll) statt E/F (deutsche Beschriftung, historisch plausibler) |
| Zündvoreilung | −10° bis +40° BTDC | alle 10° (Hinweis: 1935 hatte kein Fahrzeug ein Zündvoreilungs-Instrument im Cockpit – dies ist bewusst ein modernes Diagnose-Zusatzinstrument im historischen Gehäusedesign, im Debug/About-Screen als solches vermerkt) |

---

## 3. Projekt-Setup

- **Neues Flutter-Projekt**: `opel1935_dashboard`, angelegt unter `C:\Users\olive\Documents\Flutter\opel1935_dashboard` (Sibling-Ordner zu `obd_companion`, außerhalb des PlatformIO-Workspaces – etablierte Konvention aus dieser Session).
- **Bereits eingerichtete Umgebung wiederverwenden** (aus dieser Session verifiziert, keine Neuinstallation nötig): Flutter 3.47.0 unter `C:\src\flutter`, Android SDK unter `%LOCALAPPDATA%\Android\Sdk`, `adb` funktionsfähig, Fire HD 10 (Geräte-ID `G001KT06143314SU`, Codename `KFTRWI`/`trona`, Android 9/API 28) per USB verbunden und autorisiert.
- **applicationId**: `de.olive.opel1935dashboard`
- **minSdkVersion**: 23 (BLE-Pakete wie `flutter_blue_plus` empfehlen ≥21–23 für stabile BLE-Callbacks; Fire HD 10 hat API 28, also unkritisch)
- **targetSdkVersion**: 34

**Abhängigkeiten (`pubspec.yaml`):**

| Package | Zweck |
|---|---|
| `flutter_blue_plus` | BLE-Transport (aktiv gepflegter `flutter_blue`-Fork) |
| `wakelock_plus` | Bildschirm dauerhaft an (Kiosk) |
| `shared_preferences` | Persistenz für `SettingsService` |
| `provider` | Einfache State-Verteilung `VehicleData` → Gauge-Widgets |
| `google_fonts` | `Oswald` + `Cinzel` |
| (kein Paket für TCP nötig – `dart:io Socket`) | |
| `flutter_test` (dev, bundled) | Unit-Tests |

**AndroidManifest.xml – Ergänzungen:**
```xml
<uses-permission android:name="android.permission.INTERNET"/>
<uses-permission android:name="android.permission.ACCESS_WIFI_STATE"/>
<uses-permission android:name="android.permission.CHANGE_WIFI_STATE"/>
<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30"/>
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30"/>
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" tools:targetApi="s"/>
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30"/>
```
`<activity>`: `android:screenOrientation="landscape"`, `android:launchMode="singleTask"`, Theme ohne ActionBar (Flutter-Default-Launch-Theme ist bereits fullscreen-fähig).

---

## 4. Architektur & Dateiübersicht

```text
lib/
  main.dart                          # Kiosk-Setup (immersive, orientation, wakelock), MultiProvider, MaterialApp
  models/
    vehicle_data.dart                 # VehicleFieldValue{value,updatedAt,stale}, VehicleData{rpm,speed,coolant,fuel,ignition,connected,vin,calibrationId}
    pid_definition.dart               # PidDefinition{mode,pidHex,name,byteCount,decode(bytes)->double,priorityTierMs}, statische Katalog-Liste aller 1.4-PIDs
  obd/
    elm327_transport.dart             # abstract Elm327Transport {connect(), disconnect(), byteStream, write(bytes), connectionState}
    tcp_transport.dart                 # dart:io Socket-Implementierung, Default host 192.168.4.1:35000
    ble_transport.dart                 # flutter_blue_plus-Implementierung, Scan nach Service FFE0 / Name "ELM327 Opel1935"
    elm327_client.dart                 # Framing (Buffer bis '>'), Echo-Stripping, sequentielle Command-Queue, initializeAdapter()
    elm327_parser.dart                 # reine Funktionen: parseResponseBlock(String) -> ParseResult (noData/error/data{mode,pid,bytes}), ISO-TP-Reassemblierung
    obd_pid_decoder.dart               # reine Dekodier-Funktionen je PID (Formeln aus 1.4), decodeMode09Text()
  services/
    vehicle_polling_service.dart       # Weighted-Fair-Scheduler (Details 5.3), publiziert VehicleData via ChangeNotifier/Stream
    connection_manager.dart            # Reconnect mit Exponential Backoff, orchestriert Transport+Client+PollingService Lifecycle
    settings_service.dart              # SharedPreferences: transportType, tcpHost, tcpPort, bleDeviceId, Polling-Intervalle
  screens/
    dashboard_screen.dart              # Landscape-Layout aller 5 Gauges + ConnectionStatusBadge
    settings_screen.dart               # Transport-Auswahl (TCP/BLE), Host/Port, BLE-Scan+Auswahl, Polling-Intervalle (advanced)
    debug_log_screen.dart              # Ringpuffer letzter 100 Command/Response-Paare, gemessene Ist-Hz je PID, VIN/Calibration-ID-Anzeige, Reconnect-Button
  widgets/
    base_gauge_painter.dart            # Basis-CustomPainter: Lünette, Zifferblatt, Ticks, Farbzonen, Zeiger, Glasreflexion
    rpm_gauge.dart
    speed_gauge.dart
    coolant_gauge.dart
    fuel_gauge.dart
    ignition_gauge.dart
    connection_status_badge.dart       # kleines Overlay: Transport-Typ, verbunden/getrennt/veraltet
  theme/
    opel1935_theme.dart                # Farben, TextStyles, Skalen-Konstanten aus Abschnitt 2

test/
  obd/elm327_parser_test.dart          # Test-Vektoren aus 1.5/1.6
  obd/obd_pid_decoder_test.dart        # Formeln + Grenzwerte aus 1.4/1.6
  services/vehicle_polling_service_test.dart  # Scheduler-Priorisierung (Fake-Transport)
```

---

## 5. Kern-Algorithmen (verbindliche Spezifikation)

### 5.1 `Elm327Client` – Framing & Command-Queue

- Byte-Stream vom Transport wird in einen `StringBuffer` akkumuliert (ASCII-Decoding).
- Sobald `>` im Buffer erscheint: alles davor als "Raw-Response-Block" abspalten, aus dem Buffer entfernen, an den wartenden `Completer<String>` liefern.
- **Nur eine Anfrage gleichzeitig in Flight** (User-Vorgabe): einfache Queue via verketteter `Future`s (`_lastOperation = _lastOperation.then((_) => _sendInternal(cmd))`) – kein externes Locking-Paket nötig.
- `sendRaw(command, {timeout = Duration(seconds: 2)})`: schreibt `command + '\r'`, wartet auf Completer mit Timeout → bei Timeout `TimeoutException` werfen (signalisiert `ConnectionManager` einen Verbindungsfehler).
- **Echo-Robustheit**: Falls die erste Zeile des Raw-Blocks (getrimmt, case-insensitive) exakt dem gesendeten Kommando entspricht, diese Zeile vor der weiteren Verarbeitung verwerfen (Schutz, falls `ATE0` fehlschlägt oder ein Reset zwischendurch passiert).
- `initializeAdapter()`: sendet die 6 Befehle aus 1.2 sequenziell, prüft jede Antwort gegen die Erwartung; wirft `AdapterInitException` bei Abweichung (führt zu Reconnect-Retry).

### 5.2 `Elm327Parser.parseResponseBlock(String raw) -> ParseResult`

1. Split nach `\r|\n` (Regex, beide Zeichen als Trenner behandeln), trim, leere Zeilen verwerfen.
2. Zeile `"SEARCHING..."` verwerfen (sollte durch `ATSP6` nicht mehr vorkommen, defensiv trotzdem behandeln).
3. Zeile exakt `"NO DATA"` → `ParseResult.noData()`.
4. Zeile exakt `"?"` → `ParseResult.error("?")`.
5. Verbleibende Zeile(n) sind Hex-Token-Zeilen (durch Leerzeichen getrennt, da `ATS1`):
   - **1 Zeile** → Bytes = Tokens 1:1 (Single Frame, kein PCI-Byte). Ergebnis-Payload = diese Bytes.
   - **>1 Zeile** → ISO-TP-Reassemblierung:
     - Zeile 1 Tokens `[b0, b1, ...]`: `totalLen = ((b0 & 0x0F) << 8) | b1`; die restlichen Tokens dieser Zeile (ab Index 2) sind die ersten Payload-Bytes.
     - Jede weitere Zeile: Token 0 ist PCI (`0x20 | seq`, wird nur zur Reihenfolge-Validierung genutzt, nicht Teil der Payload), restliche Tokens sind weitere Payload-Bytes.
     - Alle Payload-Bytes aneinanderhängen, **auf genau `totalLen` Bytes trimmen** (Zero-Padding der letzten Consecutive-Frame-Zeile verwerfen!).
6. Payload-Bytes[0] = Mode-Response-Byte (`0x41`/`0x49`), Payload-Bytes[1] = PID, Rest = Nutzdaten → `ParseResult.data(mode, pid, dataBytes)`.

### 5.3 `VehiclePollingService` – priorisiertes zyklisches Polling

- Task-Liste mit Default-Intervallen (aus SettingsService überschreibbar):

| PID | Ziel-Intervall | Ziel-Frequenz |
|---|---|---|
| `0x0C` RPM | 125 ms | 8 Hz |
| `0x0D` Speed | 125 ms | 8 Hz |
| `0x0E` Ignition | 300 ms | ~3,3 Hz |
| `0x05` Coolant | 1000 ms | 1 Hz |
| `0x2F` Fuel | 1000 ms | 1 Hz |

- Scheduler-Schleife (getrieben durch Response-Ankunft, **kein** fester Timer – passt sich automatisch an TCP- vs. BLE-Durchsatz an):
  ```
  while (connected) {
    task = tasks.reduce(maxBy: (now - task.lastPolledAt) / task.targetIntervalMs)  // größtes "Überfällig"-Verhältnis
    response = await elm327Client.sendRaw("01" + task.pidHex)
    result = Elm327Parser.parseResponseBlock(response)
    value = ObdPidDecoder.decode(task.pid, result)
    task.lastPolledAt = now
    publish VehicleData-Update (value fresh, updatedAt=now, stale=false)
  }
  ```
- Bei Exception (Timeout/Socket-Fehler) aus `sendRaw`: Schleife stoppen, letzte Werte bleiben mit `stale=true` sichtbar (nicht auf 0/null zurücksetzen), Kontrolle an `ConnectionManager` übergeben.
- Mode-09-Abfragen (`0902`, `0904`) einmalig nach jedem erfolgreichen `initializeAdapter()` (nicht Teil der zyklischen Task-Liste).

### 5.4 `ConnectionManager` – Reconnect mit Backoff

- Zustände: `disconnected → connecting → initializing → polling → (Fehler) → backoffWait → connecting → ...`
- Backoff-Sequenz: 1 s, 2 s, 4 s, 8 s, 15 s (danach konstant 15 s wiederholen), Reset auf 1 s nach jedem erfolgreichen `initializeAdapter()`.
- Bei Transport-Wechsel (User ändert Einstellung TCP↔BLE) laufenden Reconnect-Zyklus abbrechen und mit neuem Transport neu starten.

### 5.5 `ObdPidDecoder` – Formeln (exakt invers zu `VehicleState.cpp`)

```dart
double decodeRpm(List<int> d) => ((d[0] << 8) | d[1]) / 4.0;
double decodeCoolant(List<int> d) => d[0] - 40.0;
double decodeSpeed(List<int> d) => d[0].toDouble();
double decodeIgnitionAdvance(List<int> d) => d[0] / 2.0 - 64.0;
double decodeFuel(List<int> d) => d[0] * 100.0 / 255.0;
String decodeMode09Text(List<int> data) => ascii.decode(data.sublist(1)); // data[0] = Record-Index, verwerfen
```

---

## 6. UI/UX-Spezifikation

- **`main.dart`**: vor `runApp` → `SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky)`, `SystemChrome.setPreferredOrientations([landscapeLeft, landscapeRight])`, `WakelockPlus.enable()`.
- **DashboardScreen**: Querformat, große RPM- und Speed-Gauge zentral/prominent, Coolant/Fuel/Ignition kleiner darunter/daneben. `ConnectionStatusBadge` dezent in einer Ecke (Transport-Typ, Status-Punkt grün/gelb(stale)/rot). Zugriff auf Settings über ein kleines, dezentes Icon (kein grelles UI-Element, um die historische Optik nicht zu stören); DebugLogScreen über Dreifach-Tap auf die VIN-Anzeige erreichbar.
- **SettingsScreen**: Radio-Auswahl TCP/BLE, TCP-Host/Port-Felder (Default `192.168.4.1:35000`), BLE-Scan-Liste + Verbinden-Button, erweiterte Polling-Intervall-Felder (optional, mit sinnvollen Grenzen).
- **DebugLogScreen**: Verbindungsstatus, Transport, Ringpuffer letzter 100 Command/Response-Zeilen, gemessene Ist-Frequenz je PID (vs. Ziel), VIN/Calibration-ID, "Jetzt neu verbinden"-Button, Hinweistext zur bekannten Firmware-Limitierung (1.7).
- **Gauge-Animation**: jedes Gauge-Widget mit `AnimationController` (600–800 ms, `Curves.easeOutCubic`), tweent von altem zu neuem Wert bei jedem `VehicleData`-Update, gerendert via `AnimatedBuilder` in den jeweiligen `CustomPainter`.

---

## 7. Test-Plan

`flutter test` im Projektordner. Verbindliche Testfälle (Vektoren aus Abschnitt 1.5/1.6):

**`elm327_parser_test.dart`:**
- Single-Frame `"41 0C 0B B8\r>"` → `data(mode:0x41, pid:0x0C, bytes:[0x0B,0xB8])`
- `"NO DATA\r>"` → `noData()`
- `"?\r>"` → `error()`
- Multi-Frame VIN (exakter String aus 1.5) → reassemblierte Bytes `[0x49,0x02,0x01,0x31,0x30,0x2D,0x35,0x38,0x30,0x39,0x30]`, Padding-Nullen NICHT enthalten
- Multi-Frame Calibration-ID (exakter String aus 1.5) → korrekt reassemblierte 16 Bytes
- Echo-Robustheit: Eingabe mit vorangestellter Echo-Zeile wird korrekt ignoriert

**`obd_pid_decoder_test.dart`:**
- RPM: `[0x0B,0xB8]` → `750.0`; `[0xFF,0xFF]` → `16383.75`; `[0,0]` → `0.0`
- Coolant: `[0x7A]` → `82.0`; `[0]` → `-40.0`; `[255]` → `215.0`
- Speed: `[42]` → `42.0`
- Ignition: `[148]` → `10.0`; `[0]` → `-64.0`; `[255]` → `63.5`
- Fuel: `[77]` → im Bereich `30.2 ± 0.5`
- Mode09-Text: reassemblierte VIN-Bytes → `"10-58090"`; Calibration-Bytes → `"Opel 1,2 ltr."`

**`vehicle_polling_service_test.dart`** (mit Fake-`Elm327Client`):
- RPM/Speed werden über ein Zeitfenster deutlich häufiger gepollt als Coolant/Fuel (Verhältnis grob proportional zu den Ziel-Intervallen)
- Bei simuliertem Transport-Fehler bleibt letzter `VehicleData`-Wert erhalten, nur `stale=true` gesetzt

---

## 8. Setup & Deployment (Windows/VS Code, aufbauend auf bereits verifizierter Umgebung dieser Session)

1. Neues Terminal öffnen (damit `flutter`/`adb` im PATH sind – bereits eingerichtet).
2. Projekt anlegen:
   ```powershell
   cd C:\Users\olive\Documents\Flutter
   flutter create --org de.olive --project-name opel1935_dashboard opel1935_dashboard
   ```
3. Dependencies gemäß Abschnitt 3 in `pubspec.yaml` ergänzen, `flutter pub get`.
4. `android/app/build.gradle`: `minSdkVersion 23`, `targetSdkVersion 34`, `applicationId "de.olive.opel1935dashboard"`.
5. `android/app/src/main/AndroidManifest.xml`: Permissions + Activity-Flags aus Abschnitt 3 ergänzen.
6. Alle Dateien aus Abschnitt 4 gemäß Spezifikation aus Abschnitt 5/6 implementieren (Implementierungs-Agent).
7. Tests lokal ausführen: `flutter test`.
8. Fire HD 10 vorbereiten:
   - Für **TCP**: Tablet einmalig mit WLAN `Opel1935` (offen, kein Passwort) verbinden (Android merkt sich das Netzwerk).
   - Für **BLE**: Tablet-Bluetooth aktivieren, App fragt beim ersten Start Berechtigungen ab.
9. Deployment: `flutter devices` → Fire HD 10 (`G001KT06143314SU`) muss gelistet sein → `flutter run -d G001KT06143314SU` (Debug, Hot Reload) bzw. für Sideload:
   ```powershell
   flutter build apk --release
   adb install -r build\app\outputs\flutter-apk\app-release.apk
   ```
   (Release-APK ist unsigniert/debug-signiert, für reines Sideloading auf dem eigenen Gerät ausreichend; ein echtes Signing-Keystore ist nur für Play-Store-Distribution relevant und hier nicht im Scope.)
10. App auf dem Tablet manuell starten, Firewall/AP-Verbindung prüfen (`adb shell ping 192.168.4.1` bei TCP-Problemen).

---

## 9. Abnahme-Checkliste

- [ ] `flutter analyze` ohne Fehler, `flutter test` grün (alle Vektoren aus Abschnitt 7)
- [ ] App startet im Kiosk-Modus (keine Systembars, Landscape fixiert, Bildschirm bleibt an)
- [ ] TCP-Verbindung zu `192.168.4.1:35000` funktioniert, Init-Sequenz (1.2) erfolgreich, alle 5 Gauges zeigen die Firmware-Default-Werte (RPM 750, Speed 42, Coolant 82°C, Fuel ~30%, Ignition 10°)
- [ ] BLE-Verbindung zu `ELM327 Opel1935` funktioniert als Alternative
- [ ] Verbindungsabbruch (WLAN aus) → Gauges bleiben auf letztem Wert stehen (visuell als "stale" erkennbar), automatischer Reconnect mit Backoff greift nach Wiederverbindung
- [ ] DebugLogScreen zeigt Roh-Kommunikation, VIN `"10-58090"`, Calibration-ID `"Opel 1,2 ltr."`
- [ ] Zeiger-Animationen sind flüssig (kein Sprung bei Werteänderung)

## 10. Bekannte Einschränkungen / Out-of-Scope (dokumentiert statt versteckt)

- Keine echten Referenzbilder verfügbar – alle Skalen/Farben/Schriften in Abschnitt 2 sind begründete Annahmen, über `theme/opel1935_theme.dart` zentral austauschbar.
- `VehicleState` in der Firmware liefert aktuell nur statische Werte (Abschnitt 1.7) – eine Firmware-Erweiterung zur dynamischen Simulation ist **nicht** Teil dieses Plans.
- Keine Play-Store-Signierung/Release-Prozess – nur lokales Sideloading via ADB.
- "Punkt 28 Kontrollfragen" aus dem Original-Auftrag existieren nicht und werden durch die Checkliste in Abschnitt 9 ersetzt.
