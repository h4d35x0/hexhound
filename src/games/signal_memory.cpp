#include "signal_memory.h"

#include "../hal/tft_compat.h"

#include <stdio.h>

// ── HexHound - Signal Memory Implementation ─────────────────────
//
// Same discipline as packet_chase.cpp: the sequence buffer is allocated once
// in begin() and freed in end(), update() never touches the heap, and nothing
// clears the whole panel during play. Only the tiles that actually changed
// state are repainted, so a cursor step costs two tile fills rather than a
// full redraw.

namespace {

constexpr uint32_t FRAME_MS        = 33;
constexpr uint32_t INTRO_MS        = 900;
constexpr uint32_t GOOD_FLASH_MS   = 520;
constexpr uint32_t BAD_FLASH_MS    = 900;
constexpr uint32_t OVER_HOLD_MS    = 1600;
// A round has to be able to end on its own if the player walks away mid-sweep.
constexpr uint32_t INPUT_QUIET_MS  = 7000;

// Colour-blind safe by construction: every tile carries a label, so colour is
// reinforcement rather than the only channel carrying the answer.
const uint16_t TILE_COLOR[4] = { TFT_CYAN, TFT_MAGENTA, TFT_GREEN, TFT_YELLOW };
const char* const TILE_LABEL[4] = { "ARP", "DNS", "TCP", "UDP" };

constexpr uint16_t COL_TITLE = TFT_MAGENTA;

}  // namespace

SignalMemory& SignalMemory::instance() {
    static SignalMemory g;
    return g;
}

// ── Cadence ───────────────────────────────────────────────────────────────
//
// Everything speeds up with the sequence length and then floors, so late
// levels are demanding but never become a coin flip.

uint32_t SignalMemory::litMs() const {
    int32_t v = 480 - (int32_t)_len * 18;
    return (uint32_t)(v < 200 ? 200 : v);
}

uint32_t SignalMemory::gapMs() const {
    int32_t v = 200 - (int32_t)_len * 6;
    return (uint32_t)(v < 90 ? 90 : v);
}

uint32_t SignalMemory::dwellMs() const {
    int32_t v = 640 - (int32_t)_len * 20;
    return (uint32_t)(v < 300 ? 300 : v);
}

// ── Geometry ──────────────────────────────────────────────────────────────
//
// A 2x2 grid inside the play rectangle. That shape works on every panel
// family: it needs no minimum width the 160x80 cannot give, and on the round
// panel the play rectangle is already inscribed in the glass, so the corner
// tiles land on glass rather than under the bezel.

