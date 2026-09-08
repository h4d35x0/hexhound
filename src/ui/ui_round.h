#pragma once

#include "../board/board_profile.h"

// ── HexHound - Round Panel Layout Family ────────────────────────
//
// The third layout family, alongside the 160x80 compact layouts and the
// 320x172 "big screen" layouts in ui_utils.h. It exists for panels whose glass
// is a circle inscribed in a square framebuffer (Waveshare ESP32-S3-LCD-1.28,
// GC9A01A 240x240), where every corner pixel sits behind the bezel.
//
// This entire header and its .cpp compile to nothing unless the selected board
// sets HEXHOUND_PANEL_ROUND, so adding the round board changes no byte of the
// firmware for any other board.
//
// The core idea: on a rectangular panel horizontal extents come from SCREEN_W,
// which is constant for every row. On a circle the usable width is a function
// of the row - the chord. Every primitive here derives its extent from
// chordHalfW(y) instead, so nothing is ever drawn under the bezel.

#if HEXHOUND_PANEL_ROUND

#include "../hal/tft_compat.h"

namespace uiround {

// ── Geometry ──────────────────────────────────────────────────────────────

constexpr int DIAM   = SCREEN_W;        // 240 or 480 - the panel is square
constexpr int R      = DIAM / 2;        // glass radius
constexpr int CX     = R;
constexpr int CY     = R;

// This family now serves two panels: the 240x240 Waveshare 1.28 it was authored
// against, and the 480x480 LilyGo T-RGB. Rather than fork the file - which
// would have duplicated the chord math, the hard part, and let the two copies
// drift - every hand-tuned constant below is expressed as a fraction of DIAM.
//
// scaled() reproduces the original 240 value EXACTLY when DIAM is 240, and each
// constant carries a static_assert saying so. Those asserts are the point: the
// 1.28 board ships, and a generalisation that shifted its layout by a pixel
// would be a regression nobody would catch until it was on glass.
constexpr int scaled(int at240) { return at240 * DIAM / 240; }

static_assert(DIAM == 240 || DIAM == 480,
              "round layout family is tuned for 240 and 480 only; a new round "
              "panel needs its rhythm checked against the chord, not assumed");

// Keep content off the very edge of the glass: the outermost few pixels of a
// round panel are optically distorted by the bezel and hard to read.
constexpr int BEZEL  = scaled(6);
constexpr int SAFE_R = R - BEZEL;
static_assert(DIAM != 240 || (BEZEL == 6 && SAFE_R == 114), "240 bezel shifted");

// GLCD font metrics. These are properties of the FONT, not of the panel, so
// they must NOT scale - a bigger panel gets bigger text by raising the text
// SIZE multiplier, which is what TEXT_SCALE below is for.
constexpr int CHAR_W = 6;               // per size unit
constexpr int CHAR_H = 8;               // per size unit

// Text size multiplier for this panel: 1 at 240, 2 at 480. A "size 2" title on
// the small round panel becomes size 4 on the big one and occupies the same
// fraction of the glass.
constexpr int TEXT_SCALE = DIAM / 240;
constexpr int ts(int size240) { return size240 * TEXT_SCALE; }
static_assert(DIAM != 240 || TEXT_SCALE == 1, "240 text scale shifted");

// CONVENTION: every text `size` argument in this namespace is 240-RELATIVE.
// The helpers multiply by TEXT_SCALE themselves, so a caller asking for size 2
// gets size 2 on the small panel and size 4 on the big one without knowing
// which panel it is on. That is why ~25 call sites across the round screens
// did not have to change, and why they must NOT be "fixed" to pass ts(2).
//
// Use these two instead of `CHAR_H * size` / `CHAR_W * size` when measuring a
// text cell, because those raw expressions miss the scale.
constexpr int textH(int size240) { return CHAR_H * ts(size240); }
constexpr int cellW(int size240) { return CHAR_W * ts(size240); }
static_assert(DIAM != 240 || (textH(2) == 16 && cellW(2) == 12),
              "240 text cell metrics shifted");

// Vertical rhythm. Rows are chosen so the chord is wide enough for the text
// each one carries - see the table in ui_round.cpp.
constexpr int TITLE_Y    = scaled(26);  // title row
constexpr int HEADER_DIV = scaled(48);  // rule under the title
constexpr int BODY_Y     = scaled(58);  // first body row
constexpr int FOOTER_DIV = scaled(190); // rule above the footer
constexpr int FOOTER_Y   = scaled(198); // footer hint
static_assert(DIAM != 240 ||
                  (TITLE_Y == 26 && HEADER_DIV == 48 && BODY_Y == 58 &&
                   FOOTER_DIV == 190 && FOOTER_Y == 198),
              "240 vertical rhythm shifted");

// Rim gauge geometry. Angles are degrees with 0 = 3 o'clock, growing clockwise
// (screen coordinates: +y is down), matching polarToXY() in ui_utils.h.
constexpr int RING_R    = R - scaled(3);    // outer progress ring
constexpr int RING_TH   = scaled(5);
constexpr int ARC_R     = R - scaled(12);   // side stat arcs
constexpr int ARC_TH    = scaled(4);
static_assert(DIAM != 240 ||
                  (RING_R == 117 && RING_TH == 5 && ARC_R == 108 && ARC_TH == 4),
              "240 rim gauge geometry shifted");

// Half-width of the visible chord at row y, for a given radius.
// Returns 0 when the row is entirely outside the circle.
int chordHalfW(int y, int radius = SAFE_R);

// Leftmost / rightmost drawable x on row y.
int rowLeft(int y, int radius = SAFE_R);
int rowRight(int y, int radius = SAFE_R);

// Pixel width of a string at the given text size.
int textW(const char* s, int size);

// True if a string of the given width fits within the chord across the whole
// height of the text (checks the narrowest row it would occupy).
bool textFits(int y, int pxWidth, int size);

// ── Primitives ────────────────────────────────────────────────────────────

// Horizontal rule clipped to the chord - the round replacement for
// drawFastHLine(0, y, SCREEN_W, color).
void hLine(TFT_eSPI& tft, int y, uint16_t color, int inset = 0);

// Chord-clipped filled band - the round replacement for
// fillRect(0, y, SCREEN_W, h, color). Reads as a capsule rather than a
// rectangle with invisible ends.
void rowFill(TFT_eSPI& tft, int y, int h, uint16_t color, int inset = 0);

// Centered text. Returns the x it was drawn at.
int centerText(TFT_eSPI& tft, int y, const char* s, int size,
               uint16_t fg, uint16_t bg);

// printf-style centered text (max 63 chars after formatting).
int centerPrintf(TFT_eSPI& tft, int y, int size, uint16_t fg, uint16_t bg,
                 const char* fmt, ...) __attribute__((format(printf, 6, 7)));

// Text left-aligned to the chord at its row (inset from the glass edge).
void leftText(TFT_eSPI& tft, int y, const char* s, int size,
              uint16_t fg, uint16_t bg, int inset = 4);

// Text right-aligned to the chord at its row.
void rightText(TFT_eSPI& tft, int y, const char* s, int size,
               uint16_t fg, uint16_t bg, int inset = 4);

// Word-wrapped centered text block. Each line is wrapped to the number of
// columns that actually fit at ITS OWN row, so a paragraph naturally narrows
// as it approaches the top or bottom of the glass instead of being clipped by
// a single fixed column count. Stops before maxY. Returns the y after the last
// line drawn.
int textBlock(TFT_eSPI& tft, int y, int maxY, const char* text, int size,
              uint16_t fg, uint16_t bg, int inset = 8);

// ── Chrome ────────────────────────────────────────────────────────────────

// Centered size-2 title with a chord-clipped rule under it.
void header(TFT_eSPI& tft, const char* title, uint16_t color);

// Chord-clipped rule with a centered size-1 hint under it.
void footer(TFT_eSPI& tft, const char* hint, uint16_t color);

// ── Rim gauges ────────────────────────────────────────────────────────────

// Arc segment from a0 to a1 degrees at the given radius and thickness.
void arc(TFT_eSPI& tft, float a0, float a1, int radius, int thickness,
         uint16_t color);

// Progress arc: draws the full sweep in bgColor then the filled portion in
// color. pct is clamped to 0..100.
void arcGauge(TFT_eSPI& tft, float a0, float a1, int pct, int radius,
              int thickness, uint16_t color, uint16_t bgColor);

// Full-circumference progress ring starting at 12 o'clock, running clockwise.
void progressRing(TFT_eSPI& tft, int pct, uint16_t color, uint16_t bgColor);

} // namespace uiround

#endif // HEXHOUND_PANEL_ROUND
