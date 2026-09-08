#pragma once

#include "board_profile.h"

// ── HexHound - Runtime Capability Service ────────────────────────
//
// board_profile.h already answers "what does this board have?" as preprocessor
// macros. That works fine for the handful of hardware modules that consume it,
// but the companion features (quests, minigames, dens, roam, encounters) all
// need the same answers, and scattering `#if HEXHOUND_HAS_*` through a dozen
// gameplay files turns tiering into preprocessor spaghetti.
//
// This header wraps those macros in `constexpr` accessors. Gameplay code reads
// `Caps::has(CAP_IMU)` instead of an #if block. Because everything is
// constexpr, the compiler folds each call to a literal and dead-strips the
// untaken branch, so this costs exactly as much as the #if did: nothing.
//
// Rule of thumb: hardware drivers may keep using the raw macros (they must, to
// avoid compiling code for absent peripherals). Gameplay code should use Caps.

enum BoardTier : uint8_t {
    // Every supported board. Assume ~250 KB of RAM and a 160x80 panel; if a
    // feature does not fit here, it does not belong in the core loop.
    TIER_CORE = 0,
    // Boards with PSRAM. Room for large content packs, den art, richer games.
    TIER_RICH = 1
};

enum Capability : uint16_t {
    CAP_IMU          = 1 << 0,   // motion: real steps, tilt, shake
    CAP_TOUCH        = 1 << 1,
    CAP_BATTERY      = 1 << 2,   // can report charge, so can gate roam length
    CAP_SD           = 1 << 3,
    CAP_USB_HID      = 1 << 4,   // native USB device port
    CAP_PSRAM        = 1 << 5,
    CAP_ROUND_PANEL  = 1 << 6    // round layout family, not a rectangle
};

namespace Caps {

constexpr uint16_t mask() {
    return (uint16_t)(
        (HEXHOUND_HAS_IMU        ? CAP_IMU         : 0) |
        (HEXHOUND_HAS_TOUCH      ? CAP_TOUCH       : 0) |
        (HEXHOUND_HAS_BATTERY    ? CAP_BATTERY     : 0) |
        (HEXHOUND_HAS_SPI_SD     ? CAP_SD          : 0) |
        (HEXHOUND_HAS_USB_HID    ? CAP_USB_HID     : 0) |
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
        CAP_PSRAM |
#endif
        (HEXHOUND_PANEL_ROUND    ? CAP_ROUND_PANEL : 0));
}

constexpr bool has(Capability c) {
    return (mask() & (uint16_t)c) != 0;
}

constexpr BoardTier tier() {
    return has(CAP_PSRAM) ? TIER_RICH : TIER_CORE;
}

// True when the board can measure real physical movement. Deliberately its own
// question rather than an alias for CAP_IMU: boards without an IMU must earn
// exploration progress from radio-environment change instead, and must never
// present a derived number as a step count.
constexpr bool canMeasureMotion() {
    return has(CAP_IMU);
}

inline const char* boardName() {
    return HEXHOUND_BOARD_NAME;
}

inline const char* tierName() {
    return tier() == TIER_RICH ? "rich" : "core";
}

}  // namespace Caps
