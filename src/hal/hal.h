#pragma once

// ── HexHound - Hardware Abstraction Layer ────────────────────────
// Provides a common interface for display, LED, button, and time.
// Two implementations: hal_hardware.cpp (real ESP32) and hal_sim.cpp (SDL2).

#include <cstdint>
#include <cstddef>

// ── Display HAL ─────────────────────────────────────────────────────────────

class HalDisplay {
public:
    virtual ~HalDisplay() = default;

    virtual void init() = 0;
    virtual void fillScreen(uint16_t color) = 0;
    virtual void drawPixel(int x, int y, uint16_t color) = 0;
    virtual void writePixel(int x, int y, uint16_t color) = 0;
    virtual void fillRect(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void writeFillRect(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void drawRect(int x, int y, int w, int h, uint16_t color) = 0;
    virtual void drawFastHLine(int x, int y, int w, uint16_t color) = 0;
    virtual void drawFastVLine(int x, int y, int h, uint16_t color) = 0;
    virtual void drawCircle(int cx, int cy, int r, uint16_t color) = 0;
    virtual void fillCircle(int cx, int cy, int r, uint16_t color) = 0;
    virtual void drawLine(int x0, int y0, int x1, int y1, uint16_t color) = 0;

    virtual void setTextColor(uint16_t fg, uint16_t bg) = 0;
    virtual void setTextSize(int size) = 0;
    virtual void setCursor(int x, int y) = 0;
    virtual void print(const char* str) = 0;
    virtual void print(char c) = 0;
    virtual void printf(const char* fmt, ...) = 0;

    virtual void setRotation(int r) = 0;
    virtual void startWrite() = 0;
    virtual void endWrite() = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
};

// ── LED HAL ────────────────────────────────────────────────────────────────

class HalLED {
public:
    virtual ~HalLED() = default;

    virtual void init() = 0;
    virtual void setColor(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void clear() = 0;
    virtual void setBrightness(uint8_t brightness) = 0;
};

// ── Button HAL ─────────────────────────────────────────────────────────────

class HalButton {
public:
    virtual ~HalButton() = default;

    virtual void init() = 0;
    virtual bool isPressed() = 0;
};

// ── Time HAL ───────────────────────────────────────────────────────────────

class HalTime {
public:
    virtual ~HalTime() = default;

    virtual uint32_t millis() = 0;
    virtual void delay(uint32_t ms) = 0;
};

// ── Serial HAL ─────────────────────────────────────────────────────────────

class HalSerial {
public:
    virtual ~HalSerial() = default;

    virtual void begin(uint32_t baud) = 0;
    virtual void println(const char* str) = 0;
    virtual void printf(const char* fmt, ...) = 0;
};

// ── Global HAL accessors ───────────────────────────────────────────────────

HalDisplay& halDisplay();
HalLED&     halLED();
HalButton&  halButton();
HalTime&    halTime();
HalSerial&  halSerial();

// Initialize the HAL (call once in setup/main)
void halInit();

// For simulator: check if quit was requested
bool halShouldQuit();

#ifdef SIMULATOR_BUILD
bool halSaveScreenshot(const char* path);
#endif
