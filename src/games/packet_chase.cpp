#include "packet_chase.h"

#include "../hal/tft_compat.h"
#include "../ui/animator.h"     // drawSpriteResized(), getStageHomeHD()
#include "../pet/pet_core.h"     // the stage the runner should look like

#include <math.h>
#include <stdio.h>

// ── HexHound - Packet Chase Implementation ──────────────────────
//
// Frame budget notes, because this is the first thing in the firmware that
// redraws on a timer while the main loop still has to service the event bus:
//
//  * update() early-returns until FRAME_MS has elapsed, so the cost of being
//    called every loop iteration is one subtraction.
//  * Nothing is ever cleared with fillScreen during play. Each moving thing
//    erases its own previous rectangle and redraws at its new one, so the SPI
//    traffic per frame is proportional to the handful of sprites on screen and
//    not to the panel area. On the 160x80 T-Dongle a full-screen clear per
//    frame is visible as a flicker; this does not flicker.
//  * The entity pool is allocated once in begin() and freed in end(). update()
//    never touches the heap.

namespace {

// The pet body is a square of side _cell. Everything else is expressed in
// those units so the game reads the same on every panel.
constexpr uint32_t FRAME_MS       = 33;     // ~30 fps
constexpr uint32_t HOP_MS         = 560;
constexpr uint32_t READY_MS       = 900;
constexpr uint32_t OVER_HOLD_MS   = 1600;   // minimum time the score is held
constexpr int      START_LIVES    = 3;
constexpr float    RAMP_DIST_CELLS = 220.0f;  // distance over which speed doubles

constexpr uint16_t COL_PET     = TFT_CYAN;
constexpr uint16_t COL_HAZARD  = TFT_RED;
constexpr uint16_t COL_PACKET  = TFT_GREEN;
constexpr uint16_t COL_WIRE    = 0x4208;    // TFT_DARKGREY
constexpr uint16_t COL_TITLE   = TFT_CYAN;

}  // namespace

PacketChase& PacketChase::instance() {
    static PacketChase g;
    return g;
}

// ── Geometry ──────────────────────────────────────────────────────────────
//
// Vertical budget is 3 cells: one for the ground lane, two for the hop. That
// is a fixed fraction of the play area, so the layout is identical in
// proportion on all three panel families and never has to be re-tuned.

void PacketChase::computeGeometry() {
    const games::Rect p = games::playArea();

    // The ceiling is NOT scaled, and that is a correction of a change made on
    // 2026-08-30 rather than an oversight.
    //
    // Scaling it to 52 on the 480 panel doubled the pet and every obstacle,
    // and _hopH follows _cell so the hop grew with them. What did NOT grow is
    // HOP_MS. An obstacle is _cell wide and passes the pet's _cell-wide box at
    // a fixed _speed, so doubling the cell DOUBLES the time it overlaps the
    // pet - while the pet is airborne for exactly as long as before. The hop
    // stopped clearing the block. Reported from the board as "the character
    // cannot jump over them", which is the only way anyone would have found
    // it: it builds, it renders, and it looks correct in a still.
    //
    // A bigger cell needs a longer hop, and that is a retune of a game that
    // plays well, not a scale factor. The size the 480 had before was right.
    _cell = games::clampInt(p.h / 4, 6, 26);
    _hopH = _cell * 2;

    _left    = p.x;
    _right   = p.x + p.w;
    _groundY = p.y + p.h;          // the wire itself, one row below the pet
    _petX    = p.x + p.w / 6;

    // If a very short play area ever made the stack taller than the space,
    // shrink the cell rather than drawing over the header.
    while (_cell > 4 && (_cell + _hopH) > p.h - 2) {
        _cell--;
        _hopH = _cell * 2;
    }
}

int PacketChase::laneTop(uint8_t lane) const {
    return lane == 0 ? (_groundY - _cell) : (_groundY - _cell - _hopH);
}

bool PacketChase::petAirborne() const {
    // Airborne once the arc has lifted the body at least halfway, which is
    // exactly the row where the air lane's band begins. Hitbox and sprite
    // therefore agree by construction rather than by tuning.
    return _hopping && (_hopPhase * _hopH) >= (float)_hopH * 0.5f;
}

