#include "ui_round.h"

#if HEXHOUND_PANEL_ROUND

#include "ui_utils.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ── HexHound - Round Panel Primitives ───────────────────────────
//
// Chord budget for the vertical rhythm in ui_round.h, tabulated for the
// 240x240 panel (safe radius 114) that this family was authored against.
//
// The 480x480 T-RGB scales every row and every text size by exactly 2 (see
// scaled() and TEXT_SCALE in ui_round.h), so the geometry is similar and this
// table still describes it: double the y, dy, half-width and usable width
// columns, and the character counts hold unchanged because the text scales
// with the panel.
//
//   row y    dy     chord half-width   usable width   size-2 chars
//   ------   ----   ----------------   ------------   ------------
//     26      -94         64               128             10
//     48      -72         88               176             14
//     58      -62         95               190             15
//    120        0        114               228             19
//    172       +52       101               202             16
//    190       +70        90               180             15
//    198       +78        83               166             13  (27 at size 1)
//    214       +94        64               128             10
//
// Anything drawn outside that envelope is behind the bezel and invisible on
// real glass. The simulator masks the same circle so it cannot lie about it.

namespace uiround {

namespace {

inline int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

int chordHalfW(int y, int radius) {
    int dy = y - CY;
    if (dy < 0) dy = -dy;
    if (dy >= radius) return 0;
    return (int)sqrtf((float)(radius * radius - dy * dy));
}

int rowLeft(int y, int radius) {
    return CX - chordHalfW(y, radius);
}

int rowRight(int y, int radius) {
    return CX + chordHalfW(y, radius);
}

// `size` here and in every other helper below is 240-RELATIVE: the scale for
// the actual panel is applied inside, so callers never have to know which round
// panel they are drawing on. On the 240 panel ts() is the identity, which is
// why this generalisation leaves that board's binary byte-identical.
int textW(const char* s, int size) {
    if (!s) return 0;
    return (int)strlen(s) * cellW(size);
}

bool textFits(int y, int pxWidth, int size) {
    // The narrowest row a glyph block occupies is whichever of its top or
    // bottom edge is further from the vertical center.
    int top = y;
    int bottom = y + textH(size) - 1;
    int hwTop = chordHalfW(top);
    int hwBottom = chordHalfW(bottom);
    int hw = hwTop < hwBottom ? hwTop : hwBottom;
    return pxWidth <= hw * 2;
}

void hLine(TFT_eSPI& tft, int y, uint16_t color, int inset) {
    int hw = chordHalfW(y) - inset;
    if (hw <= 0) return;
    tft.drawFastHLine(CX - hw, y, hw * 2, color);
}

void rowFill(TFT_eSPI& tft, int y, int h, uint16_t color, int inset) {
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= DIAM) continue;
        int hw = chordHalfW(row) - inset;
        if (hw <= 0) continue;
        tft.drawFastHLine(CX - hw, row, hw * 2, color);
    }
}

int centerText(TFT_eSPI& tft, int y, const char* s, int size,
               uint16_t fg, uint16_t bg) {
    if (!s || !*s) return CX;
    int w = textW(s, size);
    int x = CX - w / 2;
    if (x < 0) x = 0;
    tft.setTextSize(ts(size));
    tft.setTextColor(fg, bg);
    tft.setCursor(x, y);
    tft.print(s);
    return x;
}

int centerPrintf(TFT_eSPI& tft, int y, int size, uint16_t fg, uint16_t bg,
                 const char* fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return centerText(tft, y, buf, size, fg, bg);
}

void leftText(TFT_eSPI& tft, int y, const char* s, int size,
              uint16_t fg, uint16_t bg, int inset) {
    if (!s || !*s) return;
    // Align to the narrowest row the text occupies so no glyph pokes out.
    int hwTop = chordHalfW(y);
    int hwBottom = chordHalfW(y + textH(size) - 1);
    int hw = hwTop < hwBottom ? hwTop : hwBottom;
    tft.setTextSize(ts(size));
    tft.setTextColor(fg, bg);
    tft.setCursor(CX - hw + inset, y);
    tft.print(s);
}

void rightText(TFT_eSPI& tft, int y, const char* s, int size,
               uint16_t fg, uint16_t bg, int inset) {
    if (!s || !*s) return;
    int hwTop = chordHalfW(y);
    int hwBottom = chordHalfW(y + textH(size) - 1);
    int hw = hwTop < hwBottom ? hwTop : hwBottom;
    tft.setTextSize(ts(size));
    tft.setTextColor(fg, bg);
    tft.setCursor(CX + hw - inset - textW(s, size), y);
    tft.print(s);
}

int textBlock(TFT_eSPI& tft, int y, int maxY, const char* text, int size,
              uint16_t fg, uint16_t bg, int inset) {
    if (!text || !*text) return y;

    const int lineH = textH(size) + scaled(2);
    const int glyphW = cellW(size);
    char line[48];

    const char* p = text;
    while (*p && y + textH(size) <= maxY) {
        // Columns available on this row, measured at whichever of the line's
        // top or bottom edge is nearer the rim.
        int hwTop = chordHalfW(y);
        int hwBot = chordHalfW(y + textH(size) - 1);
        int hw = hwTop < hwBot ? hwTop : hwBot;
        int usable = hw * 2 - inset * 2;
        int cols = usable / glyphW;
        if (cols < 1) { y += lineH; continue; }
        if (cols > (int)sizeof(line) - 1) cols = (int)sizeof(line) - 1;

        // Skip leading spaces, honour explicit newlines.
        while (*p == ' ') p++;
        if (*p == '\n') { p++; y += lineH; continue; }
        if (!*p) break;

        // Greedy word wrap: take up to `cols` chars, then back off to the last
        // space so words are not split.
        int take = 0;
        int lastSpace = -1;
        while (p[take] && take < cols && p[take] != '\n') {
            if (p[take] == ' ') lastSpace = take;
            take++;
        }
        if (p[take] && p[take] != '\n' && p[take] != ' ' && lastSpace > 0) {
            take = lastSpace;
        }
        if (take <= 0) take = 1;

        memcpy(line, p, (size_t)take);
        line[take] = '\0';
        centerText(tft, y, line, size, fg, bg);

        p += take;
        if (*p == '\n') p++;
        y += lineH;
    }
    return y;
}

