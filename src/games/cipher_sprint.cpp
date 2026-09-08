#include "cipher_sprint.h"

#include "../hal/tft_compat.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Cipher Sprint Implementation ─────────────────────
//
// Same discipline as the other three games in this directory:
//
//  * The puzzle buffer is allocated once in begin() and freed in end().
//    update() never touches the heap: a new puzzle is generated in place, into
//    the buffer that is already there.
//  * Nothing clears the whole panel during play. The card is cleared once per
//    puzzle, the chips repaint their own rects, and the deadline bar repaints
//    only the pixels that just expired.
//  * update() early-returns until FRAME_MS has elapsed.

namespace {

constexpr uint32_t FRAME_MS     = 33;
constexpr uint32_t INTRO_MS     = 1400;
constexpr uint32_t MARK_MS      = 900;
constexpr uint32_t OVER_HOLD_MS = 1600;

constexpr uint16_t COL_TITLE = TFT_GREEN;

// Answers are interchangeable, so the colour is positional (which chip) rather
// than semantic (what it means). Every chip still carries its text, so colour
// is never the only channel carrying the answer.
const uint16_t OPT_COLOR[3] = { TFT_CYAN, TFT_YELLOW, TFT_MAGENTA };

// Three letters each, so a puzzle is readable at text size 1 on the 160x80
// panel. None is a palindrome, which the reversal family relies on.
const char* const WORDS[] = {
    "NET", "BIT", "KEY", "LOG", "ARP", "DNS", "TCP", "UDP", "HEX", "ACK",
    "SYN", "FIN", "PID", "RAM", "CPU", "BUS", "MAC", "VPN", "SSH", "TAP"
};
constexpr int WORD_N = (int)(sizeof(WORDS) / sizeof(WORDS[0]));

// Longest card line is the intro ("MATCH THE RULE"), which is what the card is
// sized for.
constexpr int CARD_COLS = 14;

// Distractor shifts, tried in order until two usable ones are found.
const int DISTRACTOR_K[] = { 1, -1, 2, -2, 3, -3, 4 };
constexpr int DISTRACTOR_N = (int)(sizeof(DISTRACTOR_K) / sizeof(DISTRACTOR_K[0]));

enum Family : uint8_t {
    FAM_SHIFT = 0,
    FAM_REVERSE,
    FAM_SEQ
};

void shift3(const char* s, int k, char* dst) {
    for (int i = 0; i < 3; i++) {
        int c = (int)(s[i] - 'A') + k;
        c %= 26;
        if (c < 0) c += 26;
        dst[i] = (char)('A' + c);
    }
    dst[3] = '\0';
}

void rev3(const char* s, char* dst) {
    dst[0] = s[2];
    dst[1] = s[1];
    dst[2] = s[0];
    dst[3] = '\0';
}

// How many of this game's rules map `in` to `out`. The example line is only
// usable when the answer is exactly 1: an example that fits both a shift and a
// reversal leaves the challenge with two defensible answers, and a player who
// picks the other one is right to call the game broken.
int ruleMatches(const char* in, const char* out) {
    char buf[4];
    int n = 0;
    for (int k = -4; k <= 4; k++) {
        if (k == 0) continue;
        shift3(in, k, buf);
        if (strcmp(buf, out) == 0) n++;
    }
    rev3(in, buf);
    if (strcmp(buf, out) == 0) n++;
    return n;
}

}  // namespace

CipherSprint& CipherSprint::instance() {
    static CipherSprint g;
    return g;
}

// ── Cadence ───────────────────────────────────────────────────────────────
//
// The deadline shortens as the round goes on and then floors well above the
// time it takes to read two short lines, so the sprint gets tense without ever
// becoming a guess.

uint32_t CipherSprint::deadlineMs() const {
    int32_t v = 9000 - (int32_t)_index * 500;
    return (uint32_t)(v < 5500 ? 5500 : v);
}