int PacketChase::petTop() const {
    const int lift = (int)(_hopPhase * (float)_hopH + 0.5f);
    return _groundY - _cell - lift;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void PacketChase::begin(TFT_eSPI* tft) {
    _tft = tft;

    if (!_ents) {
        _ents = new Ent[MAX_ENT];
    }
    for (int i = 0; i < MAX_ENT; i++) {
        _ents[i] = Ent();
    }

    computeGeometry();

    _rng.seed(millis() * 2654435761u + 0x9E3779B9u);

    _dist        = 0.0f;
    _speedBase   = (float)(_right - _left) / 2.2f;   // crosses the lane in 2.2s
    _speed       = _speedBase;
    _hopPhase    = 0.0f;
    _hopStart    = 0;
    _hopping     = false;
    _pressPending = false;
    _packets     = 0;
    _score       = 0;
    _shownScore  = 0xFFFFFFFFu;
    _lives       = START_LIVES;
    _shownLives  = -1;
    _petDrawY    = -1;
    _completed   = false;
    _phase       = PH_READY;
    _startMs     = millis();
    _lastMs      = _startMs;
    _phaseMs     = _startMs;
    _result      = MinigameResult();

    if (!_tft) return;

    // No clearPlay() here: chromeInit() blanks the whole band between the two
    // rules, which is a superset of the play area, and a clearPlay() first only
    // fills part of it twice.
    //
    // The round panel's title row is a 128px chord, so the long form would run
    // under the bezel there. Folded at compile time.
    games::chromeInit(*_tft,
                      games::panel() == games::PANEL_ROUND ? "PKT CHASE"
                                                           : "PACKET CHASE",
                      COL_TITLE);
    _tft->drawFastHLine(_left, _groundY, _right - _left, COL_WIRE);

    drawPet(petTop(), false);
    drawStatus();
    // Body size, not a hardcoded 1. At 240-relative 1 this drew 12 px wide
    // glyphs on the 480 panel, which is the size a footnote is set in, for the
    // one line that tells a first-time player what the game is. Shortened to
    // suit: at body size "HOP THE BAD ONES" is 384 px wide and the chord at
    // this row is 366, so it would have run under the bezel.
    //
    // "RED" is the accurate word - hazards are COL_HAZARD and packets are
    // COL_PACKET - and it names the thing to avoid rather than a category the
    // player has no way to identify yet.
    games::centerText(*_tft, _groundY - _cell - _hopH - 2, "HOP THE RED",
                      games::bodyTextSize(), TFT_YELLOW, TFT_BLACK);
}

void PacketChase::end() {
    if (_ents) {
        delete[] _ents;
        _ents = nullptr;
    }
    const uint32_t now = millis();
    _result.score      = _score;
    _result.durationMs = games::clampDuration(now - _startMs);
    _result.completed  = _completed;
    _result.newBest    = false;      // owned by the caller's high-score store
    _tft = nullptr;
}

void PacketChase::onPress() {
    // Consumed by update(). Nothing here draws or allocates, because this can
    // be called from the button handler between frames.
    _pressPending = true;
}

// ── Simulation ────────────────────────────────────────────────────────────

void PacketChase::spawn() {
    // Only spawn when the last thing on screen is far enough away that a hop
    // started on sight can still finish. Otherwise the game is unfair, which
    // on a one-button game is indistinguishable from broken.
    const float hopDist = _speed * (float)HOP_MS / 1000.0f;
    int rightmost = _left;
    for (int i = 0; i < MAX_ENT; i++) {
        if (_ents[i].kind == ENT_FREE) continue;
        const int edge = _ents[i].x + _cell;
        if (edge > rightmost) rightmost = edge;
    }
    const int minGap = (int)(hopDist * 1.25f) + _cell;
    if (rightmost > _right - minGap) return;

    int slot = -1;
    for (int i = 0; i < MAX_ENT; i++) {
        if (_ents[i].kind == ENT_FREE) { slot = i; break; }
    }
    if (slot < 0) return;

    Ent& e = _ents[slot];
    e.x     = (int16_t)_right;
    e.drawX = e.x;
    e.drawn = 0;
    e.lane  = (uint8_t)_rng.range(2);
    e.kind  = (uint8_t)((_rng.range(100) < 58) ? ENT_HAZARD : ENT_PACKET);
}

void PacketChase::stepWorld(uint32_t dtMs) {
    const float dt = (float)dtMs / 1000.0f;

    // Speed ramps with distance, capped at 2x so it stays readable.
    const float cells = _dist / (float)_cell;
    float ramp = 1.0f + cells / RAMP_DIST_CELLS;
    if (ramp > 2.0f) ramp = 2.0f;
    _speed = _speedBase * ramp;

    const float advance = _speed * dt;
    _dist += advance;

    const uint8_t petLane = petAirborne() ? 1 : 0;
    const int petL = _petX;
    const int petR = _petX + _cell;

    for (int i = 0; i < MAX_ENT; i++) {
        Ent& e = _ents[i];
        if (e.kind == ENT_FREE) continue;

        e.x = (int16_t)(e.x - (int)(advance + 0.5f));

        // Off the left edge: retire it.
        if (e.x + _cell < _left) {
            if (e.drawn && _tft) {
                drawEntity(e, e.drawX, true);
            }
            e.kind  = ENT_FREE;
            e.drawn = 0;
            continue;
        }

        if (e.lane != petLane) continue;
        if (e.x + _cell < petL || e.x > petR) continue;

        // Overlap in the same lane.
        if (e.kind == ENT_HAZARD) {
            _lives--;
        } else {
            _packets++;
        }
        if (e.drawn && _tft) {
            drawEntity(e, e.drawX, true);
        }
        e.kind  = ENT_FREE;
        e.drawn = 0;
    }

    _score = (uint32_t)(_dist / (float)_cell) + _packets * 5u;
    spawn();
}

// ── Drawing ───────────────────────────────────────────────────────────────

void PacketChase::drawEntity(const Ent& e, int atX, bool erase) {
    if (!_tft) return;
    const int y = laneTop(e.lane);

    // Clip to the play lane so nothing spills past the edges (and, on the round
    // panel, so nothing lands under the bezel: the play rect is inscribed).
    int x = atX;
    int w = _cell;
    if (x < _left) { w -= (_left - x); x = _left; }
    if (x + w > _right) w = _right - x;
    if (w <= 0) return;

    if (erase) {
        _tft->fillRect(x, y, w, _cell, TFT_BLACK);
        return;
    }

    if (e.kind == ENT_HAZARD) {
        _tft->fillRect(x, y, w, _cell, COL_HAZARD);
        _tft->drawLine(x, y, x + w - 1, y + _cell - 1, TFT_BLACK);
        _tft->drawLine(x, y + _cell - 1, x + w - 1, y, TFT_BLACK);
    } else {
        _tft->drawRect(x, y, w, _cell, COL_PACKET);
        const int inset = _cell / 3;
        if (w - 2 * inset > 0 && _cell - 2 * inset > 0) {
            _tft->fillRect(x + inset, y + inset,
                           w - 2 * inset, _cell - 2 * inset, COL_PACKET);
        }
    }
}

void PacketChase::drawPet(int atY, bool erase) {
    if (!_tft) return;
    if (erase) {
        _tft->fillRect(_petX, atY, _cell, _cell, TFT_BLACK);
        return;
    }

    // THE RUNNER IS THE PET, at whatever it has evolved into.
    //
    // It was a cyan box with two ears, which is readable and anonymous. The
    // whole point of the pet is that it is yours and it changes; a chase game
    // starring a generic block wastes the one thing the device has. The art is
    // already resident in flash for the home screen, the den and the closet,
    // so this costs nothing to store.
    //
    // Read fresh each frame rather than cached at begin(): a round can outlive
    // an evolution, and the runner should become what the pet became.
    //
    // THE PIXEL SPRITE, not the HD portrait, and that is the second attempt.
    //
    // The HD art is 176 px of soft shading on a charcoal body. Reduced to a 26
    // px runner by nearest neighbour it came out as a grey smudge that was
    // genuinely hard to see against the black field - rendered, looked at, and
    // rejected. The 16x16 pixel sprites are the art that was DRAWN to read at
    // this size: flat, high contrast, and already per-stage.
    //
    // This is the same conclusion ui_worn.h reaches from the other direction.
    // The two art sets are not substitutes; each is right where it was drawn to
    // be, and the HD set being newer does not make it better everywhere.
    //
    // srcW/srcH are the SPRITE's own size, never the drawn size. Passing the
    // drawn size is the mistake that once read 153 KB past the end of an array.
    const PetStage stage = PetCore::instance().state().stage;
    const int      src   = getStageSpriteSize(stage);
    if (const uint16_t* art = getStageIdleFrame(stage, 0)) {
        drawSpriteResized(*_tft, art, src, src, _petX, atY, _cell, _cell);
    }

    // A bar under the feet, in the stage's own colour.
    //
    // The runner used to be a solid cyan block and was impossible to lose
    // track of; the pet is charcoal, and on a black field at 26 px that is a
    // real loss of readability in a game where the whole task is watching it.
    // The bar restores the contrast without repainting the art, and it doubles
    // as the same grounding cue the den draws under the pet - a runner that is
    // touching the wire reads as running rather than floating.
    const uint16_t mark = STAGE_COLOR[stage - 1];
    const int barH = _cell / 8 > 0 ? _cell / 8 : 1;
    _tft->fillRect(_petX, atY + _cell - barH, _cell, barH, mark);
}

void PacketChase::drawStatus() {
    if (!_tft) return;
    // Fixed width on purpose: statusInPlace() overwrites rather than clearing,
    // so a shorter string must never be able to leave residue behind.
    unsigned long shown = (unsigned long)_score;
    if (shown > 99999ul) shown = 99999ul;
    char buf[32];
    snprintf(buf, sizeof(buf), "SCORE %-5lu LIFE %d",
             shown, _lives < 0 ? 0 : _lives);
    games::statusInPlace(*_tft, buf, _lives > 1 ? TFT_GREEN : TFT_ORANGE);
    _shownScore = _score;
    _shownLives = _lives;
}

void PacketChase::drawOver() {
    if (!_tft) return;
    games::clearPlay(*_tft);

    const games::Rect p = games::playArea();
    const int size = games::bodyTextSize();
    const int lineH = games::linePitch(size);
    int y = p.y + (p.h - lineH * 2) / 2;
    if (y < p.y) y = p.y;

    games::centerText(*_tft, y, "GAME OVER", size, TFT_RED, TFT_BLACK);
    y += lineH;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lu PTS  %lu PKT",
             (unsigned long)_score, (unsigned long)_packets);
    games::centerText(*_tft, y, buf, size, TFT_WHITE, TFT_BLACK);
}