void header(TFT_eSPI& tft, const char* title, uint16_t color) {
    // Drop to size 1 if the title is too long for the chord at the title row
    // rather than letting it run under the bezel.
    int size = textFits(TITLE_Y, textW(title, 2), 2) ? 2 : 1;
    int y = (size == 2) ? TITLE_Y : TITLE_Y + scaled(4);

    // CLEAR THE WHOLE BAND, not just behind the glyphs.
    //
    // centerText() paints a background only under the characters it draws, so
    // a shorter title left the tail of a longer one standing beside it, and
    // anything drawn in the gap between the title and the rule survived
    // forever. Found by Lane C while chasing residue on the game screens: the
    // bottom of the home screen's yellow subtitle was still on the glass under
    // the header rule of a game, rows 92..95 at 480.
    //
    // Fixed here rather than in the games, because every screen that calls
    // header() had it. A screen that does a full fillScreen() first pays two
    // extra fills of a thin band and notices nothing.
    tft.fillRect(0, 0, DIAM, HEADER_DIV, TFT_BLACK);

    centerText(tft, y, title, size, color, TFT_BLACK);
    hLine(tft, HEADER_DIV, color, scaled(8));
}

void footer(TFT_eSPI& tft, const char* hint, uint16_t color) {
    hLine(tft, FOOTER_DIV, color, scaled(8));
    if (hint && *hint) {
        centerText(tft, FOOTER_Y, hint, 1, color, TFT_BLACK);
    }
}

void arc(TFT_eSPI& tft, float a0, float a1, int radius, int thickness,
         uint16_t color) {
    // Rasterised as an annulus scan rather than as a fan of radial lines.
    // The radial-line approach leaves visible notches: polarToXY() truncates
    // toward zero, so endpoints jitter by a pixel and asymmetrically between
    // the positive and negative quadrants. Testing each candidate pixel for
    // "inside the ring AND inside the angular sweep" gives a clean edge, and
    // only the annulus band is visited so the cost stays low (a full 5px-thick
    // ring at r=117 is about 2.3k pixels, not 57.6k).
    if (thickness < 1) thickness = 1;
    float span = a1 - a0;
    if (span <= 0.0f) return;
    if (span > 360.0f) span = 360.0f;

    float start = fmodf(a0, 360.0f);
    if (start < 0.0f) start += 360.0f;

    const int rOut = radius;
    const int rIn  = (radius - thickness) < 0 ? 0 : (radius - thickness);
    const int rOut2 = rOut * rOut;
    const int rIn2  = rIn * rIn;

    tft.startWrite();
    for (int y = CY - rOut; y <= CY + rOut; y++) {
        if (y < 0 || y >= DIAM) continue;
        const int dy = y - CY;
        const int dy2 = dy * dy;
        if (dy2 > rOut2) continue;

        const int xOut = (int)sqrtf((float)(rOut2 - dy2));
        const int xIn  = (dy2 < rIn2) ? (int)sqrtf((float)(rIn2 - dy2)) : -1;

        // The row crosses the annulus in one band (rows past the inner circle)
        // or two (rows that also cross the hole).
        int bands[2][2];
        int bandCount;
        if (xIn >= 0) {
            bands[0][0] = CX - xOut; bands[0][1] = CX - xIn;
            bands[1][0] = CX + xIn;  bands[1][1] = CX + xOut;
            bandCount = 2;
        } else {
            bands[0][0] = CX - xOut; bands[0][1] = CX + xOut;
            bandCount = 1;
        }

        for (int b = 0; b < bandCount; b++) {
            for (int x = bands[b][0]; x <= bands[b][1]; x++) {
                if (x < 0 || x >= DIAM) continue;
                const int dx = x - CX;
                float ang = atan2f((float)dy, (float)dx) * 180.0f / 3.14159265f;
                if (ang < 0.0f) ang += 360.0f;
                float rel = ang - start;
                if (rel < 0.0f) rel += 360.0f;
                if (rel <= span) {
                    tft.drawPixel(x, y, color);
                }
            }
        }
    }
    tft.endWrite();
}

void arcGauge(TFT_eSPI& tft, float a0, float a1, int pct, int radius,
              int thickness, uint16_t color, uint16_t bgColor) {
    pct = clampInt(pct, 0, 100);
    arc(tft, a0, a1, radius, thickness, bgColor);
    if (pct > 0) {
        float filled = a0 + (a1 - a0) * (float)pct / 100.0f;
        arc(tft, a0, filled, radius, thickness, color);
    }
}

void progressRing(TFT_eSPI& tft, int pct, uint16_t color, uint16_t bgColor) {
    // Starts at 12 o'clock (-90 degrees) and runs clockwise through a full
    // turn, so a full ring reads as "ready to evolve".
    arcGauge(tft, -90.0f, 270.0f, pct, RING_R, RING_TH, color, bgColor);
}

} // namespace uiround

#endif // HEXHOUND_PANEL_ROUND
