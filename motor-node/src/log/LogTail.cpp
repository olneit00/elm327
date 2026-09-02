#include "log/LogTail.h"
#include <stdlib.h>

LogTail& LogTail::instance() {
    static LogTail inst;
    return inst;
}

LogTail::LogTail() {
    _mutex = xSemaphoreCreateMutex();
    if (_mutex) xSemaphoreGive(_mutex); // start with count 1 (unlocked)

    _lines = (Line*)calloc(kLogMaxLines, sizeof(Line));
    _acc   = (char*)calloc(kLogLineLimit + 1, 1);
    _lineData = (char**)calloc(kLogMaxLines, sizeof(char*));
    if (_lineData) {
        for (uint16_t i = 0; i < kLogMaxLines; i++)
            _lineData[i] = (char*)calloc(kLogLineLimit + 1, 1);
    }
}

LogTail::~LogTail() {
    if (_lineData) {
        for (uint16_t i = 0; i < kLogMaxLines; i++) free(_lineData[i]);
    }
    free(_lineData);
    free(_lines);
    free(_acc);
    if (_mutex) vSemaphoreDelete(_mutex);
}

void LogTail::reset() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        _accN = 0; _count = 0; _seq = 0;
        if (_acc) memset(_acc, 0, kLogLineLimit + 1);
        for (uint16_t i = 0; i < kLogMaxLines; i++)
            if (_lineData && _lineData[i]) memset(_lineData[i], 0, kLogLineLimit + 1);
        xSemaphoreGive(_mutex);
    }
}

// Mutex held. Push the fully-accumulated line _acc[0.._accN) into the rolling
// array, dropping the oldest if we're at capacity. seq advances by 1.
void LogTail::finaliseLine() {
    if (_accN == 0) return;
    if (!_lines || !_lineData || !_acc) { _accN = 0; return; }
    if (_count == kLogMaxLines) {
        // Drop oldest: shift all lines down by one.
        for (uint16_t i = 0; i < kLogMaxLines - 1; i++) {
            _lines[i] = _lines[i + 1];
            memcpy(_lineData[i], _lineData[i + 1], kLogLineLimit + 1);
        }
        _count--;
    }
    _lines[_count].seq = ++_seq;
    _lines[_count].n   = _accN;
    memcpy(_lineData[_count], _acc, _accN + 1); // +1 copies the trailing NUL
    _lineData[_count][kLogLineLimit] = '\0';    // paranoia
    _count++;
    _accN = 0;
}

void LogTail::append(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (size_t i = 0; i < len; i++) {
            uint8_t c = data[i];
            if (c == '\n') {
                finaliseLine();
            } else if (_accN < kLogLineLimit) {
                _acc[_accN++] = (char)c;
            }
        }
        xSemaphoreGive(_mutex);
    }
}

size_t LogTail::dump(char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { out[0]='\0'; return 0; }
    size_t j = 0;
    for (uint16_t i = 0; i < _count && j + 1 < capacity; i++) {
        size_t n = _lines[i].n;
        for (size_t m = 0; m < n && j + 1 < capacity; m++) out[j++] = _lineData[i][m];
        if (j + 1 < capacity) out[j++] = '\n';
    }
    out[j] = '\0';
    xSemaphoreGive(_mutex);
    return j;
}

size_t LogTail::dumpAfter(uint64_t afterSeq, uint64_t* maxSeqOut, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { out[0]='\0'; return 0; }
    size_t j = 0;
    for (uint16_t i = 0; i < _count; i++) {
        if (_lines[i].seq <= afterSeq) continue;
        size_t n = _lines[i].n;
        for (size_t m = 0; m < n && j + 1 < capacity; m++) out[j++] = _lineData[i][m];
        if (j + 1 < capacity) out[j++] = '\n';
        if (maxSeqOut && _lines[i].seq > *maxSeqOut) *maxSeqOut = _lines[i].seq;
    }
    out[j] = '\0';
    xSemaphoreGive(_mutex);
    return j;
}

uint64_t LogTail::lastSeq() {
    uint64_t s = 0;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s = _seq;
        xSemaphoreGive(_mutex);
    }
    return s;
}
