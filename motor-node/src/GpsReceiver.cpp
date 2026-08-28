#include "GpsReceiver.h"
#include "log/LogTail.h"
#include <string.h>
#include <stdlib.h>

// Maximum NMEA fields a sentence triggers in our parsers + margin.
constexpr int kMaxFields = 24;

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// UBX-CFG-MSG (class 0x06, id 0x01), legacy 3-byte payload (msgClass, msgId,
// rate): enables an NMEA sentence at rate 1 on *all* ports. The M8-family
// receivers (incl. the "NEO-8M" clones that actually contain an M8N and emit
// $GN talker sentences) accept this short form; the 7-byte per-port variant
// leaves the UART disabled on many clones, which is why only $GNGLL (emitted
// by default) ever appeared. Session-only: no CFG-CFG save, no reset, safe to
// resend on every boot. Fletcher-checksums verified against
// motor-node/docs/ubx_cmds.py (see issue #20 "NEO-8M/u-blox-Konfiguration").
constexpr uint8_t kUbxEnableGga[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x00,
                                      0x01, 0xFB, 0x10};
constexpr uint8_t kUbxEnableGsa[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x02,
                                      0x01, 0xFD, 0x14};
constexpr uint8_t kUbxEnableGsv[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x03,
                                      0x01, 0xFE, 0x16};
constexpr uint8_t kUbxEnableRmc[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x04,
                                      0x01, 0xFF, 0x18};
constexpr uint8_t kUbxEnableVtg[] = {0xB5, 0x62, 0x06, 0x01, 0x03, 0x00, 0xF0, 0x05,
                                      0x01, 0x00, 0x1A};

struct UbxCfgMsg {
  const uint8_t* bytes;
  size_t len;
  const char* name;
};
const UbxCfgMsg kUbxEnableMsgs[] = {
    {kUbxEnableGga, sizeof(kUbxEnableGga), "GGA"},
    {kUbxEnableGsa, sizeof(kUbxEnableGsa), "GSA"},
    {kUbxEnableGsv, sizeof(kUbxEnableGsv), "GSV"},
    {kUbxEnableRmc, sizeof(kUbxEnableRmc), "RMC"},
    {kUbxEnableVtg, sizeof(kUbxEnableVtg), "VTG"},
};

}  // namespace

GpsReceiver::GpsReceiver(uint8_t rxPin, uint8_t txPin, uint32_t baud)
    : rxPin_(rxPin), txPin_(txPin), baud_(baud) {
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_) xSemaphoreGive(mutex_);
}

GpsReceiver::~GpsReceiver() {
  if (mutex_) vSemaphoreDelete(mutex_);
}

void GpsReceiver::setSerial(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
  rxPin_ = rxPin; txPin_ = txPin; baud_ = baud;
}

void GpsReceiver::begin() {
  Serial2.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
  LOG.printf("[GPS] UART2 begin rx=%u tx=%u baud=%lu\n", rxPin_, txPin_, baud_);
  configureModule();
}

void GpsReceiver::configureModule() {
  // Deterministically (re-)enable the NMEA sentences we depend on instead of
  // trusting an unknown factory/previous configuration (see issue #20,
  // "GSV/GSA/GGA/RMC sicher aktivieren"). Session-only: no CFG-CFG save, no
  // baud/GNSS change, so this can't leave the module worse off than before.
  for (const auto& m : kUbxEnableMsgs) {
    Serial2.write(m.bytes, m.len);
    LOG.printf("[GPS] UBX CFG-MSG: enable %s on UART\n", m.name);
    delay(50);  // let the module ack/apply before the next command
  }
}

GpsSnapshot GpsReceiver::snapshot() const {
  GpsSnapshot out;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    out = snap_;
    xSemaphoreGive(mutex_);
  }
  out.bytesReceived = bytesReceived_;
  out.lastByteMs = lastByteMs_;
  return out;
}

void GpsReceiver::update() {
  while (Serial2.available()) {
    bytesReceived_ = bytesReceived_ + 1;
    lastByteMs_ = millis();
    processByte((uint8_t)Serial2.read());
  }

  // Periodic readable telemetry so we can tell from the serial log alone
  // whether the GPS module is wired, powered, and locked.
  uint32_t now = millis();
  if (now - lastStatusMs_ >= 5000) {
    lastStatusMs_ = now;
    logStatus(snapshot());
  }
}