void PacketChase::drawFrame() {
    if (!_tft) return;

    // Erase then redraw only what moved.
    for (int i = 0; i < MAX_ENT; i++) {
        Ent& e = _ents[i];
        if (e.kind == ENT_FREE) continue;
        if (e.drawn && e.drawX != e.x) {
            drawEntity(e, e.drawX, true);
            e.drawn = 0;
        }
        if (!e.drawn) {
            drawEntity(e, e.x, false);
            e.drawX = e.x;
            e.drawn = 1;
        }
    }

    const int petY = petTop();
    if (petY != _petDrawY) {
        if (_petDrawY >= 0) drawPet(_petDrawY, true);
        drawPet(petY, false);
        _petDrawY = petY;
    }

    // The wire can be nicked by an erase of a ground-lane sprite sitting one
    // row above it, so repaint the span the pet occupies each frame. One
    // drawFastHLine is cheaper than tracking which erases touched it.
    _tft->drawFastHLine(_left, _groundY, _right - _left, COL_WIRE);

    if (_score != _shownScore || _lives != _shownLives) {
        drawStatus();
    }
}

// ── Main update ───────────────────────────────────────────────────────────

bool PacketChase::update(uint32_t nowMs) {
    if (!_ents) return false;

    if (nowMs - _lastMs < FRAME_MS) return true;
    uint32_t dt = nowMs - _lastMs;
    if (dt > 120) dt = 120;          // a stall must not teleport the world
    _lastMs = nowMs;

    switch (_phase) {
    case PH_READY:
        _pressPending = false;      // no hopping before the first spawn
        if (nowMs - _phaseMs >= READY_MS) {
            if (_tft) {
                // Clear the hint line only, not the whole play area - and clear
                // the height it was actually DRAWN at. This was a hardcoded 8,
                // right for size-1 text at 240 and exactly half of it on the
                // 480 panel, so the top half of every glyph was wiped and the
                // bottom half stayed on screen for the rest of the round.
                // Entities running along the ground then chewed it into
                // fragments, which is what "text overlayed other text" was.
                _tft->fillRect(_left, _groundY - _cell - _hopH - 2,
                               _right - _left,
                               games::lineHeight(games::bodyTextSize()),
                               TFT_BLACK);
            }
            _phase = PH_RUN;
            _phaseMs = nowMs;
        }
        return true;

    case PH_RUN: {
        if (_pressPending) {
            _pressPending = false;
            if (!_hopping) {
                _hopping  = true;
                _hopStart = nowMs;
            }
        }

        if (_hopping) {
            const uint32_t t = nowMs - _hopStart;
            if (t >= HOP_MS) {
                _hopping  = false;
                _hopPhase = 0.0f;
            } else {
                _hopPhase = sinf((float)t / (float)HOP_MS * (float)PI);
            }
        }

        stepWorld(dt);
        drawFrame();

        if (_lives <= 0) {
            _completed = true;       // the round was played out, not abandoned
            _phase     = PH_OVER;
            _phaseMs   = nowMs;
            // Fill the result here as well as in end(), so result() is already
            // correct the instant update() returns false.
            _result.score      = _score;
            _result.durationMs = games::clampDuration(nowMs - _startMs);
            _result.completed  = true;
            drawOver();
            drawStatus();
        }
        return true;
    }

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
