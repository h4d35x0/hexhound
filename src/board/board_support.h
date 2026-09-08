#pragma once

#ifndef SIMULATOR_BUILD

#include "../hal/tft_compat.h"
#ifndef HEXHOUND_DISABLE_LED
#include <FastLED.h>
#endif
#include <driver/gpio.h>
#include "board_profile.h"

// Soft sleep on these two boards parks and HOLDS their panel lines so deep
// sleep is genuinely dark, and an S3 pad hold SURVIVES the core reset that a
// wake performs. Releasing it is therefore a BOOT-path job, not a wake-path
// one, and it has to happen before anything drives those pins - a held pad
// silently discards every digitalWrite() aimed at it. Guarded on the board
// macros exactly the way hexhoundApplyBoardDisplayInit() below already is, so
// no other board compiles a byte of this.
#if defined(HEXHOUND_BOARD_WAVESHARE_LCD_128) || \
    defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147)
#include "../power/soft_sleep_tft.h"
#endif

// A pin of -1 means "this board has no such line". T-RGB drives its panel over
// the RGB parallel peripheral with the chip selects on an I2C expander, so it
// legitimately declares the whole 4-wire SPI group absent. gpio_reset_pin()
// asserts on a negative number rather than ignoring it, which boot-looped the
// board, and `#if defined(PIN_TFT_MOSI)` does not catch it because -1 is still
// defined. Guard on the VALUE, not on the macro existing.
inline void hexhoundResetPinIfValid(int pin) {
    if (pin >= 0) {
        gpio_reset_pin((gpio_num_t)pin);
    }
}

inline void hexhoundResetDisplayPins() {
    hexhoundResetPinIfValid(PIN_TFT_RST);
    hexhoundResetPinIfValid(PIN_TFT_DC);
    hexhoundResetPinIfValid(PIN_TFT_CS);
    hexhoundResetPinIfValid(PIN_TFT_BL);
#if defined(PIN_TFT_MOSI)
    hexhoundResetPinIfValid(PIN_TFT_MOSI);
#endif
#if defined(PIN_TFT_SCLK)
    hexhoundResetPinIfValid(PIN_TFT_SCLK);
#endif
#if defined(PIN_TFT_WR)
    hexhoundResetPinIfValid(PIN_TFT_WR);
#endif
#if defined(PIN_TFT_RD)
    hexhoundResetPinIfValid(PIN_TFT_RD);
#endif
#if defined(PIN_TFT_D0)
    hexhoundResetPinIfValid(PIN_TFT_D0);
    hexhoundResetPinIfValid(PIN_TFT_D1);
    hexhoundResetPinIfValid(PIN_TFT_D2);
    hexhoundResetPinIfValid(PIN_TFT_D3);
    hexhoundResetPinIfValid(PIN_TFT_D4);
    hexhoundResetPinIfValid(PIN_TFT_D5);
    hexhoundResetPinIfValid(PIN_TFT_D6);
    hexhoundResetPinIfValid(PIN_TFT_D7);
#endif
}

inline void hexhoundInitBoardPower() {
#if defined(PIN_TFT_POWER)
    pinMode(PIN_TFT_POWER, OUTPUT);
    digitalWrite(PIN_TFT_POWER, HIGH);
#endif
#if defined(HEXHOUND_SOFT_SLEEP_TFT)
    // This call is the whole reason the T-Display S3's lit-blank bug cannot
    // repeat on these two boards. It sits here rather than anywhere later
    // because main.cpp's NORMAL BOOT branch calls hexhoundInitBoardPower()
    // before hexhoundInitBacklightHardware(), before hexhoundResetDisplayPins()
    // and long before tft->init() - and gpio_reset_pin() does NOT clear a hold,
    // so nothing downstream would have done it instead.
    hexhound::releaseSoftSleepHolds();
#endif
}

inline void hexhoundInitSharedLedController(
#ifndef HEXHOUND_DISABLE_LED
    CRGB* leds, int count
#else
    void* leds, int count
#endif
) {
#ifdef HEXHOUND_DISABLE_LED
    (void)leds;
    (void)count;
#else
#if defined(HEXHOUND_LED_IS_APA102)
    FastLED.addLeds<APA102, PIN_LED_DATA, PIN_LED_CLK, BGR>(leds, count);
#elif defined(HEXHOUND_LED_IS_WS2812)
    FastLED.addLeds<WS2812, PIN_LED_DATA, GRB>(leds, count);
#else
    (void)leds;
    (void)count;
#endif
#endif
}

inline bool hexhoundHasSpiSdCard() {
#if defined(HEXHOUND_HAS_SPI_SD) && HEXHOUND_HAS_SPI_SD
    return true;
#else
    return false;
#endif
}

inline void hexhoundWriteCommand(TFT_eSPI* tft, uint8_t cmd,
                                 const uint8_t* data = nullptr,
                                 size_t dataLen = 0) {
    if (!tft) {
        return;
    }

    tft->startWrite();
    tft->writecommand(cmd);
    for (size_t i = 0; i < dataLen; i++) {
        tft->writedata(data[i]);
    }
    tft->endWrite();
}

