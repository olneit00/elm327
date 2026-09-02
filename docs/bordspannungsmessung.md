# Bordspannungsmessung am ESP32 (headunit / motor-node)

Zweck: die Fahrzeug-Bordspannung (bei euch ca. 6 V – 16 V) sicher und einigermassen genau
mit einem ESP32-GPIO-ADC zu messen und auf dem Gauge-Instrument (Issue #24) anzuzeigen.

---

## 1. Die Grundidee: Spannungsteiler + ADC

Ein ESP32-ADC darf maximal **3,3 V** sehen (bei 3,3-V-Betrieb). Die Bordspannung liegt bei
6–16 V, teils mit Spitzen weit darüber. Deshalb wird die Spannung zuerst durch einen
**Spannungsteiler** heruntergeteilt, dann gemessen.

Der Teiler besteht aus zwei Widerständen in Reihe:

```
               R1
Bord+ ──────/\/\/───┬─────────► ADC-Pin (GPIO)
                    │
                    R2
                    │
Bord−/Masse ────────┴───► GND
```

### Teiler-Faktor
Mit R1 = 47 kOhm und R2 = 12 kOhm gilt:

```
Faktor = R2 / (R1 + R2) = 12k / (47k + 12k) = 12/59 ≈ 0,2034

V_adc = Vin · 0,2034

Bord 6 V  → V_adc = 1,22 V
Bord 16 V → V_adc = 3,25 V   (knapp unter 3,3 V)
```

Damit liegt `V_adc` im guten Eingabebereich des ADC (ca. 1,2 V – 3,25 V) und bleibt klar
unter 3,3 V.

### Wichtige Voraussetzung: gemeinsame Masse
Die Messung funktioniert nur, wenn die **Masse des ESP32 und die Masse der Fahrzeugbordspannung
dieselbe Leitung** sind (= Fahrzeugchassis bzw. gemeinsamer GND-Bus).

- In der Headunit wird der ESP32 ohnehin **aus der Bordspannung über einen Buck-Wandler**
  versorgt. In diesem Fall teilen sich ESP32-GND und Fahrzeug-GND dieselbe Leitung – es ist
  **keine galvanische Trennung nötig**, man misst direkt mit dem Spannungsteiler.
- Wenn der ESP32 separat versorgt wird (Labor-Netzteil, USB am Schreibtisch) und **nicht**
  mit dem Fahrzeug-Chassis verbunden ist, gibt es zwei getrennte Massen. Dann ist eine
  Messung so wie oben nicht möglich ohne einen gemeinsamen Bezug. In der Kopf-Einheit kommt
  dieser Fall aber praktisch nicht vor, weil die Module aus demselben Bordnetz gespeist werden.

---

## 2. Empfohlene Schaltung (für Fahrzeugeinsatz)

```
                     Vbat  (Bordspannung, 6..16V, mit Spitzen bis 60V+ im Auto)
                      │
                      ├───[ Schutz-Widerstand Rs 1kOhm ]───┐
                      │                                     │
                     R1 47k                                │
                      │                                     │
                      ├─────────────────────────────────────► ADC-Pin (GPIO)
                      │                                     │          │
                     R2 12k                                │          │
                      │                                     │    [ TVS 3,3V ]
                      │                                     │      (oder Zener)
                     └─────────────────────► GND (Masse)    └──────────┘
```

Bauteile:
- **R1 = 47 kOhm**, **R2 = 12 kOhm** (1 % für Genauigkeit, sorgfältig wählen).
- **Rs = 1 kOhm** in Reihe zum ADC-Pin: schützt den Pin vor Strom und begrenzt Überspannung.
- **TVS-Diode / Zener 3,3 V** vom ADC-Pin gegen GND: klemmt Überspannung (z. B. Lastabwurf)
  auf 3,3 V. Wichtig, weil im Auto Störspitzen bis >40 V auftreten können.
- **Kleiner Glättungskondensator 10 nF** vom ADC-Pin nach GND (nahe am Pin): filtert HF/Störung.
  Optional zusätzlich ein 100-Ohm-Widerstand zwischen Teiler und Kondensator (RC-Tiefpass).

Hinweis Betriebsspannung des Moduls: Die Bordspannung wird **nicht direkt** an den ESP32 gelegt –
der ESP32 (3,3 V) wird über einen **Buck-Regler (z. B. LM2596, MP1584 oder auf der Platine)**
aus der Bordspannung versorgt. Der Teiler misst nur, der Regler versorgt.

---

## 3. ADC-Konfiguration im Code (ESP32 Arduino)

### GPIO wählen
Einen ADC1-Kanal nutzen (ADC2 wird von WiFi anderweitig blockiert). Beispiele:
- GPIO 34 (ADC1_CH6), GPIO 36 (ADC1_CH0), GPIO 39 (ADC1_CH3). Diese drei sind **input-only**,
  eignen sich also gut für analoge Messung.

### In `setup()`:
```cpp
const int kVoltagePin = 34;                 // ADC1_CH6
analogReadResolution(12);                   // 12 Bit (0..4095)
analogSetAttenuation(ADC_ATTEN_11db);       // Eingangsbereich bis ~3,3 V
pinMode(kVoltagePin, INPUT);
```

### Messung und Rückrechnung
```cpp
const float kDividerFactor = 12.0F / (47.0F + 12.0F);  // R2/(R1+R2) ≈ 0,2034

float readBatteryVoltage() {
  int raw = analogRead(kVoltagePin);            // 0..4095
  float vadc = raw * 3.3F / 4095.0F;            // Spannung am ADC-Pin (0..3,3 V)
  float vin  = vadc / kDividerFactor;           // Rückrechnung auf Vin
  return vin;
}
```

### Kalibrieren
Der ESP32-ADC hat eine analytisch nicht perfekte Referenz (typisch ±5–10 %). Für ein
einigermassen genaues Voltmeter einmal mit einem bekannten Wert (Digitalmultimeter) abgleichen
und einen Korrektur-Faktor speichern:

```cpp
// Kalibriert beim Test mit z. B. 14,0 V (Multimeter) vs. gemessen 13,3 V:
const float kCalibGain = 14.0F / 13.3F;   // = 1,053
float readBatteryVoltage() {
  ...
  return vin * kCalibGain;
}
```

Sinnvolle Mittelung: mehrere Samples (`readBatteryVoltage()` x8) mitteln, um das ADC-Rauschen
zu glätten, bevor der Wert dem Gauge übergeben wird.

---

## 4. Anbindung an das Gauge-Instrument

Die Anzeige (Issue #24) bekommt nur noch den fertigen **Spannungswert**:

```cpp
float vbat = readBatteryVoltage();          // z. B. 12,8 V
float angle = GaugeScreen::valueToAngle(vbat,   // Wert
                                        8.0F,   // Skala min (für Voltmeter)
                                       16.0F,   // Skala max
                                        -120.0F, 120.0F);  // Bogenanfang/-ende
gauge.setNeedle(angle);
```

---

## 5. Häufige Fehler / Hinweise

- **Nicht direkt an GPIO messen ohne Teiler.** Ein nicht-übersteuerter ESP32-Pin wäre bei
  12 V tot. Immer teilen + TVS.
- **Masse verbinden.** Wenn beim Verdrahten die Fahrzeugmasse und die ESP32-Masse nicht
  dieselbe Leitung sind, zeigt das Voltmeter falsche/indefinite Werte. In der Headunit sind
  sie normalerweise verbunden (Buck aus Bordnetz).
- **ADC2 meiden.** ADC2-Pins werden während WiFi-Betrieb blockiert und liefern dann 0.
- **Analysis-Bereich (Attenuation) setzen.** Ohne 11-dB-Attenuation misst der ADC nur bis
  ~1,1 V – die Werte wären hart gesättigt/ungenau.
- **Störspitzen im Auto.** Der 3,3-V-Schutz (TVS/Zener) + 10-nF-Kondensator sind Pflicht,
  nicht optional.

---

## 6. Option: Wenn wirklich galvanische Trennung nötig wäre

Nur falls der ESP32 in einer selteneren Konfiguration eine **von der Fahrzeugmasse vollständig
getrennte** Versorgung hätte und trotzdem die Bordspannung messen soll:

- **Isolierter Messverstärker** / Isolation-ADC (z. B. mit interner Referenz und getrenntem
  Versorgungs-DC/DC für die Messseite), oder
- **Linear-Isolator** (z. B. HCNR201) analog / digital.
- Kosten + Aufwand steigen deutlich, Genauigkeit anscheinend geringer als direkte Messung.

In der **Headunit brauchst du das nicht**, weil gemeinsame Masse gegeben ist. Der Aufwand ist
nur relevant, wenn in einer späteren Einbaulage wirklich zwei getrennte Netze existieren.