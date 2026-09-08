#pragma once

// ── HexHound - soft sleep seam ────────────────────────────────────
//
// One name for "stop this board", bound to whichever implementation the
// selected board actually has. Header only on purpose: it adds no translation
// unit, so a board that does not set HEXHOUND_HAS_SOFT_SLEEP compiles nothing
// from here and its firmware image cannot move.
//
// The #error below is the point of the file. A capability macro with no
// consumer is invisible - this project has already shipped a content enum that
// nothing read - so a board that declares HEXHOUND_HAS_SOFT_SLEEP without
// providing a way to sleep fails at compile time instead of silently growing a
// menu row that does nothing.
//
// What an implementation owes the caller:
//   * it does not return;
//   * the panel is visibly DARK afterwards, not merely blank, because a
//     backlit screen is indistinguishable from a crash to whoever is holding
//     the device;
//   * there is a wake source on an RTC-capable pad (GPIO0..GPIO21 on the
//     ESP32-S3), and waking lands back in a normal boot.
//
// What the CALLER owes it: everything above the hardware. Persist anything
// that cannot be rebuilt, and tell the owner what is about to happen, before
// calling in. See UIConfig::onLongPress().

#include "../board/board_profile.h"

#if HEXHOUND_HAS_SOFT_SLEEP

#if defined(SIMULATOR_BUILD)

#include "../hal/tft_compat.h"   // the simulator's Serial proxy

namespace hexhound {

// [env:desktop-sim-round480] selects the T-RGB board profile WITHOUT
// HEXHOUND_RGB_PANEL - the simulator has its own display backend and never
// touches esp_lcd - so it reaches this file with the capability set and no
// panel driver behind it. That is not a mistake to #error on: the sleep ROW is
// the only part of this feature that can be verified without the board in hand,
// and it has to compile here for that to be possible.
//
// It returns, which no hardware implementation may do. The one caller treats
// the call as unreachable-after and returns immediately, so the simulator lands
// back on the config screen with the SLEEPING overlay still drawn.
inline void enterSoftSleep() {
    Serial.println("[Power] SIMULATOR: soft sleep requested (no hardware to stop)");
}

}  // namespace hexhound

#elif defined(HEXHOUND_RGB_PANEL)

#include "../hal/rgb_panel_trgb.h"

namespace hexhound {

// LilyGo T-RGB. The panel rail is behind an I2C expander and cannot be cut and
// held the way the T-Display S3 cuts its own; the driver holds the ST7701S in
// reset through the expander's output latch instead. See the long comment at
// trgb::enterDeepSleep().
inline void enterSoftSleep() { trgb::enterDeepSleep(); }

}  // namespace hexhound

#elif defined(HEXHOUND_BOARD_WAVESHARE_LCD_128) || \
      defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147)

#include "soft_sleep_tft.h"

namespace hexhound {

// The two SPI-panel Waveshares: the 1.28 round (button only) and the Touch 1.47.
// One header, but deliberately NOT one recipe - the panel reset is an RTC pad on
// the 1.28 (GPIO12) and a digital pad on the Touch 1.47 (GPIO40), so they
// release their pad holds through different registers, and GPIO40 itself means
// different things on the two boards. The pin table and the copy-paste trap it
// documents are at the top of soft_sleep_tft.h.
inline void enterSoftSleep() { enterSoftSleepTft(); }

}  // namespace hexhound

#else

#error "HEXHOUND_HAS_SOFT_SLEEP is set for this board, but src/power/soft_sleep.h \
has no implementation bound for it. Sleeping a board is not generic: it needs a \
wake source on an RTC-capable pad and a way to make the panel genuinely dark, \
and both are board hardware. Add a branch here that calls this board's own \
routine, or clear the macro in src/board/board_profile.h."

#endif  // panel selection

#endif  // HEXHOUND_HAS_SOFT_SLEEP