uint32_t CipherSprint::dwellMs() const {
    int32_t v = 640 - (int32_t)_index * 20;
    return (uint32_t)(v < 400 ? 400 : v);
}

// ── Geometry ──────────────────────────────────────────────────────────────
//
// Deadline bar, two-line card, three answer chips. Every extent is a fraction
// of games::playArea(), so nothing here knows a panel dimension and the
// proportions are the same on all three families.

void CipherSprint::computeGeometry() {
    const games::Rect p = games::playArea();
    const int gap = games::panel() == games::PANEL_COMPACT ? 3 : 5;

    const int barH  = games::clampInt(p.h / 16, games::scaled(2), games::scaled(5));
    const int chipH = games::clampInt(p.h / 3, games::scaled(12), games::scaled(34));

    _clock.layout(p.x + 2, p.y, p.w - 4, barH);

    _card = games::Rect(p.x, p.y + barH + 2, p.w,
                        p.h - chipH - gap - barH - 2);
    if (_card.h < 12) _card.h = 12;

    const games::Rect chipArea(p.x, p.y + p.h - chipH, p.w, chipH);
    _chips.layout(chipArea, OPTIONS, OPT_LEN - 2);

    // realTextSize(1) / lineHeight(1) in the divisors because _card is in REAL
    // pixels while _cardTextSize is 240-relative: see ChipRow::layout(). The
    // clamp to 2 absorbs the error at all four panels today, so this picks the
    // same size everywhere and no board's image moves. Corrected regardless -
    // drawCardLines() clamps x to the card's left edge but does not clip its
    // right, and clearCard() only ever clears _card.w, so a size chosen one
    // step too large would overrun the card and leave the overrun uncleared.
    const int byW = _card.w / (CARD_COLS * 6 * games::realTextSize(1));
    const int byH = (_card.h / 2) / games::lineHeight(1);
    _cardTextSize = games::clampInt(byW < byH ? byW : byH, 1, 2);
    _cardLineH    = _card.h / 2;
}

// ── Puzzle generation ─────────────────────────────────────────────────────
//
// Generated in place into the buffer begin() allocated. No allocation, no
// recursion, and every loop is bounded.

