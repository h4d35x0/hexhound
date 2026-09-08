#pragma once

#include "game_layout.h"

// ── HexHound - Minigame Widgets ─────────────────────────────────
//
// Two pieces of screen furniture that more than one game needs, kept here so
// they exist once rather than once per game.
//
//  * ChipRow  - the one-button selector. A row of labelled chips with a cursor
//               that the game steps at its own cadence; a short press commits
//               whichever chip the cursor is on. This is the same trick
//               signal_memory.cpp uses to turn "which one" into "when", which
//               is the only way a single button can express a choice.
//  * DrainBar - a horizontal bar that empties over a deadline, repainted by
//               delta rather than cleared and redrawn.
//
// Both take their geometry from a games::Rect the caller derives from
// games::playArea(), so neither knows a panel dimension and neither can put a
// pixel under the round board's bezel.
//
// Header-only, and nothing here allocates: every widget is a fixed-size member
// of the game that owns it. A game may therefore call any of this from
// update() without touching the heap.
//
// Nothing here ever clears a whole panel or a whole play area either. A chip
// repaints its own rectangle, and the bar repaints only the pixels that
// changed, because a full-area fill per frame is visible as a strobe on the
// 160x80 panel.

namespace games {

enum ChipState : uint8_t {
    CHIP_IDLE = 0,
    CHIP_CURSOR,     // the cursor is sitting here; a press would commit it
    CHIP_GOOD,       // committed and correct
    CHIP_BAD,        // committed and wrong
    CHIP_UNDRAWN = 0xFF
};

class ChipRow {
public:
    // Three is what the two games that use this need today. Raising it costs
    // about 22 bytes of .bss per slot per game, which on the no-PSRAM board is
    // worth spending deliberately rather than by default.
    static const int MAX_CHIPS = 3;

    ChipRow() : _n(0), _cursor(0), _labelSize(1) {
        for (int i = 0; i < MAX_CHIPS; i++) {
            _labels[i] = "";
            _colors[i] = TFT_CYAN;
            _state[i]  = CHIP_UNDRAWN;
        }
    }

    // Lay n chips evenly across `area`. maxLabelChars is the longest label the
    // caller will ever set, and it picks the text size: sizing to the longest
    // string up front means a later label can never overflow its chip.
    void layout(const Rect& area, int n, int maxLabelChars) {
        if (n < 1) n = 1;
        if (n > MAX_CHIPS) n = MAX_CHIPS;
        _n = (uint8_t)n;

        const int gap = panel() == PANEL_COMPACT ? 3 : 5;
        int w = (area.w - gap * (n + 1)) / n;
        if (w < 4) w = 4;

        for (int i = 0; i < n; i++) {
            _r[i] = Rect(area.x + gap + i * (w + gap), area.y, w, area.h);
            _state[i] = CHIP_UNDRAWN;
        }

        // realTextSize(1) is what one 240-relative size unit is worth in REAL
        // pixels: 1 on every rectangular panel and on the 240 round, 2 on the
        // 480. w and area.h are real pixels, so the fitting divisor has to
        // carry it. Without that this assumed a 6x8 glyph and picked a size
        // whose glyphs were 12x16, and the guarantee promised above - "sizing
        // to the longest string up front means a later label can never overflow
        // its chip" - was simply false on the 480 panel. Firewall's ALLOW and
        // BLOCK came out 180 px wide and 48 px tall inside a 175x34 chip: both
        // chip borders disappeared under their own labels and the two words ran
        // together into "ALLOWBLOCK".
        //
        // Only the 480 round panel moves. Recomputed for all four panels, the
        // size this picks is unchanged on 240 round, 320x172 and 160x80,
        // because realTextSize(1) is 1 there and the expression reduces to the
        // old one exactly.
        const int byW = maxLabelChars > 0
                      ? (w - 4) / (maxLabelChars * 6 * realTextSize(1)) : 1;
        const int byH = (area.h - 4) / lineHeight(1);
        _labelSize = (uint8_t)clampInt(byW < byH ? byW : byH, 1, 3);
        _cursor    = 0;
    }

    // The label pointer is stored, not copied. Callers pass either a literal or
    // a buffer that outlives the round; nothing here allocates or frees.
    void setLabel(int i, const char* s, uint16_t color) {
        if (i < 0 || i >= _n) return;
        _labels[i] = s ? s : "";
        _colors[i] = color;
    }

    int  count() const  { return _n; }
    int  cursor() const { return _cursor; }
    uint8_t state(int i) const {
        return (i < 0 || i >= _n) ? (uint8_t)CHIP_UNDRAWN : _state[i];
    }
    const Rect& rect(int i) const {
        return _r[(i < 0 || i >= _n) ? 0 : i];
    }

#if HEXHOUND_HAS_TOUCH
    // Which chip contains this point, or -1.
    //
    // The whole ChipRow is a ONE-BUTTON selector: a cursor sweeps the row and
    // a press commits whatever it is sitting on. That is the right shape for a
    // device with one button and the wrong shape for a finger, which already
    // knows which chip it wants. With the dwell down to 400 ms and the FT3267
    // needing a retry on the first read of a contact, committing the chip the
    // sweep happens to be on is a reflex test: reported from the board as
    // pressing ALLOW and being told OVERBLOCK.
    //
    // The row is padded outward by half the inter-chip gap so the dead strip
    // between two chips belongs to the nearer one. A tap that lands in a gap
    // is a tap the player meant for something.
    int chipAtXY(int x, int y) const {
        for (int i = 0; i < _n; i++) {
            const Rect& r = _r[i];
            const int pad = (panel() == PANEL_COMPACT ? 3 : 5) / 2;
            if (x >= r.x - pad && x < r.x + r.w + pad &&
                y >= r.y - pad && y < r.y + r.h + pad) {
                return i;
            }
        }
        return -1;
    }
#endif

