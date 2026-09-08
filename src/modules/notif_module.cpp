#include "notif_module.h"
#include "../hal/backlight.h"
#ifndef HEXHOUND_DISABLE_LED
#include <FastLED.h>
#endif

// ── HexHound - Notification Module Implementation ────────────────

#ifndef HEXHOUND_DISABLE_LED
// APA102 LED - shared global array registered by main.cpp setup().
// Only ONE FastLED controller exists to avoid duplicate SPI transactions.
extern CRGB g_leds[NUM_LEDS];

// Backlight keep-alive: GPIO 38 shares GPIO_OUT1 register with LED pins.
// Re-assert after every FastLED.show() to prevent any register clobbering.
extern bool g_backlightEnabled;

static inline void showLED() {
    FastLED.show();
    if (g_backlightEnabled) {
        hexhoundSetBacklight(true);
    }
}
#endif

NotifModule& NotifModule::instance() {
    static NotifModule mod;
    return mod;
}

void NotifModule::init() {
#ifdef HEXHOUND_DISABLE_LED
    Serial.println("[Notif] LED disabled");
#else
    // NOTE: FastLED controller is already registered by main.cpp setup().
    // Do NOT call FastLED.addLeds() again - duplicate registration causes
    // two SPI transactions per show() and the last writer wins (data fight).
    Serial.println("[Notif] LED initialized (shared controller)");
#endif
}

void NotifModule::update() {
    if (_active && millis() > _showUntil) {
        clear();
    }
}

void NotifModule::notify(NotifLevel level, const char* message, const char* source) {
    _current.level     = level;
    _current.timestamp = millis();
    strlcpy(_current.message, message, sizeof(_current.message));
    strlcpy(_current.source, source, sizeof(_current.source));
    _active = true;

    // Show for 5 seconds
    _showUntil = millis() + 5000;

    switch (level) {
        case NOTIF_INFO:
            setLED(0, 0, 80);     // Blue
            break;
        case NOTIF_WARN:
            setLED(80, 60, 0);    // Yellow
            break;
        case NOTIF_CRIT:
            setLED(100, 0, 0);    // Red
            break;
    }

    Serial.printf("[Notif] %s: %s [%s]\n",
                  level == NOTIF_CRIT ? "CRIT" : level == NOTIF_WARN ? "WARN" : "INFO",
                  message, source);
}

void NotifModule::clear() {
    _active = false;
    ledOff();
}

void NotifModule::setLED(uint8_t r, uint8_t g, uint8_t b) {
#ifdef HEXHOUND_DISABLE_LED
    (void)r;
    (void)g;
    (void)b;
#else
    g_leds[0] = CRGB(r, g, b);
    showLED();
#endif
}

void NotifModule::ledOff() {
#ifndef HEXHOUND_DISABLE_LED
    g_leds[0] = CRGB::Black;
    showLED();
#endif
}

void NotifModule::setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
    _active = false;  // suppress auto-clear during cutscenes
    setLED(r, g, b);
}

void NotifModule::ledClear() {
    _active = false;
    ledOff();
}
