#include "ui_games.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../games/game_registry.h"
#include "../content/minigame.h"
#include "../config.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Minigame Select Implementation ──────────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan
#define COL_CURSOR_BG 0x2945  // dark blue
#define COL_DIM       0xFFFF  // white
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white
#define COL_EMPTY     0xC618  // light grey - empty state

// Longest name any panel here can show. Minigame::name() is documented as
// "keep it short: 160x80", but the screen must survive one that is not.
#define GAME_NAME_MAX 32

UIGames& UIGames::instance() {
    static UIGames ui;
    return ui;
}

void UIGames::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIGames::open() {
    _cursor = 0;
    _scroll = 0;
    draw();
}

int UIGames::totalEntries() const {
    return games::playableCount() + 1;  // BACK + playable games
}

Minigame* UIGames::selectedGame() const {
    if (backSelected()) return nullptr;
    return games::playableAt(selectedIndex());
}

const char* UIGames::actionHint() const {
    return backSelected() ? "hold=back" : "hold=play";
}

void UIGames::draw() {
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;

    const int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);
    drawHeader();

    // Panel-family geometry, same derivation as ui_quests.cpp.
    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  charW = big ? uilg::CHAR_W : 6;
    const int  charH = big ? uilg::CHAR_H : 8;
    const int  rowH  = big ? 22 : 13;
    const int  bodyY = big ? uilg::BODY_Y : 14;
    const int  padX  = big ? uilg::PAD : 4;

    const int textOff = (rowH - charH) / 2;
    int visible = min(GAMES_PER_PAGE, total - _scroll);
    if (visible < 0) visible = 0;
#if HEXHOUND_HAS_TOUCH
    // Recorded here, where the numbers are known, so a tap is hit-tested
    // against the rows the owner can actually SEE. See rowIndexAtY().
    _rows.note(bodyY, rowH, visible, _scroll);
#endif

    for (int i = 0; i < visible; i++) {
        const int idx = _scroll + i;
        const int y   = bodyY + i * rowH;
        const bool sel = (idx == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;

        _tft->setTextSize(ts);
        if (sel) {
            _tft->fillRect(0, y, SCREEN_W, rowH, COL_CURSOR_BG);
            _tft->setTextColor(COL_CURSOR, bg);
            _tft->setCursor(padX, y + textOff);
            _tft->print("> ");
        }

        const int x = padX + 2 * charW;
        _tft->setTextColor(sel ? COL_CURSOR : COL_ITEM, bg);
        _tft->setCursor(x, y + textOff);

        if (idx == 0) {
            _tft->print("BACK");
            continue;
        }

        Minigame* g = games::playableAt(idx - 1);
        if (!g) continue;   // list shifted under us; leave the row blank

        char buf[GAME_NAME_MAX];
        snprintf(buf, sizeof(buf), "%s", g->name());
        _tft->print(buf);
    }

    // A board where every game is capability-gated out is a legitimate board,
    // not a fault. Say so rather than showing a bare BACK row.
    if (games::playableCount() == 0) {
        const char* msg = "No games here";
        const int w = (int)strlen(msg) * charW;
        _tft->setTextSize(ts);
        _tft->setTextColor(COL_EMPTY, TFT_BLACK);
        _tft->setCursor((SCREEN_W - w) / 2, bodyY + rowH + textOff);
        _tft->print(msg);
    }

    drawFooter();
#endif // !HEXHOUND_PANEL_ROUND
}

#if !HEXHOUND_PANEL_ROUND

void UIGames::drawHeader() {
    const bool big = (SCREEN_H > 100);
    const int total = totalEntries();

    if (big) {
        uiBigHeader(*_tft, "\x04 GAMES", COL_HEADER);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(COL_HEADER, TFT_BLACK);
        _tft->setCursor(4, 2);
        _tft->print("\x04 GAMES");
        _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
    }

    if (total > GAMES_PER_PAGE) {
        const int page = (_scroll / GAMES_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / GAMES_PER_PAGE) + 1;
        char pgBuf[8];
        snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
        const int charW = big ? uilg::CHAR_W : 6;
        const int pgW = (int)strlen(pgBuf) * charW;
        _tft->setTextSize(big ? 2 : 1);
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(SCREEN_W - pgW - (big ? uilg::PAD : 4),
                        big ? uilg::TITLE_Y : 2);
        _tft->print(pgBuf);
    }
}

void UIGames::drawFooter() {
    const char* hint = actionHint();

    if (SCREEN_H > 100) {
        uiBigFooter(*_tft, "scroll", hint, COL_DIVIDER);
        return;
    }

    const int footerY = SCREEN_H - 10;
    _tft->fillRect(0, footerY - 2, SCREEN_W, SCREEN_H - footerY + 2, TFT_BLACK);
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print("scroll");
    _tft->setCursor(SCREEN_W - (int)strlen(hint) * 6 - 4, footerY);
    _tft->print(hint);
}

