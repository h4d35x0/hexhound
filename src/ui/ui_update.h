#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H

// ── HexHound - Firmware Update Screen ───────────────────────────
//
// The owner-facing half of OTA. It shows what firmware is installed, what the
// update session is doing, and it is the ONLY place in the firmware that can
// arm an update.
//
// ── Why arming lives in a screen and not in a command ─────────────────────
//
// "No automatic updates and no silent ones" is not implemented by asking for
// confirmation after a host has already started. It is implemented by the fact
// that a host cannot start at all: OtaSession refuses every update verb until
// arm() has been called, and the single call site is this screen's confirm
// action, which requires a person holding the button. There is no wire path to
// it. See the frame type list in ota_serial.h, which deliberately has no ARM.
//
// ── What it must never do ─────────────────────────────────────────────────
//
// Reboot by itself. Reaching READY means the image is verified and staged and
// the device is STILL running the old firmware; the restart is offered and the
// owner takes it. A screen that restarted on its own would take away the one
// decision the whole design exists to give them.
//
// ── Panel families ────────────────────────────────────────────────────────
//
// Compiles for all three: 160x80 compact, 320x172 big, and the 240x240 round
// panel behind HEXHOUND_PANEL_ROUND with the chord-aware helpers in ui_round.*.
// Everything panel-specific is a compile-time branch, so adding this screen
// changes no byte of any existing screen on any board.

// What the caller must do after a long press. The screen handles its own
// redrawing; these are the things it cannot do for itself.
//
// DO_RESTART is returned rather than acted on because ESP.restart() has no
// meaning on the simulator, and because a restart is worth being able to find
// by grepping main.cpp rather than having it buried in a draw file.
enum UpdateOutcome : uint8_t {
    UPDATE_STAY = 0,    // handled internally, screen already redrawn
    UPDATE_EXIT,        // owner asked to leave; caller returns to the menu
    UPDATE_DO_RESTART   // owner accepted the staged image; caller reboots
};

class UIUpdate {
public:
    static UIUpdate& instance();

    void init(TFT_eSPI* tft);

    // Open the screen (resets the cursor).
    void open();

    // Draw current state.
    void draw();

    // Called on a cadence while this screen is showing, so a transfer in
    // progress actually animates. Redraws only when something the owner can
    // see has changed, because a full repaint every 200 ms on a shared SPI bus
    // competes with the flash writes the transfer is trying to do.
    void update();

    // Short press: advance cursor.
    void scrollDown();

    // Long press: act on the highlighted row.
    UpdateOutcome act(uint32_t nowMs);

private:
    UIUpdate() = default;

    // The action rows available in the CURRENT session state. The list changes
    // with the state, which is why it is computed rather than stored: offering
    // "start update" while one is already running, or "restart" before an image
    // is staged, would be a button that lies.
    enum Row : uint8_t {
        ROW_BACK = 0,
        ROW_ARM,        // start an update: the owner gate, and the only one
        ROW_CANCEL,
        ROW_RESTART,
        ROW_LATER,
        ROW_RETRY
    };

    int  buildRows(Row* out) const;
    int  rowCount() const;

    // True when the session is back at IDLE because something was refused or
    // stopped, rather than because nothing has happened. arm() refuses while
    // the running image is on trial and leaves the state at IDLE, so without
    // this the owner presses the button and the screen does not react at all.
    bool idleAfterRefusal() const;

    // A short, owner-facing description of what the session is doing now. Not
    // OtaSession::stateName(), which is a stable wire name for the companion
    // app and reads like a protocol.
    const char* statusLine() const;

    // The sentence under the status line: the refusal help when something was
    // refused, otherwise what the owner should do next.
    const char* detailLine() const;

    static const char* rowLabel(Row r);

    // Percent of the image received, 0..100, or -1 when not receiving.
    int progressPct() const;

    // Cheap snapshot of everything visible, so update() can tell whether a
    // repaint would change any pixel.
    uint32_t visibleHash() const;

#if HEXHOUND_PANEL_ROUND
    void drawRound();
#else
    void drawRect();
#endif

    TFT_eSPI* _tft     = nullptr;
    int       _cursor  = 0;
    uint32_t  _lastHash = 0;
};
