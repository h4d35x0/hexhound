#pragma once

// ── HexHound - soft sleep for the SPI-panel Waveshare boards ─────
//
// Waveshare ESP32-S3-LCD-1.28 (round, non-touch) and Waveshare
// ESP32-S3-Touch-LCD-1.47. Both are battery powered with a single button and no
// power switch, so until now the only way to stop either of them was to let the
// cell go flat.
//
// Header only, and everything is inside the board guard, for the reason
// soft_sleep.h states about itself: it adds no translation unit, so a board that
// is not one of these two compiles nothing from here and its firmware image
// cannot move. Guarded on the board macros rather than on
// HEXHOUND_HAS_SOFT_SLEEP alone, because the T-RGB also has that capability and
// binds it to its own panel driver instead.
//
// ── THE TWO BOARDS ARE NOT THE SAME RECIPE ───────────────────────────────
//
// This is why the per-board #if below exists, and the trap is nastier than
// "different pin numbers":
//
//                    1.28 round            Touch 1.47
//     button/wake    GPIO0   (RTC)         GPIO0   (RTC)
//     panel RST      GPIO12  (RTC)         GPIO40  (DIGITAL)
//     backlight      GPIO40  (DIGITAL)     GPIO46  (DIGITAL)
//
// GPIO40 appears on BOTH boards in OPPOSITE roles: backlight on the round board,
// panel reset on the touch board. A routine copied from one to the other
// therefore still parks GPIO40 low and still LOOKS right on the glass, because
// low is the wanted level for either role - while silently leaving the OTHER pin
// on that board unheld. It would not be a compile error, and on USB it would not
// even be a visible one.
//
// The release path is where the difference is load-bearing. On the S3 the
// RTC-capable pads are GPIO0..GPIO21 and their hold bit lives in the RTC domain;
// digital pads above that have their own hold bits, armed per pin by
// gpio_hold_en() and switched on for sleep by the global
// gpio_deep_sleep_hold_en(). So:
//
//   * On the 1.28, PIN_TFT_RST is GPIO12, an RTC pad. Clearing only the digital
//     register set leaves it stuck, and the pad also has to be handed back from
//     the RTC mux to the digital GPIO mux with rtc_gpio_deinit() before
//     TFT_eSPI's own reset pulse can reach the pin at all.
//   * On the Touch 1.47, PIN_TFT_RST is GPIO40, OUTSIDE the RTC range.
//     rtc_gpio_hold_dis() can only return ESP_ERR_INVALID_ARG for it and
//     rtc_gpio_deinit() would be asking about a pad with no entry in the RTC IO
//     map. The digital hold path is the only correct one there.
//
// gpio_hold_en()/gpio_hold_dis() do dispatch to the right register set for
// either pad, so the ENTRY side happens to be uniform. The exit side is not,
// because returning an RTC pad to the digital mux has no digital equivalent.
//
// ── DELIBERATELY NOT DONE ────────────────────────────────────────────────
//
// No Serial.flush() before sleeping. platformio.ini sets
// board_build.cdc_on_boot = 1 for the Touch 1.47, and on a CDC-on-boot board
// flush() waits for the USB HOST to drain the ring buffer; a host holding the
// port open without reading it has stretched that wait to 443 s on this project.
// It would present as "did the gesture and nothing happened", which is the one
// symptom this routine must never produce. The 1.28 is cdc_on_boot = 0 - its
// USB-C is a CH343P UART bridge and Serial runs on UART0 - so it is not exposed;
// the line is omitted on both anyway, because the reason it is wrong on one
// board is not a reason it would be right on the other.

#include "../board/board_profile.h"

#if HEXHOUND_HAS_SOFT_SLEEP && !defined(SIMULATOR_BUILD) && \
    (defined(HEXHOUND_BOARD_WAVESHARE_LCD_128) ||           \
     defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147))

#define HEXHOUND_SOFT_SLEEP_TFT 1

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "../hal/backlight.h"