#endif // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ── Round game select (240x240) ───────────────────────────────────────────
// Centered size-2 rows with chord-clipped selection capsules, matching the
// round main menu. Game names are short enough for size 2 at these chords;
// the per-row chord clip below still handles one that is not.

void UIGames::drawHeader() {}
void UIGames::drawFooter() {}

void UIGames::drawRound() {
    if (!_tft) return;

    const int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "GAMES", COL_HEADER);

    if (total > GAMES_PER_PAGE) {
        const int page = (_scroll / GAMES_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / GAMES_PER_PAGE) + 1;
        uiround::centerPrintf(*_tft, uiround::BODY_Y - uiround::scaled(8), 1, COL_DIM,
                              TFT_BLACK, "%d/%d", page, totalPages);
    }

    const int avail = uiround::FOOTER_DIV - uiround::BODY_Y;   // 132
    int visible = min(GAMES_PER_PAGE, total - _scroll);
    if (visible < 1) visible = 1;

    int rowH = avail / visible;
    if (rowH > uiround::scaled(30)) rowH = uiround::scaled(30);

    int textOff = (rowH - uiround::textH(2)) / 2;
    if (textOff < 0) textOff = 0;

    int y = uiround::BODY_Y + (avail - rowH * visible) / 2;

#if HEXHOUND_HAS_TOUCH
    {
        // `visible` was clamped UP to 1 above so an EMPTY list still paints its
        // empty-state row. That row is not a list entry and must not be
        // selectable, so the hit box is told how many rows are really there.
        int real = total - _scroll;
        if (real < 0) real = 0;
        if (real > visible) real = visible;
        _rows.note(y, rowH, real, _scroll);
    }
#endif

    for (int i = 0; i < visible; i++) {
        const int idx = _scroll + i;
        const bool sel = (idx == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;

        if (sel) uiround::rowFill(*_tft, y, rowH, COL_CURSOR_BG, 4);

        const char* label = "BACK";
        char buf[GAME_NAME_MAX];
        if (idx != 0) {
            Minigame* g = games::playableAt(idx - 1);
            if (!g) { y += rowH; continue; }
            // Clip to the chord at this row so a long name cannot run under
            // the bezel. Size-2 glyphs are CHAR_W * 2 wide.
            const int maxChars =
                (uiround::chordHalfW(y + textOff) * 2 - 12) /
                (uiround::cellW(2));
            int n = maxChars;
            if (n < 1) n = 1;
            if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
            snprintf(buf, (size_t)n + 1, "%s", g->name());
            label = buf;
        }

        uiround::centerText(*_tft, y + textOff, label, 2,
                            sel ? COL_CURSOR : COL_ITEM, bg);
        y += rowH;
    }

    if (games::playableCount() == 0) {
        uiround::centerText(*_tft, uiround::CY + uiround::scaled(40), "No games here", 1,
                            COL_EMPTY, TFT_BLACK);
    }

    char hintBuf[40];
    snprintf(hintBuf, sizeof(hintBuf), "press=scroll  %s", actionHint());
    uiround::footer(*_tft, hintBuf, COL_DIVIDER);
}

#endif // HEXHOUND_PANEL_ROUND

#if HEXHOUND_HAS_TOUCH
void UIGames::setCursor(int index) {
    const int total = totalEntries();
    if (total <= 0) return;
    if (index < 0 || index >= total) return;

    _cursor = index;

    // Same clamp scrollDown() uses. A tapped row is visible by construction, so
    // this is a no-op today; it is here so the two ways of moving the cursor
    // cannot disagree about what _scroll means if either list ever grows a
    // second entry point.
    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + GAMES_PER_PAGE) {
        _scroll = _cursor - GAMES_PER_PAGE + 1;
    }
}
#endif

#if HEXHOUND_HAS_TOUCH
// See uiPageBy() in touch_nav.h. Guarded rather than left uncalled: an
// unreferenced method IS dropped by --gc-sections, but dropping it did not
// leave the non-touch images byte-identical when this was measured on the
// menu, so the guard is the only version that can be verified.
void UIGames::pageDown() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), GAMES_PER_PAGE, 1)) draw();
}

void UIGames::pageUp() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), GAMES_PER_PAGE, -1)) draw();
}
#endif

void UIGames::scrollDown() {
    const int total = totalEntries();
    if (total <= 0) return;

    _cursor = (_cursor + 1) % total;

    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + GAMES_PER_PAGE) {
        _scroll = _cursor - GAMES_PER_PAGE + 1;
    }

    draw();
}
