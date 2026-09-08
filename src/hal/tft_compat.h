#pragma once

// TFT_eSPI compatibility wrapper.
// - Simulator builds use the HAL-backed implementation.
// - HEXHOUND_SOFT_TFT builds use a local ST7735 driver for LilyGo T-Dongle S3.
// - HEXHOUND_VENDOR_TFT builds use LilyGo's esp_lcd/ST7735 path behind a
//   TFT_eSPI-shaped wrapper so the app can keep its current UI code.
// - All other hardware builds use the real TFT_eSPI library.

#if defined(SIMULATOR_BUILD)

#include "hal.h"
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifndef PI
#define PI 3.14159265358979323846f
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_ORANGE      0xFD20
#define TFT_DARKGREY    0x4208
#define TFT_BGR         0

class TFT_eSPI {
public:
    void init() { halDisplay().init(); }
    void fillScreen(uint16_t color) { halDisplay().fillScreen(color); }
    void drawPixel(int x, int y, uint16_t color) { halDisplay().drawPixel(x, y, color); }
    void writePixel(int x, int y, uint16_t color) { halDisplay().writePixel(x, y, color); }
    void fillRect(int x, int y, int w, int h, uint16_t color) { halDisplay().fillRect(x, y, w, h, color); }
    void writeFillRect(int x, int y, int w, int h, uint16_t color) { halDisplay().writeFillRect(x, y, w, h, color); }
    void drawRect(int x, int y, int w, int h, uint16_t color) { halDisplay().drawRect(x, y, w, h, color); }
    void drawFastHLine(int x, int y, int w, uint16_t color) { halDisplay().drawFastHLine(x, y, w, color); }
    void drawFastVLine(int x, int y, int h, uint16_t color) { halDisplay().drawFastVLine(x, y, h, color); }
    void drawCircle(int cx, int cy, int r, uint16_t color) { halDisplay().drawCircle(cx, cy, r, color); }
    void fillCircle(int cx, int cy, int r, uint16_t color) { halDisplay().fillCircle(cx, cy, r, color); }
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color) { halDisplay().drawLine(x0, y0, x1, y1, color); }
    void setTextColor(uint16_t fg, uint16_t bg) { halDisplay().setTextColor(fg, bg); }
    void setTextColor(uint16_t fg) { halDisplay().setTextColor(fg, TFT_BLACK); }
    void setTextSize(int size) { halDisplay().setTextSize(size); }
    void setCursor(int x, int y) { halDisplay().setCursor(x, y); }

    void print(const char* str) { halDisplay().print(str); }
    void print(char c) { halDisplay().print(c); }
    void print(int val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", val);
        halDisplay().print(buf);
    }
    void print(unsigned long val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", val);
        halDisplay().print(buf);
    }

    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        halDisplay().print(buf);
    }

    int drawString(const char* str, int x, int y, int font = 1) {
        int size = font <= 1 ? 1 : font;
        setTextSize(size);
        setCursor(x, y);
        print(str);
        return (int)strlen(str) * 6 * size;
    }

    void setRotation(int r) { halDisplay().setRotation(r); }
    void startWrite() { halDisplay().startWrite(); }
    void endWrite() { halDisplay().endWrite(); }
    void writecommand(uint8_t) {}
    void writedata(uint8_t) {}
    void invertDisplay(bool) {}

    int width() const { return halDisplay().width(); }
    int height() const { return halDisplay().height(); }
};

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#endif

inline uint32_t millis() { return halTime().millis(); }
inline void delay(uint32_t ms) { halTime().delay(ms); }
inline long random(long max) { return rand() % max; }
inline long random(long min, long max) { return min + rand() % (max - min); }

class SimSerialProxy {
public:
    void begin(uint32_t baud) { halSerial().begin(baud); }
    operator bool() const { return true; }
    void flush() {}
    void println() { halSerial().println(""); }
    void println(const char* str) { halSerial().println(str); }

    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        halSerial().println(buf);
    }

    void print(const char* str) { halSerial().println(str); }
};

extern SimSerialProxy Serial;

#define INPUT_PULLUP 0
#define OUTPUT       1
#define LOW          0
#define HIGH         1

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return halButton().isPressed() ? LOW : HIGH; }

#include <string>
class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s) {}
    String(const std::string& s) : std::string(s) {}
    int length() const { return (int)size(); }
    void trim() {
        size_t start = find_first_not_of(" \t\n\r");
        size_t end = find_last_not_of(" \t\n\r");
        if (start == std::string::npos) { clear(); return; }
        *this = String(substr(start, end - start + 1).c_str());
    }
    String substring(int from, int to) const {
        return String(substr(from, to - from).c_str());
    }
    String operator+(const String& other) const {
        return String((std::string(*this) + std::string(other)).c_str());
    }
    String& operator+=(const String& other) {
        append(other);
        return *this;
    }
    String& operator+=(const char* s) {
        append(s);
        return *this;
    }
    size_t write(uint8_t c) { push_back((char)c); return 1; }
    size_t write(const uint8_t* buf, size_t n) { append((const char*)buf, n); return n; }
};

