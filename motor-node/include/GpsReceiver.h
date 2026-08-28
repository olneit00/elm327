#pragma once
//
// GpsReceiver — reads a u-blox NEO-8M GNSS module over UART2 (NMEA 0183)
// and exposes "everything the chip offers" as a typed struct the web UI
// (and later the CAN/bus layer) can consume.
//
// Why a custom parser instead of TinyGPSPlus:
//   TinyGPSPlus only parses $GGA and $RMC; it does NOT expose the per-satellite
//   GSV data (azimuth/elevation/SNR), PDOP/VDOP from $GSA, geoid separation,
//   $GPGLL, or the extra sentences. The requirement here is to surface
//   *everything* the NEO-8M emits, so we parse the full common NMEA 0183 set
//   ourselves: GGA, RMC, GSA, GSV, VTG, GLL, ZDA.
//
// Thread-safety: parse() runs on the Arduino loop task. Web reads take a
// short mutex on the async tcp task. Every field write and read is guarded by
// the same mutex; GpsSnapshot is a value copy returned to callers.
//
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

// Number of satellites tracked per GSV page / max we surface.
constexpr uint8_t kGpsMaxSatellites = 32;

// "Everything the chip offers" — a flattened, web/CAN-friendly snapshot.
// Lat/lon in degrees (decimal). Altitude MSL in meters. Speed in km/h,
// course in degrees (0..360). Time fields are UTC.
struct GpsSnapshot {
  bool valid = false;            // have we ever got a complete, checksummed sentence set
  bool activeFix = false;        // RMC status A (valid position fix)
  uint8_t fixQuality = 0;        // GGA fix quality: 0=no,1=GPS,2=DGPS,...
  uint8_t fixMode = 0;           // GSA mode: 1=no fix,2=2D,3=3D
  uint8_t satellitesInUse = 0;   // GGA/GSA: sats used in fix
  uint8_t satellitesInView = 0;  // GSV: total sats in view

  double latitude = 0.0;         // deg, +N
  double longitude = 0.0;        // deg, +E
  float altitudeMsl = 0.0f;      // meters (MSL)
  float geoidSeparation = 0.0f;  // meters
  float speedKmh = 0.0f;         // ground speed
  float courseDeg = 0.0f;        // course over ground, 0..360

  // Dilution of precision (from GGA/GSA)
  float hdop = 0.0f;
  float vdop = 0.0f;
  float pdop = 0.0f;

  // UTC date & time.
  uint8_t utcHour = 0, utcMin = 0, utcSec = 0;
  uint8_t utcDay = 0, utcMonth = 0;
  uint16_t utcYear = 0;

  // GPS-synced time valid?
  bool dateValid = false;

  // Per-satellite info from GSV (in view).
  struct Satellite {
    uint8_t prn = 0;          // satellite PRN
    uint8_t elevation = 0;    // deg
    uint16_t azimuth = 0;     // deg
    uint8_t snr = 0;          // dB-Hz (0 = not acquired)
    bool active = false;
  };
  Satellite satellites[kGpsMaxSatellites];

  // Diagnostics/model info
  uint32_t lastLineMs = 0;     // millis() of last valid NMEA sentence parsed
  uint32_t sentencesParsed = 0; // monotonic counter
  uint32_t checksumErrors = 0;
};

class GpsReceiver {
 public:
  GpsReceiver(uint8_t rxPin, uint8_t txPin, uint32_t baud = 9600);
  ~GpsReceiver();

  void begin();
  void update();                // read UART2, feed parser (call from loop())

  GpsSnapshot snapshot() const; // thread-safe copy

  // Force the serial baud / pins if the module was reconfigured.
  void setSerial(uint8_t rxPin, uint8_t txPin, uint32_t baud);

 private:
  void processByte(uint8_t c);
  void resetLine();

  // NMEA sentence parsers (run with mutex held, on loop task).
  void parseLine();
  void parseGga(char** f, int n);
  void parseRmc(char** f, int n);
  void parseGsa(char** f, int n);
  void parseGsv(char** f, int n);
  void parseVtg(char** f, int n);
  void parseGll(char** f, int n);
  void parseZda(char** f, int n);

  uint8_t rxPin_, txPin_;
  uint32_t baud_;

  // receive/parse line state (loop task only, no lock needed)
  char line_[128];
  uint8_t lineLen_ = 0;
  bool inSentence_ = false;

  GpsSnapshot snap_;            // guarded by mutex_
  mutable SemaphoreHandle_t mutex_ = nullptr;
};