# Plan: Classic 1930s Radio Frontend (`/classic-radio`) — Issue #8

Analyse von https://github.com/olneit00/elm327/issues/8 und Umsetzungsplan.
Wird schrittweise abgearbeitet; Haken werden gesetzt, sobald ein Schritt
gemerged ist.

## Ziel (Rekap)

Zusätzliches, optionales Frontend im 1930er-Jahre-Radio-Look unter
`/classic-radio`, das **dasselbe Backend/dieselben REST-/SSE-Endpunkte**
nutzt wie das bestehende `/radio`. `/radio` bleibt unverändert.

## Bestandsaufnahme Backend (verifiziert im Code)

Alle vom Issue referenzierten Endpunkte existieren bereits in
`WebServer.cpp`:

- `GET /api/radio/status`, `GET /api/radio/events` (SSE: `status`,
  `stations`, `scan`)
- `POST /api/radio/tune`, `/frequency`, `/nudge`, `/seek`
- `POST /api/radio/volume`, `/mute`, `/power`
- `GET /api/radio/stations`, `POST /api/radio/favorite`
- `POST /api/radio/scan/start`, `/scan/cancel`, `GET /scan/progress`
- `GET`/`POST /api/radio/config`
- `POST /api/wifi/connect`, `GET /api/wifi/status`

Fehlend (vom Issue selbst schon identifiziert):

- `GET /api/time` (+ optional SSE `time`) — für den Uhr-Screen mit
  Zeitquellenanzeige (GPS/RDS/NTP/manuell)
- Presets-API für 6 feste Stationstasten (V1 kann ersatzweise die ersten
  6 Favoriten nehmen)
- Settings-Erweiterungen: Displayhelligkeit, RDS-Auto-Scroll,
  Nachtmodus, GALA-Konfiguration, bevorzugte Zeitquelle

**Wichtiger Zusatzbefund (nicht im Issue erwähnt):** Es gibt aktuell
*keine* echte Zeit-/Datumsquelle im System — nur `MillisClock`
(`include/time/TimeProvider.h`), eine virtuelle Uhr, die per
`RadioService::setTimeCallback()` von der RDS-Uhrzeit (Gruppe 4A)
nachgezogen wird. Es wird **kein Datum** getrackt (nur `TimeOfDay` =
hh:mm:ss), und `SI470X::getRdsTime()` ist laut Kommentar in der
Bibliothek selbst "Under Test and construction". `GET /api/time` mit
`year/month/day` kann also in V1 nicht zuverlässig befüllt werden, ohne
vorher eine echte Datumsverfolgung einzuführen. GPS ist laut Issue
"sobald motor-node/GPS verfügbar" — das existiert im Repo aktuell nicht.

## Phasenplan

### Phase 0 — Branch & Grundgerüst (dieser Schritt)
- [x] Branch `issue-8-classic-radio-frontend` angelegt
- [x] Verzeichnis `headunit/data/classic-radio/` mit `index.html`,
      `classic-radio.css`, `classic-radio.js`
- [x] Neue Route in `WebServer::_setupStaticFiles()`:
      `serveStatic("/classic-radio", LittleFS, "/classic-radio/index.html")`
      + je eine `serveStatic()`-Route für `classic-radio.css`/`classic-radio.js`.
      **Entscheidung:** anders als bei `/radio` wird für das neue
      Frontend **keine** eingebettete C++-Fallback-Kopie gepflegt (der
      Doppelpflegeaufwand aus S7/REFACTORING_PROGRESS.md wäre für ein
      komplett neues, deutlich größeres UI unverhältnismäßig). Fällt
      LittleFS aus, liefert `/classic-radio` einfach 404 — `/radio`
      bleibt über seinen bestehenden Fallback erreichbar.
- [x] Kurzer Eintrag in `README.md`, dass `/classic-radio` existiert
      (Abschnitt war als "Geplant" bereits vorbereitet, jetzt aktualisiert).