// Arduino's core supplies strlcpy; desktop libcs differ. glibc 2.38+, musl,
// macOS and the BSDs all DECLARE strlcpy in <string.h> with C linkage, and a
// second definition here is a hard compile error there ("conflicting
// declaration ... has C language linkage"), so only fill the gap on the
// runtimes that genuinely lack it: MinGW/MSVC and pre-2.38 glibc.
// __GLIBC__ / __GLIBC_MINOR__ come in via <features.h>, pulled by <string.h>.
#if defined(_WIN32) || \
    (defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)))
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
#endif // strlcpy not a macro
#endif // libc lacks strlcpy

#ifndef min
template<typename T> T min(T a, T b) { return a < b ? a : b; }
#endif
#ifndef max
template<typename T> T max(T a, T b) { return a > b ? a : b; }
#endif

#elif defined(HEXHOUND_SOFT_TFT) || defined(HEXHOUND_VENDOR_TFT) || defined(HEXHOUND_RGB_PANEL)

// HEXHOUND_RGB_PANEL shares this class body on purpose. Only the handful of
// functions that actually touch hardware differ (constructor, init, fillScreen,
// drawPixel, fillRect, setRotation and the SPI-only command helpers); every
// composed operation above them - lines, rectangles, circles, the GLCD font,
// print/printf/drawString - is written in terms of drawPixel and fillRect and
// so is reused verbatim. Forking the class would have duplicated the text and
// geometry layer and let the copies drift.

#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>
#ifdef HEXHOUND_VENDOR_TFT
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include "../esp_lcd_st7735.h"
#endif
#ifdef HEXHOUND_RGB_PANEL
#include "rgb_panel_trgb.h"
#endif
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_ORANGE      0xFD20
#define TFT_DARKGREY    0x4208
#define TFT_BGR         0

class TFT_eSPI {
public:
    TFT_eSPI();

    void init();
    void fillScreen(uint16_t color);
    void drawPixel(int x, int y, uint16_t color);
    void writePixel(int x, int y, uint16_t color) { drawPixel(x, y, color); }
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void writeFillRect(int x, int y, int w, int h, uint16_t color) { fillRect(x, y, w, h, color); }
    void drawRect(int x, int y, int w, int h, uint16_t color);
    void drawFastHLine(int x, int y, int w, uint16_t color);
    void drawFastVLine(int x, int y, int h, uint16_t color);
    void drawCircle(int cx, int cy, int r, uint16_t color);
    void fillCircle(int cx, int cy, int r, uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);

    void setTextColor(uint16_t fg, uint16_t bg);
    void setTextColor(uint16_t fg) { setTextColor(fg, TFT_BLACK); }
    void setTextSize(int size);
    void setCursor(int x, int y);

    size_t print(const char* str);
    size_t print(char c);
    size_t print(int val);
    size_t print(unsigned long val);
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    int drawString(const char* str, int x, int y, int font = 1);

    void setRotation(int r);
    void startWrite();
    void endWrite();
    int width() const { return _width; }
    int height() const { return _height; }

    void writecommand(uint8_t cmd);
    void writedata(uint8_t data);
    void invertDisplay(bool invert);

#ifdef HEXHOUND_RGB_PANEL
    // Pushes the accumulated dirty band to the panel and clears it. Cheap and
    // safe to call when nothing is dirty. See tft_compat_rgb.cpp for why the
    // dirty region is a Y-range rather than a rectangle.
    void present();
#endif

private:
    bool _initialized;
    bool _inTransaction;
    uint8_t _rotation;
    uint8_t _colstart;
    uint8_t _rowstart;
    int _width;
    int _height;
    uint16_t _textFg;
    uint16_t _textBg;
    int _textSize;
    int _cursorX;
    int _cursorY;
#ifdef HEXHOUND_VENDOR_TFT
    esp_lcd_panel_handle_t _panel;
    esp_lcd_panel_io_handle_t _io;
#endif
#ifdef HEXHOUND_RGB_PANEL
    uint16_t* _canvas;      // 480x480 RGB565 in PSRAM, 460800 bytes
    int _dirtyTop;          // inclusive; _dirtyTop > _dirtyBottom means clean
    int _dirtyBottom;
    void markDirty(int y, int h);
#endif

    void hardReset();
    void beginScopedWrite(bool& ownsTransaction);
    void endScopedWrite(bool ownsTransaction);
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t data);
    void sendData16(uint16_t data);
    void setAddrWindow(int x, int y, int w, int h);
    void streamColor(uint16_t color, uint32_t count);
    void drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int size);
    static void rotationConfig(uint8_t rotation, uint8_t& madctl, uint8_t& colstart, uint8_t& rowstart, int& width, int& height);
#ifdef HEXHOUND_VENDOR_TFT
    void applyVendorRotation();
#endif
};

#else

#include <TFT_eSPI.h>

#endif
