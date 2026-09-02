#pragma once
//
// SystemClock - best-known wall-clock date/time for the web API
// (issue #9, GET /api/time), independent of the LVGL clock-screen
// animation (see time/TimeProvider.h / MillisClock, which only tracks
// hour/minute/second and is unaffected by this).
//
// Sources, in the priority order requested in issue #9 (GPS > RDS > NTP >
// manual > internal fallback):
//   - GPS:      not implemented yet (no GPS hardware in this repo today -
//               see issue #9's own scope note). ClockSource::Gps exists so
//               a future GPS module can plug in without changing the
//               priority logic below.
//   - RDS:      RadioService::setTimeCallback() delivers hour/minute/second
//               decoded from RDS group 4A whenever a station broadcasts it.
//               RDS does NOT reliably carry a date (the PU2CLR library's
//               own getRdsTime() is documented "Under Test and
//               construction" for the date/MJD part - see the plan doc),
//               so an RDS fix only corrects hour/minute/second and leaves
//               whatever date is currently being tracked to keep
//               free-running from.
//   - NTP:      SNTP via WiFi, only possible once the device has an STA
//               connection (see WifiManager::isStaConnected()). This is
//               the only source that can correct the *date*.
//   - manual:   reserved for a future "set time" UI/API action
//               (POST /api/time).
//   - internal: free-running fallback seeded from the firmware's own
//               build timestamp at boot, so the clock shows something
//               plausible even before any real fix arrives. Always
//               reported as invalid (valid=false) since it is not
//               synchronized to anything real.
//

#include <stdint.h>

enum class ClockSource : uint8_t {
    Internal = 0,
    Manual,
    Ntp,
    Rds,
    Gps,
};

// Human-readable source name matching the strings used in the GET
// /api/time JSON response ("internal", "manual", "ntp", "rds", "gps").
const char* clockSourceName(ClockSource source);

struct DateTime {
    bool valid = false;      // true once corrected by a real source (not "internal")
    ClockSource source = ClockSource::Internal;
    uint16_t year = 1970;
    uint8_t month = 1;       // 1-12
    uint8_t day = 1;         // 1-31
    uint8_t hour = 0;        // 0-23
    uint8_t minute = 0;      // 0-59
    uint8_t second = 0;      // 0-59
};

class SystemClock {
public:
    SystemClock();

    // Call once per main loop() iteration. Drives the periodic NTP
    // (re)sync attempt - only actually contacts a time server while
    // staConnected is true, and only re-applies a fix at most once every
    // few hours after the first one, so it doesn't fight with more
    // frequent RDS fixes for which source is reported (see the priority
    // note above).
    void loop(bool staConnected);

    // Feed a time-of-day fix decoded from RDS group 4A. Only overwrites
    // hour/minute/second; the date keeps advancing from whatever was
    // already tracked.
    void onRdsTime(uint8_t hour, uint8_t minute, uint8_t second);

    // Manual override, e.g. from a future settings UI action or
    // POST /api/time.
    void setManual(const DateTime& dt);

    // Best current estimate: the last accepted fix, advanced by the
    // elapsed wall-clock time (via millis()) since it was applied.
    DateTime now() const;

private:
    void _applyFix(DateTime dt);
    void _pollNtp(bool staConnected);

    DateTime _base;
    uint32_t _baseMs = 0;

    bool _ntpConfigured = false;
    bool _ntpEverSynced = false;
    uint32_t _lastNtpPollMs = 0;
    uint32_t _lastNtpFixMs = 0;
};