void CipherSprint::makePuzzle() {
    Puzzle& p = *_pz;
    const uint8_t fam = (uint8_t)_rng.range(3);
    p.family = fam;

    if (fam == FAM_SEQ) {
        const int a0   = _rng.between(1, 9);
        const int step = _rng.between(2, 6);
        snprintf(p.line1, LINE_LEN, "%d %d %d %d",
                 a0, a0 + step, a0 + 2 * step, a0 + 3 * step);
        snprintf(p.line2, LINE_LEN, "NEXT ?");

        const int right = a0 + 4 * step;
        int w1 = right + step;          // one term too far
        int w2 = right - 1;             // off by one
        if (w2 == w1 || w2 == a0 + 3 * step) w2 = right + 1;

        const int slot = _rng.range(OPTIONS);
        int used = 0;
        for (int i = 0; i < OPTIONS; i++) {
            const int v = (i == slot) ? right : (used++ == 0 ? w1 : w2);
            snprintf(p.opt[i], OPT_LEN, "%d", v);
        }
        p.answer = (uint8_t)slot;
        return;
    }

    const bool reversed = (fam == FAM_REVERSE);
    const char* a = WORDS[0];
    const char* b = WORDS[1];
    char ex[4];
    int  k = 1;

    // Bounded search for an example whose rule is unambiguous. In practice the
    // first draw almost always qualifies; the loop exists so a collision can
    // never ship a puzzle with two right answers.
    bool ok = false;
    for (int tries = 0; tries < 16 && !ok; tries++) {
        const int ia = _rng.range(WORD_N);
        int ib = _rng.range(WORD_N);
        if (ib == ia) ib = (ib + 1) % WORD_N;
        a = WORDS[ia];
        b = WORDS[ib];

        if (reversed) {
            rev3(a, ex);
        } else {
            k = _rng.between(1, 4);
            if (_rng.range(2)) k = -k;
            shift3(a, k, ex);
        }
        ok = (ruleMatches(a, ex) == 1);
    }
    if (!ok) {
        // Provably unambiguous fallback: NET+1 is OFU, and reversing NET is
        // TEN, so exactly one rule explains it.
        a = "NET";
        b = "KEY";
        k = 1;
        if (reversed) rev3(a, ex); else shift3(a, k, ex);
    }

    snprintf(p.line1, LINE_LEN, "%s->%s", a, ex);
    snprintf(p.line2, LINE_LEN, "%s->???", b);

    char right[4];
    if (reversed) rev3(b, right); else shift3(b, k, right);

    // Two distractors: the other rule first, because a player who mis-read the
    // example should find their mistake on offer, then neighbouring shifts.
    char wrong[2][4];
    int got = 0;
    for (int t = 0; t <= DISTRACTOR_N && got < 2; t++) {
        char cand[4];
        if (t == 0) {
            rev3(b, cand);
        } else {
            shift3(b, (reversed ? 0 : k) + DISTRACTOR_K[t - 1], cand);
        }
        if (strcmp(cand, right) == 0) continue;
        if (got == 1 && strcmp(cand, wrong[0]) == 0) continue;
        memcpy(wrong[got], cand, 4);
        got++;
    }
    // Cannot happen with the table above (there are always at least two
    // distinct shifts), but a half-filled option row would be unplayable.
    while (got < 2) {
        shift3(b, 5 + got, wrong[got]);
        got++;
    }

    const int slot = _rng.range(OPTIONS);
    int used = 0;
    for (int i = 0; i < OPTIONS; i++) {
        const char* s = (i == slot) ? right : wrong[used++];
        snprintf(p.opt[i], OPT_LEN, "%s", s);
    }
    p.answer = (uint8_t)slot;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void CipherSprint::begin(TFT_eSPI* tft) {
    _tft = tft;

    if (!_pz) {
        _pz = new Puzzle();
    }
    memset(_pz, 0, sizeof(Puzzle));

    computeGeometry();
    for (int i = 0; i < OPTIONS; i++) {
        _chips.setLabel(i, _pz->opt[i], OPT_COLOR[i]);
    }

    _rng.seed(millis() * 2891336453u + 0x27D4EB2Fu);

    _score     = 0;
    _index     = 0;
    _solved    = 0;
    _lives     = START_LIVES;
    _completed = false;
    _ended     = false;
    _pressPending = false;
    _pressedChip  = 0;
    _phase     = PH_INTRO;
    _startMs   = millis();
    _lastMs    = _startMs;
    _phaseMs   = _startMs;
    _sweepMs   = _startMs;
    _result    = MinigameResult();

    _chips.forget();
    _clock.forget();

    if (!_tft) return;

    // No clearPlay() here: chromeInit() blanks the whole band between the two
    // rules, which is a superset of the play area, and a clearPlay() first only
    // fills part of it twice.
    //
    // The round panel's title row is a 128px chord, so the long form would run
    // under the bezel there. Folded at compile time.
    games::chromeInit(*_tft,
                      games::panel() == games::PANEL_ROUND ? "CIPHER"
                                                           : "CIPHER SPRINT",
                      COL_TITLE);
    _chips.drawAll(*_tft, games::CHIP_IDLE);
    drawCardLines("MATCH THE RULE", "PICK ANSWER", TFT_YELLOW);
    drawStatus("DECRYPT 8", TFT_CYAN);
}

void CipherSprint::end() {
    if (_pz) {
        delete _pz;
        _pz = nullptr;
    }
    // Latched rather than recomputed: the abandon path can reach end() twice,
    // and a second pass must not stretch durationMs or restate the result.
    if (!_ended) {
        _ended = true;
        _result.score      = _score;
        _result.durationMs = games::clampDuration(millis() - _startMs);
        _result.completed  = _completed;
        _result.newBest    = false;   // owned by the caller's high-score store
    }
    _tft = nullptr;
}

void CipherSprint::onPress() {
    // Latch WHICH chip was under the cursor at the instant of the press, not
    // whichever chip the sweep has reached by the time update() runs.
    _pressedChip  = (uint8_t)_chips.cursor();
    _pressPending = true;
}

#if HEXHOUND_HAS_TOUCH
void CipherSprint::onPressAt(int x, int y) {
    // Same argument as firewall: a finger knows which answer it wants, and
    // committing whichever chip the sweep is on turns a puzzle into a reflex
    // test. Cipher was not reported as wrong, but it is the same widget and
    // the same sweep, and leaving one of them tap-to-choose and the other
    // press-to-commit would be worse than either.
    const int hit = _chips.chipAtXY(x, y);
    if (hit >= 0) {
        _pressedChip = (uint8_t)hit;
        if (_tft) _chips.moveCursor(*_tft, hit);
    } else {
        _pressedChip = (uint8_t)_chips.cursor();
    }
    _pressPending = true;
}
#endif

// ── Drawing ───────────────────────────────────────────────────────────────

void CipherSprint::clearCard() {
    if (!_tft) return;
    _tft->fillRect(_card.x, _card.y, _card.w, _card.h, TFT_BLACK);
}

// Two centered lines inside the card. Called once per puzzle, never per frame.
void CipherSprint::drawCardLines(const char* a, const char* b, uint16_t color) {
    if (!_tft) return;
    clearCard();

    const char* line[2] = { a, b };
    int n = 0;
    for (int i = 0; i < 2; i++) if (line[i]) n++;
    if (n == 0) return;

    int y = _card.y + (_card.h - n * _cardLineH) / 2;
    if (y < _card.y) y = _card.y;

    const int textOff = (_cardLineH - games::lineHeight(_cardTextSize)) / 2;
    _tft->setTextSize(games::realTextSize(_cardTextSize));
    _tft->setTextColor(color, TFT_BLACK);

    for (int i = 0; i < 2; i++) {
        if (!line[i]) continue;
        const int w = games::textWidth(line[i], _cardTextSize);
        int x = _card.x + (_card.w - w) / 2;
        if (x < _card.x) x = _card.x;
        _tft->setCursor(x, y + (textOff > 0 ? textOff : 0));
        _tft->print(line[i]);
        y += _cardLineH;
    }
}

void CipherSprint::drawStatus(const char* text, uint16_t color) {
    if (!_tft) return;
    games::status(*_tft, text, color);
}

void CipherSprint::drawOver() {
    if (!_tft) return;
    games::clearPlay(*_tft);
    _chips.forget();
    _clock.forget();

    const games::Rect p = games::playArea();
    const int size = games::bodyTextSize();
    const int lineH = games::linePitch(size);
    int y = p.y + (p.h - lineH * 2) / 2;
    if (y < p.y) y = p.y;

    const bool clean = (_solved >= PUZZLE_LIMIT);
    games::centerText(*_tft, y, clean ? "DECRYPTED" : "LOCKED OUT",
                      size, clean ? TFT_GREEN : TFT_RED, TFT_BLACK);
    y += lineH;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu PTS  %d/%d",
             (unsigned long)_score, (int)_solved, PUZZLE_LIMIT);
    games::centerText(*_tft, y, buf, size, TFT_WHITE, TFT_BLACK);
}

