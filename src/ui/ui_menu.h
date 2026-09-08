#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"

// ── HexHound - Main Menu + Pet Stats Screen ─────────────────────

// Identity of a menu entry. The order here is NOT the display order - see
// MENU_ORDER in ui_menu.cpp. Keep these values stable; main.cpp switches on
// them.
enum MenuItem : uint8_t {
    MENU_PATROL = 0,
    MENU_JOURNAL,
    MENU_CONFIG,
    MENU_STATS,
    MENU_ROTATE_DISPLAY,
    MENU_MISSIONS,       // Gremlin Mode+ only
    // Phase 0 companion entries. APPEND ONLY - main.cpp switches on these
    // values, so inserting above would silently repoint existing rows.
    MENU_QUESTS,
    MENU_GAMES,
    // Phase 1. APPEND ONLY, same rule as above.
    MENU_INVENTORY,
    MENU_DEN,
    MENU_ROAM,
    MENU_REPORT,
    // Phase 3. APPEND ONLY, same rule as above.
    MENU_UPDATE,
    // Phase 4. APPEND ONLY, same rule as above.
    MENU_CLOSET,
    MENU_ITEM_COUNT_ALL  // total possible items
};

class UIMenu {
public:
    static UIMenu& instance();

    void init(TFT_eSPI* tft);

    // Open menu (resets cursor)
    void open();

    // Draw current menu state
    void draw();

    // Short press: advance cursor
    void scrollDown();

    // ── Touch-only additions ──────────────────────────────────────────────
    // Guarded, and not merely left uncalled on the other boards. An unreferenced
    // method IS dropped by --gc-sections, but dropping it did not leave the
    // image byte-identical: the six non-touch boards' firmware grew by 24 bytes
    // of literal-pool padding with an otherwise identical symbol table. Adding a
    // board must not move a byte of another board's build, so the guard is the
    // only version of this that can be verified.
#if HEXHOUND_HAS_TOUCH
    // There WAS a scrollUp() here that retreated the cursor one row, wrapping,
    // as the mirror of the swipe-down gesture. Paging replaced both directions,
    // so nothing called it any more. Removed rather than left in place: an
    // unreferenced capability is the exact defect this project has already paid
    // for twice, with COSMETIC_WORN and then again with the flourish enum.

    // Move the WINDOW a whole page, not the cursor one row.
    //
    // This is what a swipe means. scrollDown() steps the cursor by one and
    // lets the window follow, which is correct for a single button -
    // the only way it can reach row 9 is to pass through rows 1 to 8. A finger
    // has no such constraint, and stepping one row per swipe made a 14-entry
    // menu take thirteen swipes to cross. The reported complaint, in one line:
    // "as I scroll it should move the entire menu up or down, not the cursor
    // one item at a time, then when I see what I want I can select it."
    //
    // So these page the view and then put the cursor INSIDE the new window
    // rather than leaving it behind. Leaving it behind would be the literal
    // reading of the request, but it strands the highlight off-screen and the
    // next button press would snap the view back to wherever the cursor was
    // parked - the board still has a button and it still has to work.
    //
    // Deliberately do NOT wrap. scrollDown() wraps because a one-button device
    // needs a way back to the top; a viewport that jumps from the end to the
    // beginning under a finger just reads as having lost your place. Swipe the
    // other way instead.
    void pageDown();
    void pageUp();

    // Jump the cursor straight to a row index in the VISIBLE list. This is the
    // one thing touch can do that a single button cannot: a tap already names
    // the row, so scrolling to it N times is pure ceremony, which is exactly
    // the complaint this feature exists to answer.
    //
    // Does NOT redraw. The tap path selects the row immediately and the menu is
    // about to be replaced, so a repaint in between is a wasted full-screen
    // blit and a visible flicker on the 480x480 panel. Callers that stay on the
    // menu (the paging gestures) draw for themselves.
    void setCursor(int index);

    // Which VISIBLE-list index a touch at screen y landed on, or -1 for the
    // dead space above or below the drawn rows.
    //
    // Answered from the geometry the LAST draw() actually used, recorded by the
    // draw itself, rather than recomputed here. Recomputing would be a second
    // copy of the row layout free to drift from the first, which is the same
    // class of bug as a hitbox drifting from its sprite - and it would drift
    // silently, because a menu that opens the wrong entry still looks like a
    // menu that works.
    int rowIndexAtY(int y) const;
#endif // HEXHOUND_HAS_TOUCH

    // Long press: select highlighted item.
    // The cursor indexes the VISIBLE list, which is not the enum order, so
    // this maps through it rather than casting the cursor.
    MenuItem select() const;

    // Whether the row under the cursor is shown but not yet earned. Callers
    // must ask this before acting on select(): a locked row explains what
    // would open it rather than opening anything.
    bool selectedIsLocked() const;

    int cursor() const { return _cursor; }

    // ── Pet Stats sub-screen ──────────────────────────────────────────────
    void drawStats();

private:
    UIMenu() = default;
    void drawStatBar(int x, int y, int w, int h, int val, const char* label);
#if HEXHOUND_PANEL_ROUND
    void drawRound();
    void drawStatsRound();
    // Chord-centered label / bar / value row for the round stats screen.
    void drawStatRowRound(int y, int val, const char* label);
#endif

    // Rows that fit on this panel between the header and the footer rule.
    // The menu used to draw every visible entry regardless, which worked only
    // while the list happened to be short enough. It was already one entry
    // over on the 160x80 panel (FLIP DISPLAY drew on top of the footer) and
    // Phase 1 adds four more, so the list now scrolls.
    static int rowsPerPage();

    // Keeps _scroll such that _cursor is on screen.
    void ensureCursorVisible();

#if HEXHOUND_HAS_TOUCH
    // Shared body of pageDown()/pageUp(), in PAGES so the sign is the only
    // difference between them and the clamping exists once.
    void pageBy(int deltaPages);
#endif

    TFT_eSPI* _tft = nullptr;
    int _cursor = 0;
    int _scroll = 0;   // index of the first drawn row

#if HEXHOUND_HAS_TOUCH
    // Row geometry as last drawn, for rowIndexAtY(). Recorded by draw() at the
    // one point that knows it. Compiled out entirely on the six boards with no
    // touch panel, so their menu object and their draw path are unchanged.
    int _rowY0    = 0;
    int _rowH     = 0;
    int _rowCount = 0;   // rows actually on screen
    int _rowFirst = 0;   // visible-list index of the topmost drawn row
#endif
};

