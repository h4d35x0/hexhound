#ifndef SIMULATOR_BUILD

#include "backlight.h"
#include <stdint.h>
#include <Arduino.h>
#include "../config.h"
#ifdef HEXHOUND_VENDOR_TFT
#include <esp_idf_version.h>
#endif

namespace {

bool g_backlightPwmReady = false;

#ifdef HEXHOUND_VENDOR_TFT
constexpr uint32_t LEDC_BACKLIGHT_FREQ = 1000;
constexpr uint8_t LEDC_BACKLIGHT_BIT_WIDTH = 8;
constexpr uint8_t LEDC_BACKLIGHT_CHANNEL = 3;

void ensureBacklightPwm() {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_TFT_BL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
#else
    ledcSetup(LEDC_BACKLIGHT_CHANNEL, LEDC_BACKLIGHT_FREQ, LEDC_BACKLIGHT_BIT_WIDTH);
    ledcAttachPin(PIN_TFT_BL, LEDC_BACKLIGHT_CHANNEL);
#endif

    g_backlightPwmReady = true;
}
#endif

} // namespace

void hexhoundInitBacklightHardware() {
#ifdef HEXHOUND_VENDOR_TFT
    ensureBacklightPwm();
#else
    pinMode(PIN_TFT_BL, OUTPUT);
#endif
}

void hexhoundSetBacklight(bool on) {
#ifdef HEXHOUND_VENDOR_TFT
    ensureBacklightPwm();
    // LilyGo's factory firmware drives this backlight active-low.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_TFT_BL, on ? 0 : 255);
#else
    ledcWrite(LEDC_BACKLIGHT_CHANNEL, on ? 0 : 255);
#endif
#else
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
#endif
}

#endif
