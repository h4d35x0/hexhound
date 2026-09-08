#pragma once

#include "../board/board_profile.h"
#include "../ui/ui_utils.h"
#include "../ui/ui_round.h"

#include <stdint.h>
#include <string.h>

// ── HexHound - Minigame Layout Kit ──────────────────────────────
//
// Three panel families ship today: the 160x80 T-Dongle, the 320x172 Waveshare
// and the 240x240 round Waveshare. A minigame that hard-codes coordinates for
// one of them is broken on the other two, and the failure is invisible until
// somebody flashes that board.
//
// So no game file contains a panel dimension. Every game asks this header for
// a play rectangle and for where its status line goes, and this header is the
// only place that knows which family it is compiling for. On the round panel
// the play rectangle is inscribed in the glass circle, so a game can treat it
// as an ordinary rectangle and still never draw a pixel behind the bezel.
//
// Everything here is header-only and either constexpr or a handful of integer
// operations. Nothing allocates.

namespace games {

// Explicit constructors rather than default member initialisers: the firmware
// targets build as C++11, where a struct with NSDMIs is not an aggregate and
// Rect{a,b,c,d} stops compiling.
struct Rect {
    int x;
    int y;
    int w;
    int h;

    Rect() : x(0), y(0), w(0), h(0) {}
    Rect(int rx, int ry, int rw, int rh) : x(rx), y(ry), w(rw), h(rh) {}
};

enum PanelFamily : uint8_t {
    PANEL_COMPACT = 0,   // 160x80
    PANEL_BIG,           // 320x172
    PANEL_ROUND          // 240x240 circular glass
};

constexpr PanelFamily panel() {
#if HEXHOUND_PANEL_ROUND
    return PANEL_ROUND;
#else
    return SCREEN_H > 100 ? PANEL_BIG : PANEL_COMPACT;
#endif
}

// Text size for the status line and in-play banners.
constexpr int bodyTextSize() {
    return panel() == PANEL_COMPACT ? 1 : 2;
}

constexpr int statusTextSize() {
    // The round panel's footer row is a narrow chord, so it takes size 1 even
    // though the panel is large.
    return panel() == PANEL_BIG ? 2 : 1;
}

inline int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// A 240-RELATIVE pixel count, in the size actually drawn.
//
// The games size their chips, cells and bars as a fraction of the play area
// and then CLAMP that fraction to a ceiling. The fractions scale with the
// panel; the ceilings did not, and they are 240-relative numbers, so on the
// 480 panel every one of them bound at half the relative size it was authored
// at. That is why the play field there was correct but small: a 34 px chip
// ceiling on a 480 panel is the 17 px chip a 240 panel would have shown.
//
// Identity on every rectangular panel, so their images cannot move. Guarded
// rather than written as a runtime multiply for the reason ui_menu.h already
// records: a helper referenced from several sites leaves GCC an out-of-line
// copy, and the non-round boards' images grew 16 bytes when one of these was
// a method.
#if HEXHOUND_PANEL_ROUND
constexpr int scaled(int at240) { return uiround::scaled(at240); }
#else
constexpr int scaled(int at240) { return at240; }
#endif

// ── 240-relative sizes vs the size actually drawn ─────────────────────────
//
// bodyTextSize() and statusTextSize() are 240-RELATIVE, like every size handed
// to a uiround:: helper: centerText() below forwards them to
// uiround::centerText(), which multiplies by TEXT_SCALE internally. On the 480
// panel a "size 2" string is therefore drawn at 4 and stands 32 px tall, not 16.
//
// Every game then computed its own line pitch as `8 * size + 4` - 20 px - and
// drew 32 px of text into it, so all four GAME OVER screens overlapped
// themselves on the 480 panel. Same defect as the patrol screen's `step = 22`.
//
// So: ask for the REAL metrics here rather than multiplying a 240-relative size
// by 8 at the call site. All three reproduce the old values exactly at 240 and
// on the rectangular panels, where realTextSize() is the identity.

constexpr int realTextSize(int size240) {
#if HEXHOUND_PANEL_ROUND
    return uiround::ts(size240);
#else
    return size240;
#endif
}

// Height of one drawn line, in real pixels.
constexpr int lineHeight(int size240) { return 8 * realTextSize(size240); }

// A pitch that leaves the old 4 px of leading, scaled with the panel.
constexpr int linePitch(int size240) {
#if HEXHOUND_PANEL_ROUND
    return lineHeight(size240) + uiround::scaled(4);
#else
    return lineHeight(size240) + 4;
#endif
}

static_assert(realTextSize(2) == 2 || HEXHOUND_PANEL_ROUND,
              "realTextSize must be the identity on a rectangular panel");

// ── Chrome geometry ───────────────────────────────────────────────────────

inline int headerDiv() {
#if HEXHOUND_PANEL_ROUND
    return uiround::HEADER_DIV;
#else
    return panel() == PANEL_BIG ? uilg::HEADER_DIV : 12;
#endif
}

inline int footerDiv() {
#if HEXHOUND_PANEL_ROUND
    return uiround::FOOTER_DIV;
#else
    return panel() == PANEL_BIG ? uilg::FOOTER_DIV : (SCREEN_H - 13);
#endif
}

inline int statusY() {
#if HEXHOUND_PANEL_ROUND
    return uiround::FOOTER_Y;
#else
    return panel() == PANEL_BIG ? uilg::FOOTER_TEXT : (SCREEN_H - 10);
#endif
}

// ── Play rectangle ────────────────────────────────────────────────────────
//
// The area between the header rule and the footer rule. On the round panel it
// is the widest rectangle that fits inside the safe circle across that whole
// vertical band, so its corners are on glass and not under the bezel.

inline Rect playArea() {
#if HEXHOUND_PANEL_ROUND
    const int top = uiround::BODY_Y;
    const int bot = uiround::FOOTER_DIV - 4;
    int hw = uiround::chordHalfW(top);
    const int hwBot = uiround::chordHalfW(bot);
    if (hwBot < hw) hw = hwBot;
    return Rect{ uiround::CX - hw, top, hw * 2, bot - top };
#else
    if (panel() == PANEL_BIG) {
        return Rect{ uilg::PAD, uilg::BODY_Y,
                     SCREEN_W - 2 * uilg::PAD,
                     uilg::FOOTER_DIV - 2 - uilg::BODY_Y };
    }
    return Rect{ 3, 15, SCREEN_W - 6, (SCREEN_H - 13) - 2 - 15 };
#endif
}

// ── Primitives ────────────────────────────────────────────────────────────

// Clear one horizontal band. Chord-clipped on the round panel so it never
// touches pixels the bezel hides.
inline void clearRow(TFT_eSPI& tft, int y, int h) {
    if (h <= 0) return;
#if HEXHOUND_PANEL_ROUND
    uiround::rowFill(tft, y, h, TFT_BLACK);
#else
    tft.fillRect(0, y, SCREEN_W, h, TFT_BLACK);
#endif
}

// Width of a drawn string. Takes a 240-RELATIVE size, like everything else
// here, and converts once - callers were passing a 240-relative size into a
// raw `6 * size` and under-measuring by half on the 480 panel.
inline int textWidth(const char* s, int size240) {
    return s ? (int)strlen(s) * 6 * realTextSize(size240) : 0;
}

// Centered text on any panel family.
inline void centerText(TFT_eSPI& tft, int y, const char* s, int size,
                       uint16_t fg, uint16_t bg = TFT_BLACK) {
    if (!s || !*s) return;
#if HEXHOUND_PANEL_ROUND
    uiround::centerText(tft, y, s, size, fg, bg);
#else
    int x = (SCREEN_W - textWidth(s, size)) / 2;
    if (x < 0) x = 0;
    tft.setTextSize(realTextSize(size));
    tft.setTextColor(fg, bg);
    tft.setCursor(x, y);
    tft.print(s);
#endif
}

// Blank every row the game owns, then draw the title and both rules.
// Called once when a game starts.
//
// ── Why this clears, and why clearPlay() was not enough ───────────────────
//
// A game owns every row down to its status line, but playArea() is deliberately
// INSET from the rules so a sprite can never sit on one. That inset is a
// POSITIONING constraint; it was also being used as the CLEARING boundary, and
// nothing owned the difference. Whatever the previous screen had drawn in the
// margin survived into the round and stayed for the whole game. There were
// four separate margins, and each one hid the next until the one in front of
// it was cleared. Measured on the sim, not derived:
//
//   1. under the header rule   round 97..115 at 480; big 29..33; compact 13..14
//   2. above the footer rule   round 376..379 at 480; big 144..145; compact
//                              SCREEN_H-15..SCREEN_H-14
//   3. between the footer rule and the status line
//                              round 381..395 at 480; big 147..149; compact
//                              SCREEN_H-12..SCREEN_H-11
//   4. the status row itself, for a game that uses statusInPlace() - packet
//      chase does, and statusInPlace() deliberately never clears, so nothing
//      had ever blanked that row and the residue either side of its fixed-width
//      string stayed for the whole game
//
// plus the PAD columns at each edge on big, and 3 columns each edge on compact.
//
// On the round 480, margin 2 was the bottom two rows of the HUD's
// "T2  M0  E100" line lying on the footer rule, and margin 1 was the bottom of
// the HUD's yellow subtitle hanging under the header rule. The identical
// residue, at identical x-runs, appeared in three different games, which is
// what proved no game was drawing it. The 160x80 panel was the worst of the
// four: the whole tail of the HUD's bottom row stayed legible for the entire
// game.
//
// The title row needs clearing too, and for the same reason the status line
// does: every header helper paints glyphs with an opaque background, so a title
// overwrites exactly the cells it occupies and nothing else. A shorter title,
// or a previous screen's subtitle sitting between the title and the rule, is
// left untouched.
//
// So this clears from row 0 to the bottom of the status row, which is one fill
// and covers all four margins at once. Everything below that belongs to the
// caller's footer hint, not to the game.
//
// It goes here, once, rather than in clearPlay(): residue can only arrive from
// the screen shown BEFORE the game, and clearPlay() also runs at game over,
// where widening it would wipe scenery a game means to keep. Packet chase's
// wire sits one row below the play rectangle and has to survive its drawOver().
inline void chromeInit(TFT_eSPI& tft, const char* title, uint16_t color) {
    // Both rules are drawn after this, so the fill may run straight through
    // them. On the round panel it is chord-clipped per row, which is why
    // clearing "full width" still never reaches the bezel.
    const int clearH = statusY() + lineHeight(statusTextSize());
#if HEXHOUND_PANEL_ROUND
    uiround::rowFill(tft, 0, clearH, TFT_BLACK);
#else
    tft.fillRect(0, 0, SCREEN_W, clearH, TFT_BLACK);
#endif

#if HEXHOUND_PANEL_ROUND
    uiround::header(tft, title, color);
    uiround::hLine(tft, uiround::FOOTER_DIV, color);
#else
    if (panel() == PANEL_BIG) {
        uiBigHeader(tft, title, color);
        tft.drawFastHLine(0, uilg::FOOTER_DIV, SCREEN_W, color);
    } else {
        tft.setTextSize(1);
        tft.setTextColor(color, TFT_BLACK);
        tft.setCursor(3, 2);
        tft.print(title);
        tft.drawFastHLine(3, 12, SCREEN_W - 6, color);
        tft.drawFastHLine(3, SCREEN_H - 13, SCREEN_W - 6, color);
    }
#endif
}

// Redraw the status line, clearing the row first. Use when the text can change
// length. It never touches the play area.
inline void status(TFT_eSPI& tft, const char* text, uint16_t color) {
    const int size = statusTextSize();
    clearRow(tft, statusY(), lineHeight(size));
    centerText(tft, statusY(), text, size, color, TFT_BLACK);
}

// Status line redrawn without clearing first: the glyphs are painted with an
// opaque background, so they overwrite the previous ones exactly.
//
// Only correct when every string the caller passes is the SAME length, which
// is why callers pad their numbers to a fixed width. The payoff is no
// clear-then-draw flash on a counter that updates several times a second,
// which on the 160x80 panel is the difference between a HUD and a strobe.
//
// Same-length is necessary but was not sufficient: the row also has to START
// blank, and nothing was blanking it, so whatever the previous screen left
// either side of the string stayed there for the whole game. chromeInit() now
// clears through this row, which is the only guarantee that makes an
// overwrite-only status line correct on its first call.
inline void statusInPlace(TFT_eSPI& tft, const char* text, uint16_t color) {
    centerText(tft, statusY(), text, statusTextSize(), color, TFT_BLACK);
}

// Clear only the play area. Used at round start and at game over, never per
// frame: a full-area fill on every frame is what makes a small panel flicker.
inline void clearPlay(TFT_eSPI& tft) {
    const Rect p = playArea();
#if HEXHOUND_PANEL_ROUND
    uiround::rowFill(tft, p.y, p.h, TFT_BLACK);
#else
    tft.fillRect(p.x, p.y, p.w, p.h, TFT_BLACK);
#endif
}

// ── Deterministic small PRNG ──────────────────────────────────────────────
//
// Its own state rather than the global random(), so a game's spawn pattern can
// never be perturbed by an unrelated caller, and so a round is reproducible
// from its seed when something needs debugging.

class Rng {
public:
    void seed(uint32_t s) { _s = s ? s : 0xA5A5F00Du; }
    uint32_t next() {
        _s ^= _s << 13;
        _s ^= _s >> 17;
        _s ^= _s << 5;
        return _s;
    }
    // Uniform-ish in [0, n).
    int range(int n) { return n <= 0 ? 0 : (int)(next() % (uint32_t)n); }
    // Uniform-ish in [lo, hi].
    int between(int lo, int hi) { return hi <= lo ? lo : lo + range(hi - lo + 1); }

private:
    uint32_t _s = 0x2545F491u;
};

// MinigameResult::durationMs is 16 bits, so a long round has to be clamped
// rather than silently wrapped to a small number.
inline uint16_t clampDuration(uint32_t ms) {
    return ms > 65535u ? (uint16_t)65535u : (uint16_t)ms;
}

}  // namespace games
