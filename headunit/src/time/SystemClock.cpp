#include "time/SystemClock.h"

#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace {

constexpr uint8_t kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool isLeapYear(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
    if (month == 2 && isLeapYear(year)) return 29;
    if (month < 1 || month > 12) return 30;  // defensive fallback, should never happen
    return kDaysInMonth[month - 1];
}

// Advances dt by the given number of whole seconds, correctly rolling over
// minutes/hours/days/months/years (including leap years). elapsedSeconds is
// expected to be small in practice (SystemClock::now() is called far more
// often than once per second), so the day-rollover loop below never runs
// more than a handful of iterations.
void advanceBySeconds(DateTime& dt, uint32_t elapsedSeconds) {
    uint32_t totalSeconds = dt.second + elapsedSeconds;
    dt.second = totalSeconds % 60;
    uint32_t totalMinutes = dt.minute + totalSeconds / 60;
    dt.minute = totalMinutes % 60;
    uint32_t totalHours = dt.hour + totalMinutes / 60;
    dt.hour = totalHours % 24;
    uint32_t days = totalHours / 24;

    for (uint32_t i = 0; i < days; i++) {
        dt.day++;
        if (dt.day > daysInMonth(dt.year, dt.month)) {
            dt.day = 1;
            dt.month++;
            if (dt.month > 12) {
                dt.month = 1;
                dt.year++;
            }
        }
    }
}

uint8_t monthFromAbbreviation(const char* abbr) {
    static const char* const kNames[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (uint8_t i = 0; i < 12; i++) {
        if (strncmp(abbr, kNames[i], 3) == 0) return i + 1;
    }
    return 1;
}

// Parses the compiler-provided __DATE__ ("Mmm dd yyyy") / __TIME__
// ("hh:mm:ss") macros into an initial DateTime, so the free-running
// "internal" fallback clock starts at a plausible value (roughly "now", at
// least the correct year) instead of the Unix epoch. Still reported with
// valid=false - it is a seed, not a real time source.
DateTime buildTimestampSeed() {
    DateTime dt;
    dt.source = ClockSource::Internal;
    dt.valid = false;

    char monthStr[4] = {0};
    int day = 1;
    int year = 1970;
    if (sscanf(__DATE__, "%3s %d %d", monthStr, &day, &year) == 3) {
        dt.month = monthFromAbbreviation(monthStr);
        dt.day = static_cast<uint8_t>(day);
        dt.year = static_cast<uint16_t>(year);
    }

    int hour = 0, minute = 0, second = 0;
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) == 3) {
        dt.hour = static_cast<uint8_t>(hour);
        dt.minute = static_cast<uint8_t>(minute);
        dt.second = static_cast<uint8_t>(second);
    }
    return dt;
}

// Central European Time with automatic CET/CEST DST transitions (POSIX TZ
// rule). Used for the NTP source only - RDS times are already converted to
// local time using the offset transmitted by the station itself (see
// RadioService::_pollRds()).
constexpr const char* kTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* kNtpServer1 = "pool.ntp.org";
constexpr const char* kNtpServer2 = "time.google.com";

// Epoch values below this are treated as "SNTP hasn't produced a real fix
// yet" (the ESP32 clock starts at/near the Unix epoch on boot). Corresponds
// to 2020-01-01 - comfortably in the past relative to any real deployment,
// while still being far enough from 1970 to reliably distinguish a real
// sync from the unsynced startup value.
constexpr time_t kPlausibleEpochThreshold = 1577836800;  // 2020-01-01T00:00:00Z

// Only re-apply a fresh NTP fix this often after the first one, so a
// higher-priority RDS fix (see the priority note in SystemClock.h) isn't
// immediately overwritten again on the very next loop() iteration just
// because the background SNTP client happens to still be "synced".
constexpr uint32_t kNtpResyncIntervalMs = 6UL * 60 * 60 * 1000;  // 6 hours
constexpr uint32_t kNtpPollIntervalMs = 5000;

}  // namespace

const char* clockSourceName(ClockSource source) {
    switch (source) {
        case ClockSource::Gps: return "gps";
        case ClockSource::Rds: return "rds";
        case ClockSource::Ntp: return "ntp";
        case ClockSource::Manual: return "manual";
        case ClockSource::Internal:
        default: return "internal";
    }
}

SystemClock::SystemClock() {
    _base = buildTimestampSeed();
    _baseMs = millis();
}

void SystemClock::_applyFix(DateTime dt) {
    dt.valid = true;
    _base = dt;
    _baseMs = millis();
}

DateTime SystemClock::now() const {
    DateTime dt = _base;
    uint32_t elapsedSeconds = (millis() - _baseMs) / 1000;
    advanceBySeconds(dt, elapsedSeconds);
    return dt;
}

void SystemClock::onRdsTime(uint8_t hour, uint8_t minute, uint8_t second) {
    // Bring the free-running estimate up to "now" first so the currently
    // tracked date is preserved - RDS never supplies one (see the class
    // header comment).
    DateTime dt = now();
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    dt.source = ClockSource::Rds;
    _applyFix(dt);
}

void SystemClock::setManual(const DateTime& dt) {
    DateTime copy = dt;
    copy.source = ClockSource::Manual;
    _applyFix(copy);
}

void SystemClock::_pollNtp(bool staConnected) {
    if (!staConnected) {
        // Reconfigure (call configTzTime() again) the next time we
        // reconnect, in case the network/DNS environment changed.
        _ntpConfigured = false;
        return;
    }

    if (!_ntpConfigured) {
        configTzTime(kTimezone, kNtpServer1, kNtpServer2);
        _ntpConfigured = true;
        // Poll again immediately below rather than waiting a full
        // kNtpPollIntervalMs after just (re)configuring.
        _lastNtpPollMs = 0;
    }

    if (millis() - _lastNtpPollMs < kNtpPollIntervalMs) return;
    _lastNtpPollMs = millis();

    // getLocalTime()/time() would block for the SNTP client's own timeout
    // if never synced; time(nullptr) alone never blocks, so poll that and
    // use a plausibility threshold to detect "has the background SNTP
    // client produced a real fix yet" without stalling the loop.
    time_t nowEpoch = time(nullptr);
    if (nowEpoch < kPlausibleEpochThreshold) return;

    const bool firstSync = !_ntpEverSynced;
    const bool dueForResync = (millis() - _lastNtpFixMs) > kNtpResyncIntervalMs;
    if (!firstSync && !dueForResync) return;

    struct tm timeinfo;
    localtime_r(&nowEpoch, &timeinfo);

    DateTime dt;
    dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
    dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
    dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
    dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
    dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
    dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
    dt.source = ClockSource::Ntp;
    _applyFix(dt);

    _ntpEverSynced = true;
    _lastNtpFixMs = millis();
    Serial.printf("[CLOCK] NTP synced: %04u-%02u-%02u %02u:%02u:%02u (%s)\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                  firstSync ? "initial" : "resync");
}

void SystemClock::loop(bool staConnected) {
    _pollNtp(staConnected);
}