void GpsReceiver::logStatus(const GpsSnapshot& s) {
  uint32_t now = millis();
  uint32_t age = (s.lastLineMs && s.lastLineMs <= now) ? (now - s.lastLineMs) / 1000 : 0xFFFFFFFF;

  LOG.printf("[GPS] bytes=%lu lines=%lu parsed=%lu csumErr=%lu",
                (unsigned long)s.bytesReceived, (unsigned long)s.linesReceived,
                (unsigned long)s.sentencesParsed, (unsigned long)s.checksumErrors);

  if (s.activeFix) {
    LOG.printf(" | FIX %u/%u sats, lat=%.6f lon=%.6f, %.1f km/h krs=%.0f",
                  s.satellitesInUse, s.satellitesInView, s.latitude, s.longitude,
                  s.speedKmh, s.courseDeg);
  } else {
    LOG.printf(" | KEIN Fix (inView=%u qual=%u mode=%u)",
                  s.satellitesInView, s.fixQuality, s.fixMode);
  }

  if (age == 0xFFFFFFFF) LOG.printf(" | letzterDatensatz=nie");
  else LOG.printf(" | letzterDatensatz=%lus zurueck", (unsigned long)age);

  if (s.bytesReceived == 0)
    LOG.printf(" | WARNUNG: keine UART-Daten - GPS an GPIO16/17 (9600) angeschlossen?");
  else if (s.lastLineMs == 0)
    LOG.printf(" | rohe Bytes, aber noch kein kompletter NMEA-Satz gepart");

  LOG.println();
}

void GpsReceiver::resetLine() {
  lineLen_ = 0;
  line_[0] = '\0';
  inSentence_ = false;
}

// Coordinator: converts a NMEA "ddmm.mmmm" (or "dddmm.mmmm") decimal-minutes
// value + hemisphere char into signed decimal degrees.
static double nmeaToDegrees(const char* val, char hem) {
  if (!val || !*val) return 0.0;
  const char* dot = strchr(val, '.');
  int beforeDot = dot ? (int)(dot - val) : (int)strlen(val);
  int degDigits = (beforeDot >= 4) ? beforeDot - 2 : 2;  // lat 2, lon 3
  const char* r = val;
  double dd = 0.0;
  for (int i = 0; i < degDigits && isDigit(*r); i++) { dd = dd * 10 + (*r - '0'); r++; }
  double mm = 0.0;
  while (isDigit(*r)) { mm = mm * 10 + (*r - '0'); r++; }
  if (*r == '.') {
    r++;
    double scale = 0.1;
    while (isDigit(*r)) { mm += (double)(*r - '0') * scale; scale *= 0.1; r++; }
  }
  double valDeg = dd + mm / 60.0;
  if (hem == 'S' || hem == 'W') valDeg = -valDeg;
  return valDeg;
}

static void parseHhmmss(const char* f, uint8_t& h, uint8_t& m, uint8_t& s) {
  h = m = s = 0;
  if (!f || strlen(f) < 6) return;
  h = (uint8_t)((f[0]-'0')*10 + (f[1]-'0'));
  m = (uint8_t)((f[2]-'0')*10 + (f[3]-'0'));
  s = (uint8_t)((f[4]-'0')*10 + (f[5]-'0'));
}

static void parseDdmmyy(const char* f, uint8_t& d, uint8_t& mo, uint16_t& y) {
  d = mo = 0; y = 0;
  if (!f || strlen(f) < 6) return;
  d  = (uint8_t)((f[0]-'0')*10 + (f[1]-'0'));
  mo = (uint8_t)((f[2]-'0')*10 + (f[3]-'0'));
  y  = (uint16_t)(2000 + ((f[4]-'0')*10 + (f[5]-'0')));
}

// Tokenize line_ into fields[]. Commas become NULs. Returns field count.
// Mutates line_, so run on the loop task with the mutex held.
static int splitFields(char* line, char** fields, int maxFields) {
  int n = 0;
  char* p = line;
  while (*p && n < maxFields) {
    if (*p == '$') { p++; continue; }        // skip leading $
    if (*p == '*') { break; }                 // checksum suffix
    fields[n++] = p;
    while (*p && *p != ',' && *p != '*') p++;
    if (*p == ',') { *p = '\0'; p++; }
  }
  return n;
}

