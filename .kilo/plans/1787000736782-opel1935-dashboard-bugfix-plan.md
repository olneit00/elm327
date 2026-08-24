# Opel 1935 Dashboard – Bugfix-Plan (Verbindung, Rendering, Kiosk-Ausweg, WLAN-Status)

## 0. Kontext

Nach der Erstimplementierung (`1786989298620-opel1935-flutter-dashboard-plan.md`) wurde die App auf dem Fire HD 10 getestet. `flutter analyze`/`flutter test` waren grün, aber die App ist auf dem Gerät **nicht benutzbar**. Diagnose erfolgte per Debug-Log, `adb shell ping`, `adb dumpsys wifi` und einem Live-Screenshot (`adb exec-out screencap`) – keine Spekulation, alle vier folgenden Root Causes sind durch Log/Screenshot/Netzwerktest belegt.

## 1. Bestätigte Root Causes

### 1.1 KRITISCH: `ConnectionManager` wird nie gestartet

In `lib/main.dart`:
```dart
void main() async {
  ...
  final settings = SettingsService();
  runApp(Opel1935DashboardApp(settings: settings));
  final connectionManager = ConnectionManager(settings);
  connectionManager.start();   // <- Instanz A: wird gestartet, aber nirgendwo verwendet
}

class Opel1935DashboardApp extends StatelessWidget {
  Widget build(BuildContext context) {
    final connectionManager = ConnectionManager(settings);  // <- Instanz B: NEU erzeugt, NIE gestartet
    return MultiProvider(
      providers: [
        ChangeNotifierProvider.value(value: connectionManager),  // Instanz B geht in den Widget-Baum
      ],
      ...
    );
  }
}
```
Zwei verschiedene `ConnectionManager`-Objekte: Instanz A wird gestartet und sofort verworfen (kein Listener, kein Provider), Instanz B geht in den Provider-Baum, wird aber nie `.start()`-et. Die App versucht dadurch **nie**, sich mit TCP/BLE zu verbinden → Badge zeigt dauerhaft "Getrennt", VIN/Calibration-ID bleiben leer, alle Gauges zeigen konstant den Default-Wert 0.

**Beleg gegen alternative Theorie (Android routet WLAN ohne Internet weg):** `adb shell ping -c 4 192.168.4.1` → 0% Paketverlust, ~10ms RTT. Netzwerk/WLAN sind einwandfrei, Android muss NICHT per Network-Binding gezwungen werden. Reiner App-Bug.

### 1.2 KRITISCH: Gauges rendern nicht (negativer Radius)

Alle Gauge-Widgets (`RpmGauge`, `SpeedGauge`, `CoolantGauge`, `FuelGauge`, `IgnitionGauge`) verwenden `CustomPaint(painter: ...)` **ohne** `size`- oder `child`-Parameter. Damit fällt Flutter auf `preferredSize = Size.zero` zurück. In der Row/Column-Flex-Struktur von `DashboardScreen` bekommen diese Widgets im Kreuz-Achsen (Höhe) nur lose Constraints (`0..maxHeight`) – `CustomPaint` wählt dann effektiv Höhe 0 (bzw. eine sehr kleine/degenerate Größe).

In `BaseGaugePainter.paint()`:
```dart
final radius = min(size.width, size.height) / 2 - 8;
```
Wird `size` degenerate (eine Dimension ≈0), wird `radius` **negativ**. Folge:
- `canvas.drawCircle(center, radius, ...)` mit negativem Radius zeichnet **nichts** (Zifferblatt, Lünette, Nabe unsichtbar).
- `canvas.drawLine(...)` (Ticks, Zeiger) ist von negativem Radius **nicht** betroffen (jeder Punkt ist ein gültiger Linienendpunkt) und zeichnet trotzdem – mit kaputter, winziger Geometrie.

**Beleg:** Screenshot zeigt exakt dieses Muster – winzige verstreute rote Linien-Cluster (Zeiger + Ticks) ohne sichtbares cremefarbenes Zifferblatt oder Chrom-Lünette; die Zifferblatt-Beschriftung (`#1A1A1A`, fast schwarz) sitzt dadurch direkt auf dem schwarzen Scaffold-Hintergrund statt auf dem (nie gezeichneten) hellen Zifferblatt → "schwarze Schrift auf schwarzem Grund".

### 1.3 Kiosk-Lockout (Design-Fehler)

