# Plan: On-Device-Zeigerkalibrierung (statt Chat-Rundenschleife) + Uhr-Grafik reparieren

Zielprojekt (Flutter-App, NICHT der aktuelle Firmware-Workspace):
`C:\Users\olive\Documents\Flutter\opel1935_dashboard`

## Ausgangslage / Problem

Die bisherige Kalibrierung der Zeiger-Winkel (`startAngleDeg` / `sweepDeg` in
`lib/widgets/instrument_specs.dart`) lief über viele Chat-Runden:

1. Nutzer liest am Tablet einen Winkel/Zustand ab und beschreibt ihn in Worten.
2. Ich rechne daraus Start-/Sweep-Winkel, ändere den Dart-Code.
3. `flutter build apk` + Installation + Neustart auf dem Tablet (mehrere Minuten).
4. Nutzer prüft erneut, meldet Ergebnis, oft widersprüchlich zur vorigen Angabe.

Bei jedem Vorzeichen- oder Verwechslungsfehler (Grad-Konvention, Min/Max
vertauscht, welches Instrument gemeint war) beginnt der Zyklus neu. Das ist der
Haupttreiber der hohen Tokenkosten – nicht die Bildanalyse selbst.

Zusätzlicher Fehler: Ein zwischenzeitlich beauftragter Hintergrund-Agent hat
`assets/instruments/instrument_clock.png` mit einer falschen Grafik überschrieben
(12:59 Uhr, 227 662 Bytes). Die vom Nutzer bestätigte Uhr befindet sich links in
`instrument_09.png` (dort ursprünglich per Crop – Zentrum ca. (128,122), Radius
125 – ausgeschnitten, unverändert vorhanden, 202 519 Bytes, Zeitstempel 08:32).

## Zielbild

Die Kalibrierung wird **einmalig** durch ein App-Update ermöglicht und danach
**vollständig selbstständig auf dem Tablet** durchgeführt – ohne weitere
Chat-Runden, ohne erneuten Build/Deploy, ohne Übertragungsfehler:

- Pro Instrument: aktueller Testwinkel groß angezeigt, Knöpfe `-10 / -1 / +1 / +10`
  (bereits vorhanden in `calibration_screen.dart`, wird wiederverwendet).
- Neu: Knöpfe **„Als MIN speichern“** und **„Als MAX speichern“**. Der Nutzer
  dreht den Zeiger mit den Knöpfen exakt auf die kleinste Skalenmarke des echten
  Zifferblatts und drückt „Als MIN speichern“, dann auf die größte Marke und
  drückt „Als MAX speichern“.
- Die App berechnet daraus sofort `startAngleDeg = minAngle` und
  `sweepDeg = maxAngle - minAngle` und **persistiert das dauerhaft auf dem Gerät**
  (SharedPreferences – Paket ist bereits in `pubspec.yaml` deklariert, aber noch
  nicht genutzt).
- Direkt auf dem Kalibrierungs-Screen wird sichtbar: `Gespeichert: MIN=…° MAX=…° Sweep=…°`
  – der Nutzer sieht sofort, dass es angekommen ist, ohne mir etwas melden zu
  müssen.
- Das Dashboard (`instrument_specs.dart` → `angleFor()`) liest beim Zeichnen
  zuerst den gespeicherten Wert; ist keiner vorhanden, wird der Default-Wert aus
  dem Code verwendet.
- Ein „Zurücksetzen“-Knopf pro Instrument löscht den gespeicherten Wert wieder
  (Fallback auf Default).

Damit endet die Kalibrierung nach **einem** Build/Deploy komplett in der Hand
des Nutzers.

## Umsetzungsschritte

1. **`lib/services/calibration_store.dart`** (neu)
   - Kapselt `SharedPreferences`-Zugriff.
   - Schlüssel je Instrument: stabiler String (`"rpm"`, `"speed"`, `"coolant"`,
     `"fuel"`), nicht der Anzeigename.
   - API:
     - `Future<void> init()` – lädt beim App-Start einmal alle Werte in den
       Speicher (Cache), damit `angleFor()` synchron bleibt.
     - `double? startAngleFor(String key)`, `double? sweepFor(String key)`
     - `Future<void> setMinMax(String key, double minAngle, double maxAngle)`
       → speichert `startAngle = minAngle`, `sweep = maxAngle - minAngle`.
     - `Future<void> reset(String key)`
   - Guard: keine Division/NaN, falls `minAngle == maxAngle` (sweep=0 ist
     erlaubt, Zeiger bleibt dann fix – kein Crash).