void GpsReceiver::parseLine() {
  // Sentence type word is chars [3..5] of "GPGGA..." (after "GP").
  char type[4] = {0};
  if (strlen(line_) >= 6) { memcpy(type, line_ + 3, 3); type[3] = '\0'; }

  char* fields[kMaxFields] = {nullptr};
  int n = splitFields(line_, fields, kMaxFields);

  if (strcmp(type, "GGA") == 0 && n >= 12) parseGga(fields, n);
  else if (strcmp(type, "RMC") == 0 && n >= 10) parseRmc(fields, n);
  else if (strcmp(type, "GSA") == 0 && n >= 3) parseGsa(fields, n);
  else if (strcmp(type, "GSV") == 0) parseGsv(fields, n);
  else if (strcmp(type, "VTG") == 0 && n >= 2) parseVtg(fields, n);
  else if (strcmp(type, "GLL") == 0 && n >= 6) parseGll(fields, n);
  else if (strcmp(type, "ZDA") == 0 && n >= 5) parseZda(fields, n);
}

void GpsReceiver::parseGga(char** f, int n) {
  // $GPGGA,time,lat,N,lon,E,fix,sats,hdop,alt,M,geoid,M,...*cs
  parseHhmmss(f[1], snap_.utcHour, snap_.utcMin, snap_.utcSec);
  snap_.latitude = nmeaToDegrees(f[2], f[3][0]);
  snap_.longitude = nmeaToDegrees(f[4], f[5][0]);
  snap_.fixQuality = (uint8_t)atoi(f[6]);
  snap_.satellitesInUse = (uint8_t)atoi(f[7]);
  snap_.hdop = (float)atof(f[8]);
  snap_.altitudeMsl = (float)atof(f[9]);
  if (n > 11) snap_.geoidSeparation = (float)atof(f[11]);
}

void GpsReceiver::parseRmc(char** f, int n) {
  // $GPRMC,time,status,lat,N,lon,E,speed,course,date,...*cs
  parseHhmmss(f[1], snap_.utcHour, snap_.utcMin, snap_.utcSec);
  snap_.activeFix = (f[2][0] == 'A');
  snap_.latitude = nmeaToDegrees(f[3], f[4][0]);
  snap_.longitude = nmeaToDegrees(f[5], f[6][0]);
  snap_.speedKmh = (float)(atof(f[7]) * 1.852);   // knots -> km/h
  snap_.courseDeg = (float)atof(f[8]);
  parseDdmmyy(f[9], snap_.utcDay, snap_.utcMonth, snap_.utcYear);
  snap_.dateValid = (snap_.utcDay > 0 && snap_.utcDay <= 31 && snap_.utcMonth >= 1 && snap_.utcMonth <= 12);
}

void GpsReceiver::parseGsa(char** f, int n) {
  // $GPGSA,mode,fix,sat...,pdop,hdop,vdop*cs
  // f[2]=fix mode(1/2/3), f[3..14]=PRN used, f[15]=pdop,f[16]=hdop,f[17]=vdop
  snap_.fixMode = (uint8_t)atoi(f[2]);
  uint8_t used = 0;
  for (int i = 3; i <= 14 && i < n; i++) {
    if (f[i][0] != '\0') used++;
  }
  if (used > 0) snap_.satellitesInUse = used;
  if (n > 15) snap_.pdop = (float)atof(f[15]);
  if (n > 16) snap_.hdop = (float)atof(f[16]);
  if (n > 17) snap_.vdop = (float)atof(f[17]);
}

