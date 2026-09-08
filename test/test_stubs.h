#pragma once

// ── HexHound - Shared Test Stubs ──────────────────────────────────
// Minimal Arduino-like stubs for native unit tests.
// Include this BEFORE any src/ headers.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <string>
#include <functional>

// ── Arduino type stubs ────────────────────────────────────────────────────

static uint32_t s_fakeMillis = 0;
inline uint32_t millis() { return s_fakeMillis; }
inline void delay(uint32_t) {}
inline long random(long max) { return max > 0 ? rand() % max : 0; }
inline long random(long min, long max) { return min + rand() % (max - min); }

// Serial stub
class FakeSerial {
public:
    void begin(uint32_t) {}
    void println(const char*) {}
    void printf(const char*, ...) __attribute__((format(printf, 2, 3))) {}
    void print(const char*) {}
};
static FakeSerial Serial;

// Pin stubs
#define INPUT_PULLUP 0
#define OUTPUT       1
#define LOW          0
#define HIGH         1
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }

// PROGMEM stubs
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#endif

// String class (minimal Arduino-compatible)
class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    int length() const { return (int)size(); }
    void trim() {
        size_t start = find_first_not_of(" \t\n\r");
        size_t end = find_last_not_of(" \t\n\r");
        if (start == std::string::npos) { clear(); return; }
        *this = String(substr(start, end - start + 1).c_str());
    }
    String operator+(const String& other) const {
        return String((std::string(*this) + std::string(other)).c_str());
    }
    String& operator+=(const String& other) { append(other); return *this; }
    String& operator+=(const char* s) { append(s); return *this; }
    // ArduinoJson serialization support
    size_t write(uint8_t c) { push_back((char)c); return 1; }
    size_t write(const uint8_t* buf, size_t n) { append((const char*)buf, n); return n; }
};

// strlcpy for platforms that lack it
#ifndef strlcpy
inline size_t strlcpy(char* dst, const char* src, size_t siz) {
    size_t len = strlen(src);
    if (siz > 0) {
        size_t cp = (len >= siz) ? siz - 1 : len;
        memcpy(dst, src, cp);
        dst[cp] = '\0';
    }
    return len;
}
#endif

// min/max
#ifndef _TEST_MINMAX
#define _TEST_MINMAX
template<typename T> T min(T a, T b) { return a < b ? a : b; }
template<typename T> T max(T a, T b) { return a > b ? a : b; }
#endif

// ── Test framework macros ─────────────────────────────────────────────────

static int s_testsPassed = 0;
static int s_testsFailed = 0;

#define TEST(name) static void name()
#define RUN_TEST(name) do { \
    printf("  %-40s ", #name); \
    name(); \
    s_testsPassed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %d != %d\n", __FILE__, __LINE__, (int)_a, (int)_b); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: assertion failed\n", __FILE__, __LINE__); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))