2. **`instrument_specs.dart`**
   - `InstrumentSpec` bekommt zusätzliches Feld `key` (z. B. `'rpm'`).
   - `angleFor(spec, value)` fragt zuerst `CalibrationStore` (per Singleton
     oder übergebene Instanz) nach `startAngleFor(spec.key)` /
     `sweepFor(spec.key)`; nutzt sonst weiterhin `spec.startAngleDeg` /
     `spec.sweepDeg` als Default.

3. **`main.dart`**
   - `CalibrationStore` analog zu `SettingsService` vor `runApp` initialisieren
     (`await calibrationStore.init()`), per `Provider`/`ChangeNotifierProvider`
     bereitstellen, damit Dashboard und Kalibrierungs-Screen denselben Stand
     sehen und bei Änderung sofort neu zeichnen.

4. **`calibration_screen.dart`** überarbeiten
   - Pro Instrument zusätzlich zum bestehenden Winkel-Stepper:
     - Zeile „Gespeichert: MIN=…° MAX=…° Sweep=…°“ (liest aus
       `CalibrationStore`, `—` falls nicht gesetzt).
     - Knopf „Als MIN speichern“ → merkt sich `minAngle` lokal im Screen-State
       (noch nicht persistiert, bis auch MAX gesetzt ist) **oder** persistiert
       sofort mit dem zuletzt bekannten/Default-Max, je nachdem was zuerst
       gedrückt wird (Reihenfolge MIN/MAX beliebig).
     - Knopf „Als MAX speichern“ analog.
     - Knopf „Zurücksetzen“ → `CalibrationStore.reset(key)`.
   - Bestehende Navigation (3× Tippen auf VIN → Debug-Log → „Zeiger-Kalibrierung“)
     bleibt unverändert.

5. **Uhr-Grafik reparieren**
   - `instrument_clock.png` neu aus `instrument_09.png` zuschneiden (Python/
     Pillow, deterministischer Crop wie zuvor: Zentrum (128,122), Radius 125,
     quadratischer Ausschnitt), Ergebnis überschreibt die aktuell falsche Datei.
   - Kein Einsatz eines Bild-Agenten – reine, reproduzierbare Pixel-Operation.

6. **Tests**
   - Kleiner Unit-Test für `CalibrationStore`: `setMinMax` → `startAngleFor`/
     `sweepFor` liefern erwartete Werte; `reset` setzt zurück auf `null`.
   - `flutter analyze` und `flutter test` müssen weiterhin ohne Findings/mit
     allen Tests grün durchlaufen.

7. **Ein einziger Build + Deploy** auf das Tablet (`flutter build apk --debug`,
   `adb install -r`, Neustart der App).

8. **Kurzanleitung an den Nutzer** (nach Deploy, keine weitere Code-Iteration
   nötig):
   „3× auf die VIN oben links tippen → Debug-Log → ‚Zeiger-Kalibrierung‘. Pro
   Instrument mit den Knöpfen den Zeiger exakt auf die kleinste Marke des
   echten Zifferblatts drehen → ‚Als MIN speichern‘ drücken. Dann auf die
   größte Marke drehen → ‚Als MAX speichern‘ drücken. Das war’s – die App
   merkt sich das sofort dauerhaft und wendet es direkt im Dashboard an, auch
   nach einem Neustart der App.“

## Nicht-Ziele

- Keine weiteren Vision-Agent-Einsätze zur Bildanalyse oder Nadel-Entfernung
  (haben sich als unzuverlässig und zeitintensiv erwiesen).
- Keine Änderung an der ELM-Datenermittlung/Dekodierung – die numerischen
  Werte kommen bereits korrekt an und werden auf der Tabellen-Seite (Seite 3)
  korrekt angezeigt.
- Keine Änderung der Werteskalen (`minValue`/`maxValue`, z. B. RPM 0..5000) –
  nur die **Winkel** werden vom Nutzer kalibriert.

## Validierung

- `flutter analyze` (0 Findings) und `flutter test` (alle grün) nach den
  Änderungen.
- Geräte-Test: MIN/MAX für ein Instrument setzen, App per `am force-stop` +
  Neustart beenden/starten, prüfen dass der gespeicherte Winkel weiterhin
  greift (Persistenz-Nachweis über Prozess-Neustart).
- Sichtprüfung durch den Nutzer, dass die Uhr wieder die ursprünglich
  bestätigte Grafik zeigt.

## Risiken / offene Punkte

- Reihenfolge MIN/MAX: Screen muss so gebaut sein, dass das Drücken von nur
  einem der beiden Knöpfe nicht zu einem funktionslosen Zwischenzustand führt
  (z. B. Default-Werte als Platzhalter verwenden, bis beide gesetzt sind).
- `instrument_04` (Tank) hatte ohnehin keine sichtbare eingebackene Nadel –
  hier ist nur die Winkel-Kalibrierung relevant, keine weitere Bildbearbeitung
  nötig.