inline void hexhoundApplyBoardDisplayInit(TFT_eSPI* tft) {
#if defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147)
    if (!tft) {
        return;
    }

    delay(120);

    const uint8_t df[] = { 0x98, 0x53 };
    const uint8_t b2[] = { 0x23 };
    const uint8_t b7a[] = { 0x00, 0x47, 0x00, 0x6F };
    const uint8_t bb[] = { 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0 };
    const uint8_t c0[] = { 0x44, 0xA4 };
    const uint8_t c1a[] = { 0x16 };
    const uint8_t c3[] = { 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77 };
    const uint8_t c4a[] = { 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82 };
    const uint8_t c8[] = {
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00
    };
    const uint8_t d0[] = { 0x04, 0x06, 0x6B, 0x0F, 0x00 };
    const uint8_t d7[] = { 0x00, 0x30 };
    const uint8_t e6[] = { 0x14 };
    const uint8_t de1[] = { 0x01 };
    const uint8_t b7b[] = { 0x03, 0x13, 0xEF, 0x35, 0x35 };
    const uint8_t c1b[] = { 0x14, 0x15, 0xC0 };
    const uint8_t c2[] = { 0x06, 0x3A };
    const uint8_t c4b[] = { 0x72, 0x12 };
    const uint8_t be[] = { 0x00 };
    const uint8_t de2[] = { 0x02 };
    const uint8_t e5a[] = { 0x00, 0x02, 0x00 };
    const uint8_t e5b[] = { 0x01, 0x02, 0x00 };
    const uint8_t de0[] = { 0x00 };
    const uint8_t g35[] = { 0x00 };
    const uint8_t g3a[] = { 0x05 };
    const uint8_t g2a[] = { 0x00, 0x22, 0x00, 0xCD };
    const uint8_t g2b[] = { 0x00, 0x00, 0x01, 0x3F };
    const uint8_t g36[] = { 0x00 };

    hexhoundWriteCommand(tft, 0x11);
    delay(120);
    hexhoundWriteCommand(tft, 0xDF, df, sizeof(df));
    hexhoundWriteCommand(tft, 0xB2, b2, sizeof(b2));
    hexhoundWriteCommand(tft, 0xB7, b7a, sizeof(b7a));
    hexhoundWriteCommand(tft, 0xBB, bb, sizeof(bb));
    hexhoundWriteCommand(tft, 0xC0, c0, sizeof(c0));
    hexhoundWriteCommand(tft, 0xC1, c1a, sizeof(c1a));
    hexhoundWriteCommand(tft, 0xC3, c3, sizeof(c3));
    hexhoundWriteCommand(tft, 0xC4, c4a, sizeof(c4a));
    hexhoundWriteCommand(tft, 0xC8, c8, sizeof(c8));
    hexhoundWriteCommand(tft, 0xD0, d0, sizeof(d0));
    hexhoundWriteCommand(tft, 0xD7, d7, sizeof(d7));
    hexhoundWriteCommand(tft, 0xE6, e6, sizeof(e6));
    hexhoundWriteCommand(tft, 0xDE, de1, sizeof(de1));
    hexhoundWriteCommand(tft, 0xB7, b7b, sizeof(b7b));
    hexhoundWriteCommand(tft, 0xC1, c1b, sizeof(c1b));
    hexhoundWriteCommand(tft, 0xC2, c2, sizeof(c2));
    hexhoundWriteCommand(tft, 0xC4, c4b, sizeof(c4b));
    hexhoundWriteCommand(tft, 0xBE, be, sizeof(be));
    hexhoundWriteCommand(tft, 0xDE, de2, sizeof(de2));
    hexhoundWriteCommand(tft, 0xE5, e5a, sizeof(e5a));
    hexhoundWriteCommand(tft, 0xE5, e5b, sizeof(e5b));
    hexhoundWriteCommand(tft, 0xDE, de0, sizeof(de0));
    hexhoundWriteCommand(tft, 0x35, g35, sizeof(g35));
    hexhoundWriteCommand(tft, 0x3A, g3a, sizeof(g3a));
    hexhoundWriteCommand(tft, 0x2A, g2a, sizeof(g2a));
    hexhoundWriteCommand(tft, 0x2B, g2b, sizeof(g2b));
    hexhoundWriteCommand(tft, 0xDE, de2, sizeof(de2));
    hexhoundWriteCommand(tft, 0xE5, e5a, sizeof(e5a));
    hexhoundWriteCommand(tft, 0xDE, de0, sizeof(de0));
    hexhoundWriteCommand(tft, 0x36, g36, sizeof(g36));
    hexhoundWriteCommand(tft, 0x21);
    delay(10);
    hexhoundWriteCommand(tft, 0x29);
#else
    (void)tft;
#endif
}

#endif
