#ifndef SIMULATOR_BUILD

// ── HexHound - Hardware HAL (ESP32 / TFT_eSPI / FastLED) ────────

#include "hal.h"
#include <Arduino.h>
#include "tft_compat.h"
#include "backlight.h"
#include "../board/board_support.h"
#ifndef HEXHOUND_DISABLE_LED
#include <FastLED.h>
#endif
#include "../config.h"

// ── Display ────────────────────────────────────────────────────────────────

class HwDisplay : public HalDisplay {
public:
    HwDisplay() : _tft(nullptr) {}

    void init() override {
        // Reset display pins to a clean GPIO state before driver init.
        hexhoundResetDisplayPins();

        // Hold backlight OFF during init
        hexhoundInitBacklightHardware();
        hexhoundSetBacklight(false);

        _tft = new TFT_eSPI();  // Heap-allocate to avoid static constructor crash
        _tft->init();
        hexhoundApplyBoardDisplayInit(_tft);
        _tft->setRotation(1);
        _tft->fillScreen(TFT_BLACK);

        // Backlight ON after content is drawn
        hexhoundSetBacklight(true);
    }

    void fillScreen(uint16_t color) override { _tft->fillScreen(color); }
    void drawPixel(int x, int y, uint16_t color) override { _tft->drawPixel(x, y, color); }
    void writePixel(int x, int y, uint16_t color) override { _tft->drawPixel(x, y, color); }
    void fillRect(int x, int y, int w, int h, uint16_t color) override { _tft->fillRect(x, y, w, h, color); }
    void writeFillRect(int x, int y, int w, int h, uint16_t color) override { _tft->fillRect(x, y, w, h, color); }
    void drawRect(int x, int y, int w, int h, uint16_t color) override { _tft->drawRect(x, y, w, h, color); }
    void drawFastHLine(int x, int y, int w, uint16_t color) override { _tft->drawFastHLine(x, y, w, color); }
    void drawFastVLine(int x, int y, int h, uint16_t color) override { _tft->drawFastVLine(x, y, h, color); }
    void drawCircle(int cx, int cy, int r, uint16_t color) override { _tft->drawCircle(cx, cy, r, color); }
    void fillCircle(int cx, int cy, int r, uint16_t color) override { _tft->fillCircle(cx, cy, r, color); }
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color) override { _tft->drawLine(x0, y0, x1, y1, color); }

    void setTextColor(uint16_t fg, uint16_t bg) override { _tft->setTextColor(fg, bg); }
    void setTextSize(int size) override { _tft->setTextSize(size); }
    void setCursor(int x, int y) override { _tft->setCursor(x, y); }

    void print(const char* str) override { _tft->print(str); }
    void print(char c) override { _tft->print(c); }

    void printf(const char* fmt, ...) override {
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        _tft->print(buf);
    }

    void setRotation(int r) override { _tft->setRotation(r); }
    void startWrite() override { _tft->startWrite(); }
    void endWrite() override { _tft->endWrite(); }

    int width() const override { return _tft->width(); }
    int height() const override { return _tft->height(); }

private:
    mutable TFT_eSPI* _tft;
};

// ── LED ────────────────────────────────────────────────────────────────────

#ifndef HEXHOUND_DISABLE_LED
static CRGB _leds[NUM_LEDS];
#endif

class HwLED : public HalLED {
public:
    void init() override {
#ifndef HEXHOUND_DISABLE_LED
        hexhoundInitSharedLedController(_leds, NUM_LEDS);
        FastLED.setBrightness(LED_BRIGHTNESS);
#endif
        clear();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
#ifndef HEXHOUND_DISABLE_LED
        _leds[0] = CRGB(r, g, b);
        FastLED.show();
#else
        (void)r;
        (void)g;
        (void)b;
#endif
    }

    void clear() override {
#ifndef HEXHOUND_DISABLE_LED
        _leds[0] = CRGB::Black;
        FastLED.show();
#endif
    }

    void setBrightness(uint8_t brightness) override {
#ifndef HEXHOUND_DISABLE_LED
        FastLED.setBrightness(brightness);
#else
        (void)brightness;
#endif
    }
};

// ── Button ─────────────────────────────────────────────────────────────────

class HwButton : public HalButton {
public:
    void init() override {
        pinMode(PIN_BUTTON, INPUT_PULLUP);
    }

    bool isPressed() override {
        return digitalRead(PIN_BUTTON) == LOW;
    }
};

// ── Time ───────────────────────────────────────────────────────────────────

class HwTime : public HalTime {
public:
    uint32_t millis() override { return ::millis(); }
    void delay(uint32_t ms) override { ::delay(ms); }
};

// ── Serial ─────────────────────────────────────────────────────────────────

class HwSerial : public HalSerial {
public:
    void begin(uint32_t baud) override { Serial.begin(baud); }

    void println(const char* str) override { Serial.println(str); }

    void printf(const char* fmt, ...) override {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.print(buf);
    }
};

// ── Global instances ───────────────────────────────────────────────────────

static HwDisplay s_display;
static HwLED     s_led;
static HwButton  s_button;
static HwTime    s_time;
static HwSerial  s_serial;

HalDisplay& halDisplay() { return s_display; }
HalLED&     halLED()     { return s_led; }
HalButton&  halButton()  { return s_button; }
HalTime&    halTime()    { return s_time; }
HalSerial&  halSerial()  { return s_serial; }

void halInit() {
    s_serial.begin(115200);
    s_button.init();
    s_display.init();
    s_led.init();
}

bool halShouldQuit() { return false; }

#endif // !SIMULATOR_BUILD