// ── Round flow ────────────────────────────────────────────────────────────

void CipherSprint::showPuzzle(uint32_t nowMs) {
    makePuzzle();

    if (_tft) {
        drawCardLines(_pz->line1, _pz->line2, TFT_WHITE);
        // The labels changed under the chips, so every chip has to repaint
        // even where its state did not change.
        _chips.forget();
        _chips.drawAll(*_tft, games::CHIP_IDLE);
        _chips.moveCursor(*_tft, 0);

        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d  SCR %lu  LIFE %d",
                 (int)_index + 1, PUZZLE_LIMIT, (unsigned long)_score,
                 _lives < 0 ? 0 : (int)_lives);
        drawStatus(buf, _lives > 1 ? TFT_GREEN : TFT_ORANGE);
    }

    _phase   = PH_SOLVE;
    _phaseMs = nowMs;
    _sweepMs = nowMs;
}

void CipherSprint::mark(uint8_t choice, uint32_t nowMs) {
    const bool timedOut = (choice >= OPTIONS);
    const bool right    = !timedOut && (choice == _pz->answer);

    uint32_t bonus = 0;
    if (right) {
        const uint32_t limit = deadlineMs();
        const uint32_t spent = nowMs - _phaseMs;
        const uint32_t left  = spent >= limit ? 0 : (limit - spent);
        bonus  = 60u * left / limit;      // speed is worth something
        _score += 100u + bonus;
        _solved++;
    } else {
        _lives--;
    }

    if (_tft) {
        _clock.clear(*_tft);
        if (!timedOut && !right) _chips.draw(*_tft, choice, games::CHIP_BAD);
        // Always show the right answer, including on a timeout: a puzzle you
        // are never shown the answer to teaches nothing.
        _chips.draw(*_tft, _pz->answer, games::CHIP_GOOD);
        drawStatus(right ? "CLEAR" : (timedOut ? "TIMEOUT" : "GARBLED"),
                   right ? TFT_GREEN : TFT_RED);
    }

    _index++;
    _phase   = PH_MARK;
    _phaseMs = nowMs;
}