namespace hexhound {

// Releases every pad hold that enterSoftSleepTft() armed.
//
// MUST run on the boot path BEFORE tft->init(), and unconditionally rather than
// only on a deep-sleep wake: a pad hold lives in RTC_CNTL and survives the core
// reset that a wake performs, so it is cleared only by a power-on reset or by
// being released here. Without it the FIRST sleep is also the last useful boot -
// the panel comes up held in reset with its backlight held off, and every later
// boot shows a dead black screen indistinguishable from a brick.
//
// The T-Display S3 shipped exactly this bug; see the hold release beside
// PIN_TFT_POWER at the top of setup() in main.cpp. Called from
// hexhoundInitBoardPower() in src/board/board_support.h, which is the first GPIO
// action on the normal boot path.
//
// Every call below is a no-op on a cold boot.
inline void releaseSoftSleepHolds() {
    // Global first: this is the latch that makes digital-pad holds survive deep
    // sleep at all. It is shared, so it is cleared once rather than per pin.
    gpio_deep_sleep_hold_dis();

    // Backlight. A digital pad on both boards (GPIO40 / GPIO46), so only the
    // gpio_* register set applies to it.
    gpio_hold_dis((gpio_num_t)PIN_TFT_BL);

    // Panel reset. The one line the two boards genuinely disagree about.
#if defined(HEXHOUND_BOARD_WAVESHARE_LCD_128)
    // GPIO12 is inside GPIO0..GPIO21, so its hold is in the RTC domain. Clear
    // both register sets and return the pad to the digital GPIO mux, or
    // TFT_eSPI's reset pulse writes into an output register nothing is routed
    // to and the panel is initialised while still held in reset.
    rtc_gpio_hold_dis((gpio_num_t)PIN_TFT_RST);
    gpio_hold_dis((gpio_num_t)PIN_TFT_RST);
    rtc_gpio_deinit((gpio_num_t)PIN_TFT_RST);
#else
    // GPIO40 is outside the RTC range; the digital hold bit is the only one it
    // has. The rtc_gpio_* calls are omitted rather than called and ignored,
    // because a call that can only ever fail is a comment pretending to be code.
    gpio_hold_dis((gpio_num_t)PIN_TFT_RST);
#endif

    // GPIO0, the wake pad. esp_sleep's ext0 preparation puts it into RTC input
    // mode and holds it, so after a wake it is still owned by the RTC mux and
    // main.cpp's pinMode()/digitalRead() would be talking to a pad no longer
    // routed to the digital GPIO matrix. The button would simply stop working,
    // on every boot after the first sleep.
    rtc_gpio_hold_dis((gpio_num_t)PIN_BUTTON);
    rtc_gpio_deinit((gpio_num_t)PIN_BUTTON);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

// Blanks the panel, parks and HOLDS this board's panel lines, and puts the S3
// into deep sleep with the BOOT button armed as the wake source. DOES NOT
// RETURN.
//
// The caller owes it everything above the hardware - saving state, telling the
// owner what is about to happen - because the last statement here is
// esp_deep_sleep_start(). Wake is a full boot through setup().
inline void enterSoftSleepTft() {
    // 1. Backlight off FIRST, so the panel reset below is not a visible flash.
    //
    // hexhoundSetBacklight() rather than a local digitalWrite: the off LEVEL is
    // a board fact, and it is already stated once, in src/hal/backlight.cpp.
    // Holding it is what turns "driven off now" into "still driven off with the
    // CPU stopped", which is the difference between a dark device and a lit
    // blank one.
    hexhoundSetBacklight(false);
    gpio_hold_en((gpio_num_t)PIN_TFT_BL);

    // 2. Panel controller into reset, and held there.
    //
    // The backlight alone would satisfy "the screen is dark", but "the backlight
    // is off" and "the panel is off" are different claims and only the second
    // survives someone finding the device in a bag and shining a light at it.
    // Same reasoning as trgb::enterDeepSleep(), which latches the ST7701S reset
    // through its I2C expander for exactly this.
    pinMode(PIN_TFT_RST, OUTPUT);
    digitalWrite(PIN_TFT_RST, LOW);
    gpio_hold_en((gpio_num_t)PIN_TFT_RST);

    // 3. Arm the holds for deep sleep.
    //
    // Needed on BOTH boards, and it is the call that is easy to leave out:
    // ESP-IDF's own gpio.h says in as many words that gpio_hold_en() alone will
    // NOT hold a digital pad through deep sleep. Each of these boards holds at
    // least one digital pad - the backlight on both, and the reset as well on
    // the Touch 1.47 - so without this line the panel comes back to life the
    // instant the CPU stops.
    gpio_deep_sleep_hold_en();

    // 4. Wake on the BOOT button going low.
    //
    // GPIO0 is RTC-capable on both boards. It is also a strapping pin, and the
    // obvious worry is that waking with it held low lands in ROM download mode.
    // It does not: the strap latches sample at CHIP reset, and a deep-sleep wake
    // is one level below that, a CORE reset that leaves the RTC domain and the
    // latch sampled at power-on alone. The fuller version of that argument is at
    // trgb::enterDeepSleep().
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);

    // Hold the pull-up in the RTC domain. Without it the pad floats once the
    // digital domain powers down, and a floating input wakes the board
    // immediately, which presents as sleep doing nothing at all.
    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);

    // 5. Never sleep while the button asking for sleep is still down.
    //
    // This matters far more here than on the T-RGB. That board's entry gesture
    // is a touch hold, so its button is usually not involved at all. The 1.28
    // has no touch at all, the gesture IS a button hold, and handleButton()
    // fires onLongPress() the moment BUTTON_LONG_PRESS_MS elapses, WHILE the
    // button is still pressed. Sleeping into a low ext0 level would wake the
    // board instantly and read as "it just restarted".
    //
    // Bounded, because a pin stuck low here would otherwise be an infinite loop
    // behind a dark screen - the exact false hang this feature exists to remove.
    // If the wait does time out, sleeping anyway costs one immediate wake into a
    // normal boot, which is recoverable; hanging is not.
    const uint32_t start = millis();
    while (digitalRead(PIN_BUTTON) == LOW && (millis() - start) < 3000) {
        delay(20);
    }
    delay(120);   // debounce the release

    Serial.println("[Power] Entering deep sleep. Press the BOOT button to wake.");
    esp_deep_sleep_start();
}

}  // namespace hexhound

#endif  // board selection
