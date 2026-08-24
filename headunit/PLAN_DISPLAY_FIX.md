# Plan: Anzeige-Fix Ziffernblatt (GC9A01 240x240)

Status: Bildseite ist bereits umgesetzt (Asset neu generiert). Die folgenden
Code-Änderungen sind bewusst NICHT implementiert, weil gerade ein anderer
Agent im Code arbeitet. Umsetzung nach Absprache, Reihenfolge wie nummeriert.

## Befunde (recherchiert, 24.08.2026)

1. Blaustich + Farbverlaeufe — Byte-Reihenfolge-Mismatch im Flush-Pfad
   - `src/display/DisplayManager.cpp:80` setzt `LV_COLOR_FORMAT_RGB565`
     (little-endian, unvertauscht).
   - `FlushCb` (DisplayManager.cpp:31) schiebt den Buffer mit
     `lcd.pushImage(...)` roh raus. LovyanGFX behandelt `uint16_t*`-Buffer bei
     `_swapBytes == false` (Default, LGFXBase.hpp:1029) als bereits
     byte-vertauscht (`swap565_t`) und sendet die Bytes unverändert.
   - Folge: jedes Pixel wird auf dem Panel byteweise vertauscht.
     Altes Beige (237,197,145) -> Blaugrau (49,93,115), Randton (231,200,152)
     -> Dunkelgruen (49,77,49). Deshalb "blau-ton mit Farbuergaengen".
   - Referenz: LVGLs eigener LovyanGFX-Treiber
     (`.pio/.../lvgl/src/drivers/display/lovyan_gfx/lv_lovyan_gfx.cpp`)
     setzt genau deshalb `LV_COLOR_FORMAT_RGB565_SWAPPED`.

2. Rahmen im Bild / Ziffernblatt zu klein
   - Das alte Asset enthielt den dunklen Metallring; das Ziffernblatt fuellte
     nur ~85 % der 240x240-Flaeche (Dial-Radius ~102 px statt 120 px).
   - Behoben durch Neugenerierung (siehe unten), kein Code-Eingriff noetig.

3. Zahlen/Ticks doppelt und falsch skaliert
   - `ClockScreen::renderFace()` (src/ui/ClockScreen.cpp:108) zeichnet die 12
     programmatischen Ticks (Radius 96-106) UNBEDINGT, obwohl der Kommentar
     "only when the bitmap is unavailable" verspricht. Sie lagen ueber den
     Bitmap-Zahlen, deren Ring im alten Asset nur ~78 px Radius hatte.

## Bildseite — BEREITS UMGESETZT

- `assets/clock/generate_clock_face.py` (neu): schneidet aus
  `..\..\..\uhr_optimiert.png` (1254x1254) den Dial-Kreis (Zentrum 626.7/599.6,
  Radius 502) OHNE Rahmen, flattet die Farben auf zwei Toene (Creme 233,202,153
  einheitlich + reines Schwarz, kein Foto-Verlauf mehr), behaelt den kleinen
  Messing-Hub (r<40) und skaliert vollflaechig auf 240x240 mit runder
  Alpha-Maske.
- `include/assets/clock_face.c` neu generiert: identisches Format wie vorher
  (RGB565A8, 240x240, 172800 Bytes, gleicher Descriptor) -> keine Codeanpassung
  fuer den Tausch noetig.
- `assets/clock/uhr_ziffernblatt_240.png`: Quell-PNG des Assets (Referenz).
- Regenerieren: `python assets/clock/generate_clock_face.py`

Wichtig: Ohne Code-Fix 1 bleibt die Anzeige verfaerbt (neues Asset waere dann
gleichmaessig olivgruen statt blaugrau-gemustert). Erst Fix 1 liefert Beige.

## Code-Aenderungen (PLAN, noch nicht implementiert)

### 1. Byte-Reihenfolge im Flush korrigieren (Pflicht, behebt Blaustich)

Variante B (empfohlen, 1 Zeile, keine lv_conf-Aenderung):
- `src/display/DisplayManager.cpp`, in `begin()` nach `lcd.setColorDepth(16)`:
  `lcd.setSwapBytes(true);`
- `pushImage` vertauscht dann je Pixel high/low byte; LVGL-Buffer bleibt
  `LV_COLOR_FORMAT_RGB565`.

Variante A (Alternative, entspricht LVGL-Builtin-Treiber):
- `lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565_SWAPPED);`
- Dazu zwingend `include/lv_conf.h`:
  `LV_DRAW_SW_SUPPORT_RGB565_SWAPPED` 0 -> 1 (aktuell explizit deaktiviert).

### 2. Doppelte Ticks abschalten (Pflicht, behebt "Zahlen passen nicht")

- `src/ui/ClockScreen.cpp`: `renderFace()` nur aufrufen, wenn KEIN Bitmap
  gezeigt wird (z. B. Flag/Parameter, Kommentar im Code bereits so intendiert,
  ClockScreen.cpp:48-50). Das neue Bitmap enthaelt Zahlen, Minuten- und
  Stundenticks in korrektem Massstab.

### 3. Zeigerlaengen an die volle Skala anpassen (optional, Feinschliff)

- Neue Geometrie im Bitmap: Minutentrack bis r~114, Zahlenring r~90-105.
- Aktuell: kSecondLen 100 (passt), kMinuteLen 82 und kHourLen 58 wirken jetzt
  kurz. Vorschlag: Minute ~95, Stunde ~62, Sekunde ~108; Hub (12 px, hell)
  bleibt, darunter schaut der Messing-Hub des Bildes ~4 px hervor.

### 4. Race Condition Web-Handler <-> RadioService (offen, mittel)

- Code-Analyse: AsyncWebServer-Handler (async_tcp-Task) schreiben direkt in
  RadioService-Member (_status, _config), waehrend loop() (Main-Task) dieselben
  Felder liest/schreibt (z. B. setFrequency/setVolume/setMuted vs.
  _updateStatusFromHardware/_pollRds).
- Symptome moeglich: zerrissene uint16-Werte, inkonsistente Anzeigen,
  Doppelzugriffe auf den SI470X. Erklaert NICHT die ausgebliebenen
  Web-Frontend-Reaktionen (das war Cache/JS-Ausfuehrung + SSE).
- Fix-Idee: kurze Critical Section (portMUX) oder FreeRTOS-Mutex in
  RadioService um Status/Konfig-Zugriffe; Web-Handler arbeiten nur auf Kopien.

### 5. Verifikation nach Umsetzung

- `pio run -e esp32dev` (bzw. aktuelles Env) + Flash.
- Boot: LVGL-Farbtest (DisplayManager.cpp:88-96) muss jetzt echt rot/gruen/blau
  zeigen (vorher durch den Swap verfaerbt) -> schnelle Gegenprobe fuer Fix 1.
- Ziffernblatt: vollflaechig bis zum Displayrand, einheitlich Creme ohne
  Verlauf, keine programmatischen Tick-Striche ueber den Zahlen, Zeiger
  ueberdecken die Skala korrekt.
