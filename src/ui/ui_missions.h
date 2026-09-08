#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H
#include "touch_nav.h"        // RowHitBox

// ── HexHound - Mission Select Screen ──────────────────────────────
// Scrollable list of USB HID missions. Accessible from Gremlin Mode.

// Loop / scroll bound only (never an array size), so a runtime expression is
// safe. Large panels (320x172) fit 5 size-2 rows; the 160x80 keeps 4.
#define MISSIONS_PER_PAGE ((SCREEN_H > 100) ? 5 : 4)

class UIMissions {
public:
    static UIMissions& instance();

    void init(TFT_eSPI* tft);

    // Open mission select (resets cursor)
    void open();

    // Draw current state
    void draw();

    // Short press: advance cursor
    void scrollDown();

    // ── Touch-only additions ──────────────────────────────────────────────
    // Guarded rather than merely left uncalled, for the reason spelled out in
    // ui_menu.h: --gc-sections DOES drop an unreferenced method, and dropping
    // it still did not leave the six non-touch boards' images byte-identical.
    // Adding touch must not move a byte of a board that has no touch panel.
#if HEXHOUND_HAS_TOUCH
    // Which list index a touch at screen y landed on, or -1 for the dead space
    // above or below the rows. Answered from the geometry the LAST draw()
    // actually used, not recomputed. See RowHitBox.
    int rowIndexAtY(int y) const { return _rows.indexAtY(y); }

    // Page the WINDOW a whole screenful, carrying the cursor into it. This is
    // the swipe verb; scrollDown() above is the button verb and steps one row.
    // Both are needed - the boards with a touch panel still have a button.
    // Clamping lives once, in uiPageBy() in touch_nav.h.
    void pageDown();
    void pageUp();

    // Put the cursor straight on a row. This is the one thing a touch panel can
    // do that a single button cannot: a tap already NAMES the row, so walking
    // to it one short press at a time is pure ceremony - which is exactly the
    // complaint this exists to answer.
    //
    // Does NOT draw. A tap keeps the screen up and needs the repaint; a hold
    // opens the row and the repaint would be a wasted full-screen blit, which
    // on the 480x480 panel is the entire 39 ms frame budget. The caller knows
    // which case it is in and the two must not both pay for it.
    //
    // Out-of-range is ignored rather than clamped. A clamp would silently move
    // the cursor somewhere the finger never was.
    void setCursor(int index);
#endif

    bool backSelected() const { return _cursor == 0; }
    // Long press: select highlighted mission (returns mission index)
    int selectedMission() const { return _cursor - 1; }

    // Mission briefing screen - shows description, USB status.
    //
    // ALSO the disarm point for the confirm gate below: entering this screen
    // always starts disarmed. That is why the gate needs no matching "leaving
    // the screen" call in main.cpp - there is no second thing for a caller to
    // forget. See longPressShouldExecute().
    void drawBriefing(int missionIndex);

    // ── GUI-keys confirm gate ─────────────────────────────────────────────
    //
    // The one question the mission-brief long press has to ask before it runs
    // anything. Answers "may THIS mission execute on THIS press?".
    //
    //   true  - run it. Either the mission does not press the host's GUI
    //           modifier, or it does and this is the second, confirming hold.
    //   false - do NOT run. Nothing has happened to the host. For a GUI-keys
    //           mission the screen has been repainted with a warning naming
    //           the actual key combination, and is waiting for a second hold.
    //
    // A mission WITHOUT MISSION_GUI_KEYS returns true on the first call and
    // repaints nothing, so the majority path keeps exactly the behaviour it
    // had. That is deliberate: seven of the nine missions only type text, and
    // a regression there would be worse than the defect this closes.
    //
    // Every refusal path returns false, including "the index is out of range"
    // and "there is no display to warn on". Refusing to run costs one more
    // hold; running something the owner did not confirm is the incident this
    // exists to prevent, so the two are not symmetric and the gate always
    // fails closed.
    //
    // NOT const and NOT idempotent: calling it twice for one physical press
    // is a wiring bug. See ARM_MIN_CONFIRM_MS in the .cpp for what stops that
    // second call from being read as the owner's confirmation.
    bool longPressShouldExecute(int missionIndex);

    // True while a GUI-keys mission is armed and the brief screen is showing
    // the warning rather than the briefing. Read-only; exists for tests and
    // for a caller that wants to label the screen, never as a gate - the gate
    // is longPressShouldExecute() and asking this instead is the one misuse
    // that would reintroduce the defect.
    bool isArmed() const { return _armed >= 0; }

private:
    UIMissions() = default;
    void drawHeader();
    void drawFooter();
    int totalEntries() const;
    // The armed repaint. Private because it must not be reachable except
    // through longPressShouldExecute(): a screen that says "hold again to
    // run" while nothing is actually armed would be a lie in the one place
    // the owner is being asked to trust the wording.
    void drawBriefingArmed(int missionIndex);
#if HEXHOUND_PANEL_ROUND
    // This screen had NO round layout until the T-RGB arrived, and nobody
    // noticed because the only round board before it (Waveshare 1.28) has no
    // native USB port: HEXHOUND_HAS_USB_HID is 0 there, so menuItemVisible()
    // hides MISSIONS outright. T-RGB is the first round board that can reach
    // this screen, and without these it fell through to the rectangular
    // layout and drew itself into the top-left corner under the bezel.
    void drawRound();
    void drawBriefingRound(int missionIndex);
    void drawBriefingArmedRound(int missionIndex);
#endif

    TFT_eSPI* _tft    = nullptr;
    int       _cursor = 0;
    // Which mission is armed, or -1 for none. Stores the INDEX, not a bool,
    // so a confirming hold has to match the mission that was armed. A bool
    // would let an arm on Lock Screen confirm a later Map Link, which is the
    // "opened a different mission than the one being looked at" shape of the
    // defect all over again, one screen further in.
    int       _armed   = -1;
    uint32_t  _armedAt = 0;   // millis() at the moment it armed
#if HEXHOUND_HAS_TOUCH
    // Row geometry as LAST DRAWN, for rowIndexAtY(). Recorded by draw() at the
    // one point that knows it. Compiled out on the boards with no touch panel,
    // so their object layout and their draw path are unchanged.
    RowHitBox _rows;
#endif
    int       _scroll = 0;  // top visible index
};