// ── Main update ───────────────────────────────────────────────────────────

bool CipherSprint::update(uint32_t nowMs) {
    if (!_pz) return false;

    if (nowMs - _lastMs < FRAME_MS) return true;
    _lastMs = nowMs;

    switch (_phase) {
    case PH_INTRO:
        _pressPending = false;
        if (nowMs - _phaseMs >= INTRO_MS) {
            showPuzzle(nowMs);
        }
        return true;

    case PH_SOLVE: {
        if (_pressPending) {
            _pressPending = false;
            mark(_pressedChip < OPTIONS ? _pressedChip : (uint8_t)0, nowMs);
            return true;
        }

        const uint32_t elapsed = nowMs - _phaseMs;
        const uint32_t limit   = deadlineMs();
        if (elapsed >= limit) {
            // Costs a life, and it is what lets an abandoned round finish on
            // its own rather than holding the screen forever.
            mark((uint8_t)OPTIONS, nowMs);
            return true;
        }

        if (_tft) {
            const uint32_t left = limit - elapsed;
            const uint16_t col = (left * 2 > limit) ? TFT_GREEN
                               : (left * 4 > limit) ? TFT_ORANGE : TFT_RED;
            _clock.draw(*_tft, elapsed, limit, col);
        }

        if (nowMs - _sweepMs >= dwellMs()) {
            _sweepMs = nowMs;
            if (_tft) _chips.step(*_tft);
        }
        return true;
    }

    case PH_MARK:
        _pressPending = false;
        if (nowMs - _phaseMs >= MARK_MS) {
            if (_lives <= 0 || _index >= PUZZLE_LIMIT) {
                _completed = true;      // played out, not abandoned
                _result.score      = _score;
                _result.durationMs = games::clampDuration(nowMs - _startMs);
                _result.completed  = true;
                drawOver();
                _phase   = PH_OVER;
                _phaseMs = nowMs;
            } else {
                showPuzzle(nowMs);
            }
        }
        return true;

    case PH_OVER:
    default:
        if (nowMs - _phaseMs >= OVER_HOLD_MS) {
            if (_pressPending) {
                _pressPending = false;
                return false;
            }
            // Auto-close after a generous hold so an unattended device does
            // not sit on a dead screen forever.
            if (nowMs - _phaseMs >= OVER_HOLD_MS * 4) return false;
        } else {
            _pressPending = false;   // swallow the press that ended the round
        }
        return true;
    }
}