void SignalMemory::computeGeometry() {
    const games::Rect p = games::playArea();
    const int gap = games::panel() == games::PANEL_COMPACT ? 3 : 5;

    const int tw = (p.w - gap * 3) / 2;
    const int th = (p.h - gap * 3) / 2;

    for (int i = 0; i < TILES; i++) {
        const int col = i % 2;
        const int row = i / 2;
        _tiles[i].x = p.x + gap + col * (tw + gap);
        _tiles[i].y = p.y + gap + row * (th + gap);
        _tiles[i].w = tw;
        _tiles[i].h = th;
    }

    // Largest label that fits both axes of a tile, capped so a big panel does
    // not turn three characters into a billboard.
    //
    // The divisors carry realTextSize(1) / lineHeight(1) because tw and th are
    // REAL pixels while _labelSize is 240-relative: see ChipRow::layout(). Here
    // the clamp to 3 happens to absorb the error at all four panels today, so
    // this changes no size on any board. It is corrected anyway - a formula
    // that is only right because a clamp hides it is how the same defect
    // reached four different call sites.
    const int byW = (tw - 4) / (3 * 6 * games::realTextSize(1));
    const int byH = (th - 4) / games::lineHeight(1);
    _labelSize = games::clampInt(byW < byH ? byW : byH, 1, 3);

    _barH = games::clampInt(th / 10, games::scaled(2), games::scaled(6));
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void SignalMemory::appendSymbol() {
    if (_len >= MAX_SEQ) return;
    // Never repeat the same tile three times running: a triple looks like a
    // rendering glitch rather than a sequence, and players read it as a bug.
    uint8_t s = (uint8_t)_rng.range(TILES);
    if (_len >= 2 && _seq[_len - 1] == s && _seq[_len - 2] == s) {
        s = (uint8_t)((s + 1 + _rng.range(TILES - 1)) % TILES);
    }
    _seq[_len++] = s;
}

void SignalMemory::begin(TFT_eSPI* tft) {
    _tft = tft;

    if (!_seq) {
        _seq = new uint8_t[MAX_SEQ];
    }
    for (int i = 0; i < MAX_SEQ; i++) _seq[i] = 0;

    computeGeometry();

    _rng.seed(millis() * 2246822519u + 0x85EBCA6Bu);

    _len       = 0;
    _showIdx   = 0;
    _inputIdx  = 0;
    _cursor    = 0;
    _score     = 0;
    _barW      = -1;
    _timedOut  = false;
    _completed = false;
    _pressPending = false;
    _phase     = PH_INTRO;
    _startMs   = millis();
    _lastMs    = _startMs;
    _phaseMs   = _startMs;
    _inputMs   = _startMs;
    _result    = MinigameResult();

    for (int i = 0; i < TILES; i++) _tileState[i] = TS_UNDRAWN;

    appendSymbol();

    if (!_tft) return;

    // No clearPlay() here: chromeInit() blanks the whole band between the two
    // rules, which is a superset of the play area, and a clearPlay() first only
    // fills part of it twice.
    //
    // The round panel's title row is a 128px chord, so the long form would run
    // under the bezel there. Folded at compile time.
    games::chromeInit(*_tft,
                      games::panel() == games::PANEL_ROUND ? "SIGNAL"
                                                           : "SIGNAL MEMORY",
                      COL_TITLE);
    drawAllTiles(TS_IDLE);
    drawStatus("WATCH", TFT_CYAN);
}

void SignalMemory::end() {
    if (_seq) {
        delete[] _seq;
        _seq = nullptr;
    }
    const uint32_t now = millis();
    _result.score      = _score;
    _result.durationMs = games::clampDuration(now - _startMs);
    _result.completed  = _completed;
    _result.newBest    = false;      // owned by the caller's high-score store
    _tft = nullptr;
}

void SignalMemory::onPress() {
    // Latch WHICH tile was under the cursor at the instant of the press, not
    // whichever tile the sweep has reached by the time update() runs. Without
    // this the player is punished for the frame interval.
    _pressedTile  = _cursor;
    _pressPending = true;
}

// ── Drawing ───────────────────────────────────────────────────────────────

void SignalMemory::drawTile(int i, uint8_t state) {
    if (!_tft) return;
    const games::Rect& t = _tiles[i];
    const uint16_t c = TILE_COLOR[i];

    uint16_t fill = TFT_BLACK;
    uint16_t border = dimColor(c, 1);
    uint16_t label = dimColor(c, 2);

    switch (state) {
    case TS_LIT:
        fill = c;  border = c;  label = TFT_BLACK;
        break;
    case TS_CURSOR:
        fill = TFT_BLACK;  border = c;  label = c;
        break;
    case TS_WRONG:
        fill = TFT_RED;  border = TFT_RED;  label = TFT_BLACK;
        break;
    default:
        break;
    }

    _tft->fillRect(t.x, t.y, t.w, t.h, fill);
    _tft->drawRect(t.x, t.y, t.w, t.h, border);
    if (state == TS_CURSOR) {
        _tft->drawRect(t.x + 1, t.y + 1, t.w - 2, t.h - 2, border);
    }

    // textWidth() rather than a raw 6 * size: _labelSize is 240-RELATIVE, so on
    // the 480 panel the glyph is 12 px wide and this measured 6. The label was
    // then centred against half its real width and sat a quarter of its own
    // width right of centre in the tile. lh below was already asking
    // lineHeight() for the real height, which is why only the horizontal
    // centring was off and it read as a nudge rather than as a bug.
    const int lw = games::textWidth(TILE_LABEL[i], _labelSize);
    const int lh = games::lineHeight(_labelSize);
    _tft->setTextSize(games::realTextSize(_labelSize));
    _tft->setTextColor(label, fill);
    _tft->setCursor(t.x + (t.w - lw) / 2, t.y + (t.h - lh) / 2);
    _tft->print(TILE_LABEL[i]);

    _tileState[i] = state;
}

void SignalMemory::setTile(int i, uint8_t state) {
    if (_tileState[i] == state) return;
    drawTile(i, state);
}

void SignalMemory::drawAllTiles(uint8_t state) {
    for (int i = 0; i < TILES; i++) setTile(i, state);
}

// The sweep cadence made visible: a bar under the cursor tile that drains over
// the dwell. Without it the player is guessing at the cadence, and a timing
// game you cannot see the clock for is just luck.
void SignalMemory::drawCursorBar(uint32_t nowMs) {
    if (!_tft) return;
    const games::Rect& t = _tiles[_cursor];
    const int full = t.w - 6;
    if (full <= 0) return;

    const uint32_t dw = dwellMs();
    uint32_t elapsed = nowMs - _phaseMs;
    if (elapsed > dw) elapsed = dw;
    int w = full - (int)((uint64_t)full * elapsed / dw);
    if (w < 0) w = 0;
    if (w == _barW) return;

    const int by = t.y + t.h - _barH - 2;
    if (w < _barW) {
        _tft->fillRect(t.x + 3 + w, by, _barW - w, _barH, TFT_BLACK);
    }
    if (w > 0) {
        _tft->fillRect(t.x + 3, by, w, _barH, TILE_COLOR[_cursor]);
    }
    _barW = w;
}

void SignalMemory::clearCursorBar() {
    if (!_tft || _barW <= 0) { _barW = -1; return; }
    const games::Rect& t = _tiles[_cursor];
    _tft->fillRect(t.x + 3, t.y + t.h - _barH - 2, t.w - 6, _barH, TFT_BLACK);
    _barW = -1;
}

void SignalMemory::drawStatus(const char* what, uint16_t color) {
    if (!_tft) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "LVL %d  %s", (int)_len, what);
    games::status(*_tft, buf, color);
}

