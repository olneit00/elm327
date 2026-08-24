# Plan: Mehrpunkt-Messtabelle in der Zeiger-Kalibrierung (zusätzlich zu MIN/MAX)

Zielprojekt (Flutter-App, NICHT der aktuelle Firmware-Workspace):
`C:\Users\olive\Documents\Flutter\opel1935_dashboard`

## Ausgangslage

Der `CalibrationScreen` (`lib/screens/calibration_screen.dart`) hat bereits:

- Pro Instrument einen Winkel-Stepper (`-10 / -1 / +1 / +10`), aktueller Winkel
  als Text unter dem Zifferblatt.
- „Als MIN speichern“ / „Als MAX speichern“ / „Zurücksetzen“, die über
  `CalibrationStore` (SharedPreferences-backed, `lib/services/calibration_store.dart`)
  linear `startAngleDeg`/`sweepDeg` persistieren und von `InstrumentSpecs.angleFor()`
  fürs Dashboard gelesen werden.

Diese lineare 2-Punkt-Kalibrierung (MIN/MAX) reicht nicht für Zifferblätter mit
nicht-linearer Skalenteilung. Der Nutzer will zusätzlich pro Instrument **mehrere**
(Wert → Winkel)-Messpunkte aufnehmen (z. B. bei 0/20/40/60/80/100/120 km/h je den
exakten Winkel notieren), um daraus **in einem späteren, separaten Schritt** eine
genauere (ggf. stückweise-lineare) Winkelberechnung in den Code zu übernehmen.

## Entschiedene Eckpunkte (aus Rückfrage-Runde)

- **Koexistenz**: Die bestehende MIN/MAX-Speicherung bleibt unverändert erhalten.
  Neu ist ein zusätzlicher Modus auf demselben Screen.
- **Ort**: Kein neuer Screen. `CalibrationScreen` bekommt einen Modus-Umschalter
  („Linear (MIN/MAX)“ / „Mehrpunkt-Tabelle“), der nur den unteren Bedienbereich
  wechselt; Zifferblatt-Anzeige und Instrumenten-Bottom-Nav bleiben gleich.
- **Persistenz der Mehrpunkt-Tabelle**: Ja, dauerhaft je Instrument in
  SharedPreferences (JSON-kodiert), damit ein Absturz/Force-Stop während einer
  längeren Messreihe keine Punkte verwirft.
- **Werteingabe**: Freies Zahlenfeld (`TextField`, numerisch) für den
  Realwert (z. B. „20“ für 20 km/h) – keine Preset-Buttons, funktioniert generisch
  für alle 4 Instrumente ohne zusätzliche Tick-Mark-Konfiguration.
- **Winkel-Anzeige**: Der aktuelle Winkel wird zusätzlich als Overlay direkt auf
  dem Instrumentenbild angezeigt (nicht nur wie bisher als Text darunter) – in
  **beiden** Modi.
- **Kein Export**: Kein Zwischenablage/Copy-Feature. Der Nutzer liest die Tabelle
  direkt vom Bildschirm ab und meldet die Werte selbst.
- **Kein Runtime-Effekt**: `InstrumentSpecs.angleFor()` und die Dashboard-Zeiger
  werden in diesem Schritt **nicht** verändert. Die Mehrpunkt-Tabelle ist reine
  Erfassung; das Einbacken in eine (ggf. stückweise-lineare) `angleFor()`-Logik
  ist explizit ein separater, späterer Schritt, sobald der Nutzer die Werte
  durchgegeben hat.

## Umsetzungsschritte

### 1. `lib/services/calibration_points_store.dart` (neu)

- `class CalibrationPoint { final double value; final double angle; const CalibrationPoint(this.value, this.angle); }`
  - `Map<String, dynamic> toJson()` / `factory CalibrationPoint.fromJson(...)`.
- `class CalibrationPointsStore extends ChangeNotifier`:
  - Interner State: `Map<String, List<CalibrationPoint>> _points`.
  - `Future<void> init({SharedPreferences? prefs})` – lädt für jeden bekannten
    Instrument-Key `"calibpts.<key>"` (JSON-Array) aus SharedPreferences in den
    Cache; unbekannte/fehlende Keys ergeben eine leere Liste.
  - `List<CalibrationPoint> pointsFor(String key)` – gibt die (nach `value`
    aufsteigend sortierte) Liste zurück (unveränderliche Kopie).
  - `Future<void> addPoint(String key, double value, double angle)` – fügt einen
    Punkt ein (an sortierter Position nach `value`), persistiert die gesamte
    Liste als JSON-String, `notifyListeners()`. Kein Dedup – doppelte `value`s
    sind erlaubt (liegt in der Verantwortung des Nutzers).
  - `Future<void> removeAt(String key, int sortedIndex)` – entfernt den Punkt an
    der (sortierten) Anzeigeposition, persistiert, `notifyListeners()`.
  - `Future<void> clear(String key)` – leert die Liste für dieses Instrument,
    persistiert (leeres Array oder Key entfernen), `notifyListeners()`.
  - Guard: ungültige/NaN-Werte beim Hinzufügen abweisen (still `return`, kein
    Crash) – Validierung des Zahlenfelds passiert aber schon in der UI (siehe
    Schritt 3).

### 2. `lib/main.dart`

- Analog zu `CalibrationStore`: `final calibrationPointsStore = CalibrationPointsStore(); await calibrationPointsStore.init();`
  vor `runApp`, als zusätzliches Feld an `Opel1935DashboardApp` übergeben, per
  `ChangeNotifierProvider.value` bereitstellen.

### 3. `lib/screens/calibration_screen.dart` überarbeiten

- Neuer lokaler State: `enum _CalibMode { linear, multiPoint }`, Feld
  `_mode = _CalibMode.linear` (Default unverändert wie bisher).