`SystemUiMode.immersiveSticky` + `android:launchMode="singleTask"` + Landscape-Lock, ohne jeden Fluchtweg. Nutzer kann weder Systemleisten noch Android-WLAN-Einstellungen erreichen.

### 1.4 Kein Android-WLAN/BT-Status sichtbar

Aktuell zeigt die App nur den App-internen OBD-Verbindungsstatus (`ConnectionStatusBadge`), nicht den tatsächlichen Android-WLAN-Status/SSID.

---

## 2. Entschiedene Lösungsansätze

Nutzer-Entscheidung zum Kiosk-Ausweg: **Kombination** – WLAN-Settings-Button als primärer Weg (Android-Intent direkt aus der App, kein echtes Verlassen der App nötig) **plus** `SystemUiMode.edgeToEdge` statt `immersiveSticky` als Fallback-Sicherheitsnetz (Systemleisten bleiben per Swipe erreichbar).

Für den WLAN/BT-Status: SSID-Anzeige via `network_info_plus` (nutzt die bereits im Manifest vorhandene `ACCESS_FINE_LOCATION`-Berechtigung, die ohnehin für BLE-Scan nötig ist – kein zusätzlicher Berechtigungs-Mehraufwand). Bei verweigerter Berechtigung: Fallback-Text statt Absturz.

---

## 3. Implementierungsschritte

### 3.1 `lib/main.dart` – ConnectionManager-Singleton-Bug fixen

- `ConnectionManager` **einmalig** in `main()` erzeugen, `.start()` darauf aufrufen, dieselbe Instanz per Konstruktor-Parameter an `Opel1935DashboardApp` übergeben.
- `Opel1935DashboardApp.build()` darf **keine neue** `ConnectionManager`-Instanz mehr erzeugen – nur noch `ChangeNotifierProvider.value(value: connectionManager)` mit der übergebenen Instanz.
- `SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky)` → `SystemUiMode.edgeToEdge` ändern.

### 3.2 Gauge-Rendering robust machen

**`lib/widgets/rpm_gauge.dart`, `speed_gauge.dart`, `coolant_gauge.dart`, `fuel_gauge.dart`, `ignition_gauge.dart`:**
- Jedes Gauge-Widget mit `LayoutBuilder` umschließen, das die tatsächlich verfügbaren `constraints.maxWidth`/`maxHeight` liest, daraus `side = min(maxWidth, maxHeight)` berechnet und `CustomPaint(size: Size(side, side), painter: ...)` explizit mit dieser Größe instanziiert. Damit ist die Canvas-Größe garantiert positiv und quadratisch, unabhängig von Row/Column-Loose-Constraints.

**`lib/widgets/base_gauge_painter.dart`:**
- Defensive Absicherung zusätzlich einbauen: `final radius = (min(size.width, size.height) / 2 - 8).clamp(4.0, double.infinity);` – verhindert für alle Zukunft, dass ein negativer/degenerierter Radius überhaupt entstehen kann, selbst falls an anderer Stelle nochmal ein Layout-Sizing-Problem auftritt.
- Optional (geringes Risiko, zusätzliche Absicherung): vor dem Gradient-Bezel/Dial einen soliden, undurchsichtigen Basis-Fill in `Opel1935Theme.dialColor` zeichnen, damit auch bei Shader-Problemen immer ein heller Untergrund für den Text vorhanden ist.

### 3.3 Kiosk-Ausweg: WLAN-Settings-Button

- Package `app_settings` zu `pubspec.yaml` hinzufügen.
- In `lib/screens/dashboard_screen.dart`: zusätzlichen Icon-Button (WLAN-Symbol) neben dem bestehenden Settings-Gear platzieren, der `AppSettings.openWifiSettings()` aufruft (öffnet Android-WLAN-Auswahl direkt, App bleibt im Hintergrund aktiv/wird nicht beendet).

### 3.4 Android-WLAN-Status anzeigen

- Package `network_info_plus` zu `pubspec.yaml` hinzufügen.
- Neuer Service oder Erweiterung in `lib/services/settings_service.dart` (oder eigener kleiner `WifiStatusService`): periodische Abfrage (z. B. alle 5s) von `NetworkInfo().getWifiName()` (Android liefert SSID ggf. mit Anführungszeichen – trimmen). Bei `PermissionDenied`/`null`: Fallback-String `"WLAN: unbekannt"`.
- Anzeige in `lib/widgets/connection_status_badge.dart` (SSID neben/unter dem bestehenden OBD-Status) und ausführlicher in `lib/screens/debug_log_screen.dart` (SSID + Hinweis, dass das für BLE ohnehin genutzte Standort-Recht wiederverwendet wird).