void GpsReceiver::parseGsv(char** f, int n) {
  // $GPGSV,totalMsgs,msgNum,totalInView,(prn,elev,azim,snr)xN*cs
  // f[3]=totalInView. Satellites at f[4],f[8],f[12],f[16]...
  int totalMsgs = atoi(f[1]);
  int msgNum = atoi(f[2]);
  snap_.satellitesInView = (uint8_t)atoi(f[3]);
  if (debugLogging_) {
    LOG.printf("[GPS GSV] msgs=%d msg=%d inView=%d\n", totalMsgs, msgNum, snap_.satellitesInView);
  }
  for (int base = 4; base + 3 < n; base += 4) {
    if (f[base][0] == '\0') continue;
    int prn = atoi(f[base]);
    if (prn <= 0) continue;
    int elevation = atoi(f[base + 1]);
    int azimuth = atoi(f[base + 2]);
    int snr = atoi(f[base + 3]);
    if (debugLogging_) {
      LOG.printf("[GPS GSV]   prn=%d elev=%d azim=%d snr=%d\n", prn, elevation, azimuth, snr);
    }
    uint8_t slot = (uint8_t)(prn % kGpsMaxSatellites);
    snap_.satellites[slot].prn = (uint8_t)prn;
    snap_.satellites[slot].elevation = (uint8_t)elevation;
    snap_.satellites[slot].azimuth = (uint16_t)azimuth;
    snap_.satellites[slot].snr = (uint8_t)snr;
    snap_.satellites[slot].active = true;
  }
  snap_.valid = true;
}

void GpsReceiver::parseVtg(char** f, int n) {
  // $GPVTG,course,T,...,speed(knots),N,speed(km/h),K*cs
  snap_.courseDeg = (float)atof(f[1]);
  if (n > 7 && f[7][0]) snap_.speedKmh = (float)atof(f[7]);
}

void GpsReceiver::parseGll(char** f, int n) {
  // $GPGLL,lat,N,lon,E,time,status*cs
  snap_.latitude = nmeaToDegrees(f[1], f[2][0]);
  snap_.longitude = nmeaToDegrees(f[3], f[4][0]);
  if (n > 6 && f[6][0] == 'A') snap_.activeFix = true;
}

void GpsReceiver::parseZda(char** f, int n) {
  // $GPZDA,time,day,month,year,ltzh,ltzn*cs
  parseHhmmss(f[1], snap_.utcHour, snap_.utcMin, snap_.utcSec);
  snap_.utcDay = (uint8_t)atoi(f[2]);
  snap_.utcMonth = (uint8_t)atoi(f[3]);
  snap_.utcYear = (uint16_t)atoi(f[4]);
  snap_.dateValid = true;
}

void GpsReceiver::processByte(uint8_t c) {
  // NMEA sentences are terminated "\r\n". Only '\n' ends a sentence; '\r'
  // must be ignored (NOT reset the buffer), otherwise the '\r' that arrives
  // just before every '\n' wipes line_ first and the immediately-following
  // '\n' then sees lineLen_==0 and never parses anything - which used to
  // mean *no* sentence, ever, made it past this function (issue #20: this
  // alone is enough to explain "0 satellites", independent of antenna/RF).
  if (c == '\r') return;

  if (c == '\n') {
    if (lineLen_ > 0) {
      // line_ = "$...*HH" -> validate checksum then parse.
      char* star = strchr(line_, '*');
      bool csumOk = false;
      if (star) {
        uint8_t calc = 0;
        for (const char* p = line_ + 1; p < star; ++p) calc ^= (uint8_t)*p;
        uint8_t rx = (uint8_t)strtoul(star + 1, nullptr, 16);
        csumOk = (calc == rx);
      }
if (line_[0] == '$') {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
          snap_.linesReceived++;
          snap_.lastLineMs = millis();
          if (csumOk) {
            if (debugLogging_) {
              LOG.print(F("[GPS RAW] "));
              LOG.println(line_);
            }
            snap_.sentencesParsed++;
            parseLine();
          } else {
            if (debugLogging_) {
              LOG.printf("[GPS ERR] checksum mismatch, line=\"%s\"\n", line_);
            }
            snap_.checksumErrors++;
          }
          xSemaphoreGive(mutex_);
        }
      } else if (lineLen_ > 0) {
        if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
          snap_.checksumErrors++;
          xSemaphoreGive(mutex_);
        }
      }
    }
    resetLine();
    return;
  }

  if (lineLen_ == 0 && c != '$') return;  // wait for sentence start
  if (lineLen_ < sizeof(line_) - 1) {
    line_[lineLen_++] = (char)c;
    line_[lineLen_] = '\0';
  }
}