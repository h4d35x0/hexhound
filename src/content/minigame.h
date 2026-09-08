#pragma once

#include "content_types.h"
#include "../board/board_profile.h"   // HEXHOUND_HAS_TOUCH
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

class TFT_eSPI;

// ── HexHound - Minigame Interface ────────────────────────────────
//
// One interface every minigame implements, so adding a game costs a class and
// a table entry rather than new plumbing in main.cpp.
//
// Constraints every core-tier game must respect:
//
//  * ONE BUTTON. The T-Dongle S3 has a single button, and it is the primary
//    board. Short press is the action; long press always exits. Games that
//    need touch, a second button or the IMU are fine, but they must declare it
//    through requiredCaps() and will simply not be offered elsewhere.
//  * 160x80. The smallest panel is the design target. Anything that fits there
//    scales up; the reverse is not true.
//  * No heap churn in update(). Allocate in begin(), free in end(). A game
//    that fragments the heap on a device with no PSRAM will eventually fail to
//    render something else entirely, and the cause will look unrelated.
//  * update() must return promptly. It runs from the main loop, which still
//    has to service the event bus and the button.

class Minigame {
public:
    virtual ~Minigame() {}

    // Stable identifier, used for high scores and "favourite game" memory.
    virtual const char* id() const = 0;
    // Shown in the game select list. Keep it short: 160x80.
    virtual const char* name() const = 0;
    // Capability bits from capabilities.h that this game needs. 0 = any board.
    virtual uint16_t requiredCaps() const { return 0; }

    // Called once when the game is entered. Allocate here.
    virtual void begin(TFT_eSPI* tft) = 0;
    // Called every main-loop iteration while the game is the active screen.
    // Returns true while still playing, false once the round is over.
    virtual bool update(uint32_t nowMs) = 0;
    // Short button press during play.
    virtual void onPress() = 0;

#if HEXHOUND_HAS_TOUCH
    // A press that happened AT a point, on a board that has a touch panel.
    //
    // Defaults to the positionless press, so a game that has nothing at that
    // point - packet chase, where the only verb is "hop" - inherits the right
    // behaviour and is not obliged to care. Only the games with something to
    // aim at override it.
    //
    // Guarded because a virtual added unconditionally puts a slot in every
    // game's vtable on all eight boards, and the five without a touch panel
    // can never call it.
    virtual void onPressAt(int x, int y) { (void)x; (void)y; onPress(); }
#endif
    // Called once when the round ends or the player exits. Free here.
    virtual void end() = 0;

    // Valid only after update() has returned false or end() has been called.
    virtual MinigameResult result() const = 0;
};