### 3.5 `pubspec.yaml`

Ergänzen:
```yaml
dependencies:
  app_settings: ^5.1.1
  network_info_plus: ^5.0.3
```
(Aktuelle stabile Versionen zum Zeitpunkt der Umsetzung via `flutter pub add app_settings network_info_plus` prüfen/festlegen.)

---

## 4. Tests

### 4.1 Regressionstest für den Start-Bug

`test/services/connection_manager_test.dart` (neu):
- Mit einer Fake-/Test-`SettingsService` und ggf. einem injizierbaren Transport-Factory-Hook (falls nötig minimal in `ConnectionManager` ergänzen, um Tests ohne echten Socket zu ermöglichen): `ConnectionManager.start()` aufrufen und verifizieren, dass `state` von `disconnected` zu `connecting` wechselt (Beweis, dass `.start()` tatsächlich `_connect()` auslöst). Dieser Test hätte den ursprünglichen Bug (falsche Instanz gestartet) zwar nicht direkt gefangen – das ist primär ein Wiring-Fehler in `main.dart`, der durch Code-Review/manuelles Testen abgedeckt wird – dient aber als dauerhafte Absicherung, dass `start()` grundsätzlich funktioniert.

### 4.2 Regressionstest für den Rendering-Bug

`test/widgets/base_gauge_painter_test.dart` (neu):
- `BaseGaugePainter(...).paint(canvas, Size.zero)` sowie `Size(1, 1)` (Grenzfälle) direkt auf einem `Canvas` aus `PictureRecorder` aufrufen und sicherstellen, dass **keine Exception** wirft (belegt den Clamp-Fix).
- Zusätzlich `paint(canvas, Size(200, 200))` aufrufen und sicherstellen, dass ebenfalls keine Exception wirft (Normalfall weiterhin funktionsfähig).

### 4.3 Bestehende Tests

- Alle bisherigen 31 Tests (`elm327_parser_test.dart`, `obd_pid_decoder_test.dart`, `vehicle_polling_service_test.dart`, `widget_test.dart`) müssen weiterhin grün bleiben.

---

## 5. Validierung auf dem Gerät (nach Implementierung)

1. `flutter analyze` – keine Fehler/Warnungen.
2. `flutter test` – alle Tests grün (inkl. der 2 neuen Testdateien).
3. `flutter run -d G001KT06143314SU` (Debug, USB) – Terminal-Log auf Exceptions prüfen.
4. Visuelle Prüfung: ConnectionStatusBadge wechselt von "Verbinde..." zu grün "TCP" (Tablet ist bereits im WLAN `Opel1935`) – bestätigt Fix 1.1.
5. Visuelle Prüfung: alle 5 Gauges zeigen volles Zifferblatt (creme), Chrom-Lünette, lesbare schwarze Beschriftung, roten Zeiger auf Position der Firmware-Default-Werte (RPM 750, Speed 42, Coolant 82°C, Fuel ~30%, Ignition 10°) – bestätigt Fix 1.2.
6. WLAN-Icon-Button tippen → Android-WLAN-Einstellungen öffnen sich, App bleibt im Hintergrund aktiv (kein Force-Stop) – bestätigt Fix 3.3.
7. SSID `Opel1935` wird im Badge/Debug-Screen angezeigt (oder Fallback-Text, falls Berechtigung verweigert) – bestätigt Fix 3.4.
8. Von einem Bildschirmrand nach innen wischen (edgeToEdge) → Systemleiste erscheint kurz, App bleibt im Vordergrund – bestätigt Fallback-Ausweg aus 2.
9. `adb build apk --release` + Sideload, gleiche Prüfungen im Release-Build wiederholen (Debug-Verhalten kann von Release abweichen, insbesondere bei Assertions).

---

## 6. Nicht im Scope

- Firmware-Änderungen (weiterhin nur statische `VehicleState`-Werte, wie in Abschnitt 1.7 des Ursprungsplans dokumentiert).
- Vollständiges Redesign des Zifferblatt-Layouts/Skalierung über die reine Bugfix-Notwendigkeit hinaus (Größenverhältnisse RPM/Speed vs. Coolant/Fuel/Ignition bleiben wie ursprünglich geplant, nur die technische Rendering-Grundlage wird repariert).
- Play-Store-Signierung/Verteilung (weiterhin nur lokales Sideloading).
