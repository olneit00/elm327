#pragma once
//
// LogTail — in-RAM capture of the device's debug output so it can be tailed
// over the web (LAN/AP) without a serial connection.
//
// Design (feature: "web log tail without serial"):
//   - Serial.println/printf/print are captured by a Print subclass (TeePrint)
//     whose write() forwards every byte to the real HardwareSerial AND into a
//     raw byte ring buffer.
//   - Separately we keep a compact rolling array of the most recent completed
//     LINES (fixed max chars each, oldest dropped). Line-oriented consumers
//     (REST dump + SSE tail) read from this array.
//         GET /api/log          -> last N complete lines as JSON text
//         /api/log/events (SSE) -> live tail -f of new lines
//
// Thread-safety: appends come from any task (loop(), async_tcp web task,
// BLE/NimBLE callbacks). Every mutation/read is guarded by a short FreeRTOS
// mutex.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <string.h>

constexpr size_t kLogMaxLines   = 48;    // rolling line-cache capacity
constexpr size_t kLogLineLimit  = 120;   // max chars saved per line

class LogTail {
public:
    static LogTail& instance();

    void append(const uint8_t* data, size_t len);
    void append(uint8_t c) { append(&c, 1); }
    void append(char c) { append((const uint8_t*)&c, 1); }
    void append(const char* s) { append((const uint8_t*)s, strlen(s)); }

    // Copy all currently held complete lines into out (NUL-terminated),
    // oldest-first. Returns bytes written (excl NUL).
    size_t dump(char* out, size_t capacity);

    // Copy lines whose seq > afterSeq (newest since a subscriber's cursor),
    // oldest-first within that set. Sets *maxSeqOut to the highest seq seen.
    // Returns bytes written.
    size_t dumpAfter(uint64_t afterSeq, uint64_t* maxSeqOut, char* out, size_t capacity);

    // Highest seq of any held line (0 if empty) — new-subscriber baseline.
    uint64_t lastSeq();

    void reset();

private:
    LogTail();
    ~LogTail();
    void finaliseLine();     // mutex held

    struct Line { uint64_t seq; uint16_t n; };
    Line* _lines = nullptr;              // [kLogMaxLines]
    char** _lineData = nullptr;          // [kLogMaxLines][kLogLineLimit+1]
    uint16_t _accN = 0;                  // chars accumulated in the in-progress line
    char* _acc = nullptr;                // [kLogLineLimit+1]
    uint16_t _count = 0;                 // number of complete lines held (0..kLogMaxLines)
    uint64_t _seq = 0;                   // last assigned seq

    SemaphoreHandle_t _mutex;
};

// ---------------------------------------------------------------------------
// TeePrint: a Print whose write() goes to the real serial port AND LogTail.
// A global instance named `LOG` replaces all Serial.println/printf/print
// calls. "Serial" the object and Serial.begin() are left untouched so the
// physical UART keeps working.
// ---------------------------------------------------------------------------
class TeePrint : public Print {
public:
    TeePrint(HardwareSerial& serial, LogTail& sink) : _serial(serial), _sink(sink) {}

    size_t write(uint8_t b) override {
        _serial.write(b);
        _sink.append(b);
        return 1;
    }
    size_t write(const uint8_t* buffer, size_t size) override {
        _serial.write(buffer, size);
        _sink.append(buffer, size);
        return size;
    }

private:
    HardwareSerial& _serial;
    LogTail& _sink;
};

// Global tee: route ALL debug output to the real serial AND the web log tail.
// Defined in main.cpp.
extern TeePrint LOG;