    // Call after something else cleared the area these chips live in, so the
    // next set() repaints instead of believing the chips are still on screen.
    void forget() {
        for (int i = 0; i < MAX_CHIPS; i++) _state[i] = CHIP_UNDRAWN;
    }

    void draw(TFT_eSPI& tft, int i, uint8_t st) {
        if (i < 0 || i >= _n) return;
        const Rect& r = _r[i];
        const uint16_t c = _colors[i];

        uint16_t fill   = TFT_BLACK;
        uint16_t border = dimColor(c, 1);
        uint16_t label  = dimColor(c, 2);

        switch (st) {
        case CHIP_CURSOR:
            border = c;         label = c;          break;
        case CHIP_GOOD:
            fill = TFT_GREEN;   border = TFT_GREEN; label = TFT_BLACK; break;
        case CHIP_BAD:
            fill = TFT_RED;     border = TFT_RED;   label = TFT_BLACK; break;
        default:
            break;
        }

        tft.fillRect(r.x, r.y, r.w, r.h, fill);
        tft.drawRect(r.x, r.y, r.w, r.h, border);
        if (st == CHIP_CURSOR && r.w > 2 && r.h > 2) {
            tft.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, border);
        }

        const char* s = _labels[i];
        const int lw = textWidth(s, _labelSize);
        const int lh = games::lineHeight(_labelSize);
        tft.setTextSize(games::realTextSize(_labelSize));
        tft.setTextColor(label, fill);
        tft.setCursor(r.x + (r.w - lw) / 2, r.y + (r.h - lh) / 2);
        tft.print(s);

        _state[i] = st;
    }

    void set(TFT_eSPI& tft, int i, uint8_t st) {
        if (i < 0 || i >= _n) return;
        if (_state[i] == st) return;
        draw(tft, i, st);
    }

    void drawAll(TFT_eSPI& tft, uint8_t st) {
        for (int i = 0; i < _n; i++) set(tft, i, st);
    }

    // Move the cursor, repainting only the chip it left and the one it reached.
    void moveCursor(TFT_eSPI& tft, int i) {
        if (i < 0 || i >= _n) return;
        if (i != _cursor && _state[_cursor] == CHIP_CURSOR) {
            set(tft, _cursor, CHIP_IDLE);
        }
        _cursor = (uint8_t)i;
        set(tft, i, CHIP_CURSOR);
    }

    void step(TFT_eSPI& tft) {
        moveCursor(tft, (_cursor + 1) % (_n > 0 ? _n : 1));
    }

private:
    Rect        _r[MAX_CHIPS];
    const char* _labels[MAX_CHIPS];
    uint16_t    _colors[MAX_CHIPS];
    uint8_t     _state[MAX_CHIPS];
    uint8_t     _n;
    uint8_t     _cursor;
    uint8_t     _labelSize;
};

// ── DrainBar ──────────────────────────────────────────────────────────────
//
// A deadline the player can see. A one-button game that judges you on a clock
// you cannot read is a coin flip, so every timed decision in these games has
// one of these above it.
//
// Repaints by delta: the pixels that just expired are blacked out and nothing
// else is touched, so a bar that updates thirty times a second costs two small
// fills per frame instead of a clear plus a redraw.

class DrainBar {
public:
    DrainBar() : _x(0), _y(0), _w(0), _h(0), _drawn(-1), _color(0) {}

    void layout(int x, int y, int w, int h) {
        _x = x; _y = y; _w = w; _h = h;
        _drawn = -1;
        _color = 0;
    }

    // Forget what is on screen without drawing, for when the caller has just
    // cleared the region this bar lives in.
    void forget() { _drawn = -1; _color = 0; }

    void draw(TFT_eSPI& tft, uint32_t elapsed, uint32_t total, uint16_t color) {
        if (_w <= 0 || _h <= 0 || total == 0) return;
        if (elapsed > total) elapsed = total;

        int w = _w - (int)((uint64_t)_w * elapsed / total);
        if (w < 0) w = 0;

        // A colour change has to repaint the part that is already lit, so force
        // a full redraw of the remaining span rather than only the delta.
        const bool recolor = (color != _color);
        if (w == _drawn && !recolor) return;

        if (_drawn > w) {
            tft.fillRect(_x + w, _y, _drawn - w, _h, TFT_BLACK);
        }
        if (w > 0) {
            const int from = (recolor || _drawn < 0) ? 0 : _drawn;
            if (w > from) tft.fillRect(_x + from, _y, w - from, _h, color);
        }
        _drawn = w;
        _color = color;
    }

    void clear(TFT_eSPI& tft) {
        if (_drawn > 0) tft.fillRect(_x, _y, _w, _h, TFT_BLACK);
        _drawn = -1;
        _color = 0;
    }

private:
    int      _x, _y, _w, _h;
    int      _drawn;     // pixels currently lit, -1 when nothing is on screen
    uint16_t _color;
};

}  // namespace games
