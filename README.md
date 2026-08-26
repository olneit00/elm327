# elm327 – ESP32 Headunit & Motor-Node für Opel 1935

ESP32-basiertes Elektronikprojekt für einen Opel von 1935. Ziel ist, moderne Sensorik, CAN-Kommunikation, GPS, Radio/RDS und OBD2-/ELM327-Kompatibilität möglichst unauffällig in das historische Fahrzeug zu integrieren.

> **Leitidee:** Technik modern, Darstellung 1935.

Die Elektronik darf im Hintergrund modern arbeiten. Die sichtbaren Bedienelemente und Anzeigen sollen sich dagegen möglichst harmonisch in das historische Fahrzeug einfügen.

## Architektur

Das Projekt besteht im Wesentlichen aus zwei ESP32-Knoten:

### `motor-node`

Knoten im Motorraum für Fahrzeug- und Sensordaten.

Aufgaben bzw. geplante Aufgaben:

- Erfassen von Motordrehzahl
- Kühlmitteltemperatur
- Bordspannung des 6-V-Bordnetzes
- Zündzeitpunkt
- Drosselklappen-/Gasstellung
- Tankgeber bzw. Tankfüllstand
- GPS
- Bereitstellung von Fahrzeugdaten über CAN
- ELM327-/OBD2-Emulation für ausgewählte Fahrzeugwerte

### `headunit`

Knoten im Armaturenbrett für Anzeige, Radio und Bedienung.

Funktionen:

- FM-Radio
- Si4703-Tuner
- RDS
- Sendersuchlauf
- Stationsliste
- Favoriten
- Webfrontend
- WiFi
- rundes Display / klassische Instrumentendarstellung
- zukünftig Empfang und Darstellung der CAN-Fahrzeugdaten

## Radio & RDS

Die Headunit bietet unter anderem:

- Frequenzwahl
- Feinabstimmung
- automatischen Sendersuchlauf
- Stationsliste
- Favoriten
- Lautstärke
- Mute
- Power
- RDS Program Service (PS)
- RDS RadioText (RT)
- weitere RDS-Diagnosedaten

Die Radiofunktionen stehen zusätzlich über eine REST-API und Server-Sent Events zur Verfügung.

## Weboberflächen

### Technisches Radio-Frontend

```text
/radio
```

Aktuelle funktionale Oberfläche für Radio, Sendersuche, Stationen und RDS.

### Klassisches Radio-Frontend

```text
/classic-radio
```

Mobile-first-Oberfläche im Stil eines Radios der 1930er Jahre mit klassischer Senderskala, mechanisch wirkenden Stationstasten, analoger Uhr und dezent integrierten RDS-Informationen. Nutzt dasselbe Backend/dieselben REST-/SSE-Endpunkte wie `/radio`.

Das bestehende `/radio` bleibt dabei als technische Oberfläche erhalten.

Aktueller Stand (siehe `headunit/docs/issue-8-classic-radio-frontend-plan.md`): Radio-Hauptansicht, Stationen/Favoriten, Sendersuchlauf, RDS-Detail, Uhr (vorläufig mit Browserzeit) und ein Teil der Einstellungen sind umgesetzt. Ein eigener `GET /api/time`-Endpoint für eine echte Server-Zeitquelle ist als Folge-Issue geplant.

### Diagnose

Als spätere Erweiterung ist eine technische Diagnoseoberfläche vorgesehen:

```text
/diagnostics
```

für CAN-Rohdaten, Sensorwerte, GPS, RDS, WiFi, Speicher, Uptime und weitere Debug-Informationen.

## REST-API

Bereits vorhandene Radio-Endpunkte umfassen unter anderem:

```text
GET  /api/radio/status
GET  /api/radio/stations
GET  /api/radio/scan/progress
POST /api/radio/tune
POST /api/radio/frequency
POST /api/radio/nudge
POST /api/radio/seek
POST /api/radio/scan/start
POST /api/radio/scan/cancel
POST /api/radio/volume
POST /api/radio/mute
POST /api/radio/power
POST /api/radio/favorite
GET  /api/radio/config
POST /api/radio/config
GET  /api/wifi/status
POST /api/wifi/connect
```

Live-Updates erfolgen über Server-Sent Events:

```text
/api/radio/events
```

## Klassische Fahrzeuginstrumente

CAN-Daten sollen auf dem runden Display nicht wie ein modernes OBD-Dashboard aussehen, sondern als historische analoge Instrumente dargestellt werden.

Vorgesehen sind beispielsweise:

- Kühlwassertemperatur
- Drehzahl
- Geschwindigkeit
- **Voltmeter für das 6-V-Bordnetz (ca. 0–8 V)**
- Zündzeitpunkt
- Tankinhalt
- GPS-Kompass

Das Display kann später abhängig vom Fahrzeugzustand automatisch zwischen Instrumenten wechseln.

## Weitere Ideen

Geplante bzw. untersuchte Erweiterungen sind unter anderem:

- GPS-Zeit und GPS-Geschwindigkeit
- digitaler Tageskilometerzähler
- Fahrtenrekorder / Blackbox
- Startprotokoll
- Batterie- und Ladesystemdiagnose
- Temperatur-Trendüberwachung
- Service-/Wartungslog
- Retro-Navigation
- ortsabhängige Radiofavoriten
- Sensor-Recorder mit CSV-Export
- zusätzliche virtuelle OBD2-PIDs

Die ausführlichere Ideensammlung befindet sich in der separaten Knowledge Base.

## Knowledge Base

Hardwaredaten, verwendete Libraries, Protokolle, verifizierte Quellen und Projektideen werden separat gepflegt:

https://github.com/olneit00/elm327-knowledge-base

Dort befinden sich unter anderem Informationen zu:

- ESP32
- GC9A01 Runddisplay
- LVGL
- Si4703
- RDS
- NimBLE
- ESPAsyncWebServer
- zukünftigen Hardware-/Software-Erweiterungen

Die Knowledge Base dient ausdrücklich auch als verifizierter Kontext für Coding- und Debug-Agenten.

## Projektstruktur

```text
elm327/
├── headunit/       # ESP32 Headunit, Radio, Display, Web
├── motor-node/     # ESP32 Motorraum-Knoten / Fahrzeugdaten
└── README.md
```

Beide Teilprojekte werden unabhängig entwickelt und geflasht, sollen aber über CAN zusammenarbeiten.

## Entwicklungsumgebung

Das Projekt verwendet PlatformIO / ESP32 und wird primär in Visual Studio Code entwickelt.

Verwendete bzw. vorgesehene Technologien umfassen:

- ESP32
- PlatformIO
- C/C++ / Arduino Framework
- CAN
- WiFi
- Bluetooth LE / NimBLE
- ESPAsyncWebServer
- LittleFS
- LVGL
- GC9A01
- Si4703
- RDS

## Status

Das Projekt befindet sich in aktiver Entwicklung. Radio, RDS, Webfrontend und ELM327-Grundfunktionen sind bereits vorhanden; CAN-Fahrzeugdaten, GPS und die klassische Instrumenten-/Radiooberfläche werden schrittweise ergänzt.