- [x] Folge-Issue für `GET /api/time` angelegt: [#9](https://github.com/olneit00/elm327/issues/9)

### Phase 1 — V1 mit ausschließlich bestehender API (Kernumfang)
Kein Backend-Change nötig, nur Frontend.

- [x] **Radio-Hauptansicht**: Skala 87.5–108 MHz als horizontaler
      Balken/SVG mit Zeiger, Frequenz aus `status.frequency`/SSE
      `status`; PS/RT dezent; RSSI-Instrument aus `status.rssi`;
      Stereo-/RDS-Kontrollleuchten aus `status.stereo`/`status.rds.synced`;
      Lautstärke-Drehregler → `POST /volume`; Feinabstimmung → `POST
      /nudge`; 6 Stationstasten aus den ersten 6 `favorite`-Einträgen
      der Senderliste (`GET /stations`).
- [x] **Stationen/Favoriten**: Liste aus `GET /stations`, Tap → `POST
      /frequency`, Stern toggelt `POST /favorite`.
- [x] **Sendersuchlauf-Dialog**: `POST /scan/start`, Fortschritt per SSE
      `scan`, `POST /scan/cancel`, danach Wechsel zur Stationsliste.
- [x] **RDS-Detailansicht**: reine Darstellung aus `status`/SSE
      (`programService`, `radioText`, `rds.piCode`, `rds.pty`/`ptyName`,
      `rds.tp`, `rds.ta`, `rds.synced`, `rds.bler.{a,b,c,d}`, `rssi`,
      `stereo`, `frequency`) — alle Felder existieren bereits aus Issue
      #3.
- [x] **Einstellungen (Teilmenge)**: Lautstärke, Mute, WLAN
      SSID/Passwort — über bestehende Endpoints, optisch ans
      Retro-Design angepasst.
- [x] **Navigation**: feste Bottom-Nav (Radio/Stationen/RDS/Uhr/
      Einstellungen), mobile-first, große Touch-Flächen, kein Scrollen
      auf der Hauptansicht im Smartphone-Format nötig.
- [x] **Uhr-Screen V1 (ohne neuen Endpoint)**: analoge Uhr rein
      clientseitig aus der Browser-Systemzeit gerendert (Zeitquelle
      als `"lokal (Browser)"` gekennzeichnet), bis Issue #9 (Phase 2)
      landet.

### Phase 2 — `GET /api/time` + echte Zeitquelle
- [ ] `TimeOfDay`/`ITimeSource` um Datum erweitern oder separate
      `DateTime`-Struktur einführen.
- [ ] Prioritätslogik GPS (noch nicht vorhanden) → RDS → NTP (noch
      nicht vorhanden) → manuell/intern, mit `source`-Feld.
- [ ] Neuer Endpoint `GET /api/time` (Grundgerüst kann bereits ohne
      GPS/NTP ausgeliefert werden: `source` ist dann immer `"rds"` oder
      `"internal"`).
- [ ] Optional SSE-Event `time`.
- [ ] Uhr-Screen im Classic-Frontend auf den echten Endpoint umstellen.
- [ ] Optional `POST /api/time` für manuelle Einstellung.

### Phase 3 — Presets-API (optional, spätere Ausbaustufe)
- [ ] `GET /api/radio/presets`, `POST /api/radio/presets/{slot}`,
      optional `DELETE`. Slots 1–6 mit fester Reihenfolge statt
      "erste 6 Favoriten".

### Phase 4 — Settings-Erweiterungen (optional, spätere Ausbaustufe)
- [ ] Displayhelligkeit, RDS-Auto-Scroll, Nachtmodus, GALA-Konfiguration,
      bevorzugte Zeitquelle — Erweiterung von `RadioConfig`/
      `/api/radio/config` oder neuer System-Config-Endpoint.

## Gestaltungsansatz (technisch)

- Reines HTML/CSS/JS ohne Build-Step (Konsistenz mit `/radio`), CSS
  Custom Properties für die Retro-Palette (dunkles Holz/Bakelit,
  Messing-Akzente, cremefarbene/bernsteinfarbene Skala).
- Skala/Zeiger: `<canvas>` oder positioniertes `<div>` mit `transform:
  translateX(...)`, berechnet aus `frequency` linear zwischen 87.5 und
  108 MHz — kein SVG-Asset-Pflichtbedarf für V1.
- Drehknöpfe: großer Tap-Bereich mit `+`/`-`-Zonen (kein reines Drag,
  siehe Vorgabe "Mobile First" im Issue) plus optional Pointer-Drag als
  Zusatzbedienung.
- Analoge Uhr: CSS-rotierte Zeiger (`transform: rotate()`), pro Sekunde
  aktualisiert.

## Offene Risiken / Rückfragen

1. Datum/GPS/NTP existieren nicht — Phase 2 braucht ggf. eigene
   Rückfrage/Priorisierung, bevor sie umgesetzt wird.
2. LittleFS-Speicherbudget für zusätzliche Assets (Bilder/Fonts) ist
   nicht geprüft — Plan sieht bewusst CSS/SVG-basiertes Design statt
   Bitmap-Assets vor, um das gering zu halten.
3. Kein lokaler Compiler/PlatformIO-Build in dieser Sandbox möglich —
   Verifikation bleibt manuell/statisch (wie bei allen bisherigen
   Änderungen in diesem Repo).

## Umsetzungsreihenfolge in dieser Session

Phase 0 (dieser Commit) → Phase 1 (nächster Schritt, kompletter
Kernumfang ohne Backend-Änderungen) → Rückmeldung/Test durch den
Nutzer, bevor Phase 2–4 (neue Endpoints) angegangen werden.
