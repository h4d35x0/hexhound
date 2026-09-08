#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H
#include "touch_nav.h"        // RowHitBox

class Minigame;

// ── HexHound - Minigame Select Screen ───────────────────────────
//
// Renders the game list for SCREEN_GAMES. The list comes from
// games::playableAt(), never games::at(): only playableAt() applies the
// requiredCaps filter, and a game this board cannot run must not take a menu
// slot. An option that cannot work is worse than an option that is not there.
//
// The list is therefore board-dependent and can be short, or empty if a future
// game set is entirely capability-gated. Nothing here assumes a fixed count.
//
// Same one-button convention as ui_missions: short press moves the cursor,
// long press acts on the highlighted row. Row 0 is BACK.

// Loop / scroll bound only (never an array size), so a runtime expression is
// safe. Matches MISSIONS_PER_PAGE in ui_missions.h.
#define GAMES_PER_PAGE ((SCREEN_H > 100) ? 5 : 4)

class UIGames {
public:
    static UIGames& instance();

    void init(TFT_eSPI* tft);

    // Open game select (resets cursor and scroll).
    void open();

    // Draw current state.
    void draw();

    // Short press: advance cursor.
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

    // Index into the PLAYABLE list, or -1 when BACK is highlighted.
    int selectedIndex() const { return _cursor - 1; }

    // The highlighted game, or nullptr when BACK is highlighted or the list
    // shifted underneath the cursor. Hand this straight to the SCREEN_GAME_PLAY
    // loop; the caller never has to touch the registry itself.
    Minigame* selectedGame() const;

private:
    UIGames() = default;

    int  totalEntries() const;   // BACK + playableCount()
    void drawHeader();
    void drawFooter();
    const char* actionHint() const;
#if HEXHOUND_PANEL_ROUND
    void drawRound();
#endif

    TFT_eSPI* _tft    = nullptr;
    int       _cursor = 0;
#if HEXHOUND_HAS_TOUCH
    // Row geometry as LAST DRAWN, for rowIndexAtY(). Recorded by draw() at the
    // one point that knows it. Compiled out on the boards with no touch panel,
    // so their object layout and their draw path are unchanged.
    RowHitBox _rows;
#endif
    int       _scroll = 0;  // top visible index
};