void SignalMemory::drawOver() {
    if (!_tft) return;
    games::clearPlay(*_tft);
    for (int i = 0; i < TILES; i++) _tileState[i] = TS_UNDRAWN;

    const games::Rect p = games::playArea();
    const int size = games::bodyTextSize();
    const int lineH = games::linePitch(size);
    int y = p.y + (p.h - lineH * 2) / 2;
    if (y < p.y) y = p.y;

    games::centerText(*_tft, y,
                      _timedOut ? "SIGNAL LOST" : "DESYNC",
                      size, TFT_RED, TFT_BLACK);
    y += lineH;

    char buf[32];
    snprintf(buf, sizeof(buf), "SEQUENCE %lu", (unsigned long)_score);
    games::centerText(*_tft, y, buf, size, TFT_WHITE, TFT_BLACK);
}

// ── Main update ───────────────────────────────────────────────────────────

bool SignalMemory::update(uint32_t nowMs) {
    if (!_seq) return false;

    if (nowMs - _lastMs < FRAME_MS) return true;
    _lastMs = nowMs;

    switch (_phase) {
    case PH_INTRO:
        _pressPending = false;
        if (nowMs - _phaseMs >= INTRO_MS) {
            _showIdx = 0;
            _phase   = PH_SHOW;
            _phaseMs = nowMs;
            setTile(_seq[0], TS_LIT);
        }
        return true;

    case PH_SHOW:
        _pressPending = false;
        if (nowMs - _phaseMs >= litMs()) {
            setTile(_seq[_showIdx], TS_IDLE);
            _phase   = PH_SHOW_GAP;
            _phaseMs = nowMs;
        }
        return true;

    case PH_SHOW_GAP:
        _pressPending = false;
        if (nowMs - _phaseMs >= gapMs()) {
            _showIdx++;
            if (_showIdx < _len) {
                setTile(_seq[_showIdx], TS_LIT);
                _phase   = PH_SHOW;
                _phaseMs = nowMs;
            } else {
                _inputIdx = 0;
                _cursor   = 0;
                _barW     = -1;
                _phase    = PH_INPUT;
                _phaseMs  = nowMs;
                _inputMs  = nowMs;
                setTile(_cursor, TS_CURSOR);
                drawStatus("YOUR TURN", TFT_GREEN);
            }
        }
        return true;

    case PH_INPUT: {
        if (_pressPending) {
            _pressPending = false;
            const uint8_t picked = _pressedTile < TILES ? _pressedTile : 0;

            if (picked == _seq[_inputIdx]) {
                _inputIdx++;
                if (_inputIdx >= _len) {
                    // Whole sequence reproduced.
                    _score = _len;
                    clearCursorBar();
                    setTile(_cursor, TS_IDLE);
                    drawAllTiles(TS_LIT);
                    drawStatus("CORRECT", TFT_GREEN);
                    _phase   = PH_GOOD;
                    _phaseMs = nowMs;
                    return true;
                }
                // Confirm the hit, restart the sweep from the picked tile so
                // the next choice is never a full lap away by accident.
                clearCursorBar();
                // The sweep can have stepped on between the press and this
                // frame. The press is still judged against the tile that was
                // highlighted when the button went down, so the tile the sweep
                // moved to has to be put back to idle or two tiles stay lit.
                if (_cursor != picked) setTile(_cursor, TS_IDLE);
                drawTile(picked, TS_LIT);
                _cursor  = picked;
                _phaseMs = nowMs;
                _inputMs = nowMs;
                _barW    = -1;
                return true;
            }

            clearCursorBar();
            if (_cursor != picked) setTile(_cursor, TS_IDLE);
            setTile(picked, TS_WRONG);
            drawStatus("DESYNC", TFT_RED);
            _timedOut = false;
            _phase    = PH_BAD;
            _phaseMs  = nowMs;
            return true;
        }

        // A tile flashed as confirmation returns to the cursor look on the
        // next frame, so the confirmation is visible but does not persist.
        if (_tileState[_cursor] == TS_LIT && nowMs - _phaseMs >= 120) {
            drawTile(_cursor, TS_CURSOR);
        }

        if (nowMs - _phaseMs >= dwellMs()) {
            clearCursorBar();
            setTile(_cursor, TS_IDLE);
            _cursor  = (uint8_t)((_cursor + 1) % TILES);
            _phaseMs = nowMs;
            setTile(_cursor, TS_CURSOR);
        } else if (_tileState[_cursor] == TS_CURSOR) {
            drawCursorBar(nowMs);
        }

        // Quiet-signal timeout: measured from the start of this input attempt,
        // NOT from _phaseMs, which the sweep resets on every cursor step and
        // which therefore could never reach the threshold.
        if (nowMs - _inputMs >= INPUT_QUIET_MS) {
            _timedOut = true;
            clearCursorBar();
            drawStatus("SIGNAL LOST", TFT_RED);
            _phase   = PH_BAD;
            _phaseMs = nowMs;
        }
        return true;
    }

    case PH_GOOD:
        _pressPending = false;
        if (nowMs - _phaseMs >= GOOD_FLASH_MS) {
            drawAllTiles(TS_IDLE);
            if (_len >= MAX_SEQ) {
                // Nothing left to add: the player beat the game outright.
                _completed = true;
                _result.score      = _score;
                _result.durationMs = games::clampDuration(nowMs - _startMs);
                _result.completed  = true;
                _timedOut = false;
                drawOver();
                _phase   = PH_OVER;
                _phaseMs = nowMs;
                return true;
            }
            appendSymbol();
            _showIdx = 0;
            _phase   = PH_SHOW;
            _phaseMs = nowMs;
            drawStatus("WATCH", TFT_CYAN);
            setTile(_seq[0], TS_LIT);
        }
        return true;

    case PH_BAD:
        _pressPending = false;
        if (nowMs - _phaseMs >= BAD_FLASH_MS) {
            _completed = true;       // played out, not abandoned
            _result.score      = _score;
            _result.durationMs = games::clampDuration(nowMs - _startMs);
            _result.completed  = true;
            drawOver();
            _phase   = PH_OVER;
            _phaseMs = nowMs;
        }
        return true;

    case PH_OVER:
    default:
        if (nowMs - _phaseMs >= OVER_HOLD_MS) {
            if (_pressPending) {
                _pressPending = false;
                return false;
            }
            if (nowMs - _phaseMs >= OVER_HOLD_MS * 4) return false;
        } else {
            _pressPending = false;   // swallow the press that ended the round
        }
        return true;
    }
}