- Direkt unter der Erklärungs-Zeile: `SegmentedButton<_CalibMode>` (oder
  `ToggleButtons`) mit Labels „Linear (MIN/MAX)“ / „Mehrpunkt-Tabelle“, wechselt
  `_mode` per `setState`.
- **Winkel-Overlay auf dem Instrument** (beide Modi): beim Aufbau des
  `InstrumentGauge` zusätzlich `valueLabel: '${angle.toStringAsFixed(0)}°'`
  übergeben (Widget unterstützt das Feld bereits, aktuell ungenutzt in diesem
  Screen).
- Bestehender Stepper (`-10/-1/+1/+10`) bleibt in **beiden** Modi sichtbar
  (wird für Mehrpunkt-Messung genauso gebraucht wie für MIN/MAX).
- Wenn `_mode == linear`: bestehender Block unverändert (MIN/MAX/Zurücksetzen +
  „Gespeichert: …“-Zeile).
- Wenn `_mode == multiPoint`: neuer Block:
  - Pro Instrument ein persistenter `TextEditingController` (Map keyed by
    `spec.key`, in `initState` befüllt für alle `InstrumentSpecs.all`, in
    `dispose()` aufgeräumt) für die Werteingabe; Inhalt bleibt beim
    Instrumentenwechsel erhalten (kein automatisches Clearen außer nach
    erfolgreichem Hinzufügen).
  - Row: `TextField` (schmal, `keyboardType: TextInputType.numberWithOptions(signed: true, decimal: true)`,
    Label/HintText z. B. „Wert (z. B. km/h)“) + `ElevatedButton('Punkt hinzufügen')`.
    - Validierung: `double.tryParse(controller.text.trim())`; bei `null`
      `ScaffoldMessenger.showSnackBar('Bitte gültigen Zahlenwert eingeben')` und
      abbrechen. Sonst `pointsStore.addPoint(spec.key, parsedValue, angle)`,
      danach `controller.clear()`.
  - Darunter: feste Höhe (z. B. `SizedBox(height: 160)`) mit
    `ListView.builder` über `pointsStore.pointsFor(spec.key)`, pro Zeile
    `Text('${p.value} → ${p.angle.toStringAsFixed(0)}°')` +
    `IconButton(Icons.delete_outline)` → `pointsStore.removeAt(spec.key, index)`.
  - `OutlinedButton('Tabelle leeren')` → `pointsStore.clear(spec.key)`.
- `context.watch<CalibrationPointsStore>()` zusätzlich zum bestehenden
  `context.watch<CalibrationStore>()`.

### 4. Tests

- Neuer Test `test/services/calibration_points_store_test.dart`:
  - `addPoint` fügt Punkte sortiert nach `value` ein.
  - `removeAt` entfernt den korrekten (sortierten) Eintrag.
  - `clear` leert die Liste.
  - Reload (`CalibrationPointsStore().init()` mit denselben Mock-Prefs) liefert
    die zuvor persistierten Punkte unverändert zurück (inkl. Sortierreihenfolge).
  - Ungültiger/NaN-Wert wird abgewiesen, kein Crash, Liste bleibt unverändert.
- `flutter analyze` (0 Findings) und `flutter test` (alle grün) müssen weiterhin
  bestehen – bestehende Tests (`calibration_store_test.dart`, Widget-Test, etc.)
  dürfen nicht brechen.

### 5. Build + Deploy

- `flutter build apk --debug`, `adb install -r` auf das bereits verbundene
  Tablet (`G001KT06143314SU`), danach `adb shell am force-stop
  de.olive.opel1935dashboard` + Neustart der App zum Persistenz-Nachweis.

## Nicht-Ziele

- Keine Änderung an `InstrumentSpecs.angleFor()` oder den Dashboard-Zeigern in
  diesem Schritt – reine Datenerfassung.
- Kein Export/Clipboard/Sharing der Mehrpunkt-Tabelle.
- Keine Preset-Tick-Mark-Buttons; nur freie Zahleneingabe.
- Keine Änderung der bestehenden MIN/MAX-Logik oder des `CalibrationStore`.

## Validierung

- `flutter analyze` (0 Findings) und `flutter test` (alle grün).
- Geräte-Test: Für mind. ein Instrument in den Mehrpunkt-Modus wechseln,
  3+ Punkte mit unterschiedlichen Werten/Winkeln hinzufügen, einen Punkt wieder
  löschen, App per `am force-stop` beenden und neu starten → Tabelle muss mit
  den verbleibenden Punkten identisch wieder angezeigt werden.
- Sichtprüfung: Winkel-Overlay ist auf dem Instrumentenbild selbst lesbar (nicht
  nur im Text darunter), in beiden Modi.
- Umschalten zwischen „Linear“ und „Mehrpunkt-Tabelle“ verändert weder den
  aktuell eingestellten Test-Winkel noch die MIN/MAX-Werte im `CalibrationStore`.

## Risiken / offene Punkte

- Lösch-Index-Mapping: `removeAt` muss sich auf die **sortierte** Anzeigeliste
  beziehen, nicht auf die interne Rohreihenfolge – bei der Persistenz sollte
  die Liste daher immer in sortierter Form gespeichert werden, damit Anzeige-
  und Speicher-Index konsistent bleiben.
- Doppelte `value`-Einträge sind erlaubt (kein Dedup) – bewusst in Kauf
  genommen, liegt in der Verantwortung des Nutzers beim Ablesen.
- Zahlenfeld-Parsing muss Dezimal-Komma vs. Punkt tolerant genug sein oder klar
  per `HintText` „z. B. 12.5“ auf das erwartete Format hinweisen, damit
  `double.tryParse` nicht grundlos fehlschlägt.
