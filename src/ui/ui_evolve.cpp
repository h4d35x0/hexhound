#include "ui_evolve.h"
#include "ui_worn.h"   // drawWornOn()
#include "ui_utils.h"
#include "ui_round.h"
#include "animator.h"
#include "../config.h"
#include "../modules/storage_module.h"
#include "../modules/touch_module.h"
#ifndef SIMULATOR_BUILD
#include <pgmspace.h>
#endif

// ── HexHound - Evolution Cutscene Implementation ───────────────

// Phase durations (ms). One generous, dramatic set for BOTH hardware and the
// simulator, so what the sim shows is exactly what runs on the device. Earlier
// the hardware ran at half these values, which felt rushed.
// NAME_REVEAL must outlast the typewriter for the longest name
// ("BEACON BEAST" and "GREMLIN MODE", 12 chars at 80ms = 960ms) plus a
// hold. It was sized for an 18-char name that no longer exists, so there
// is now more slack here than the timing needs, not less.
#define PHASE_FLASH_MS         250
// The old form stands there fully visible before anything happens to it -
// without this beat it appears and starts dissolving in the same frame, so you
// never actually get to look at it.
#define PHASE_DISSOLVE_HOLD_MS 1500
#define PHASE_DISSOLVE_MS      1000
#define PHASE_ENERGY_MS        1400
#define PHASE_NAME_REVEAL_MS   2200
#define PHASE_SPRITE_REVEAL_MS 1500
#define PHASE_FANFARE_MS       2200
#define PHASE_HOLD_MS          2800
#define PHASE_WIPE_MS          400

namespace {

// ── Panel-relative pixel constants ────────────────────────────────────────
//
// This file was written against panels whose layout constants could be plain
// literals. The round family now serves TWO panels, a 240x240 and a 480x480,
// and every literal here was silently a 240 constant. On the 480 T-RGB that
// showed up as a cutscene the owner described as "there was no evolution
// sequence": half-size text, hairline energy rings, and (see updateDissolve
// and updateHold) a hero portrait drawn from 153 KB of memory past the end of
// its own array.
//
// px() and tsz() are the identity off the round path, so every non-round board
// keeps its literal values byte for byte, and on the 240 round panel
// uiround::scaled(x) is x by construction. Only the 480 panel moves.
//
// They also keep uiround:: behind the HEXHOUND_PANEL_ROUND guard, which the
// header requires: ui_round.h compiles to nothing on a rectangular board, so a
// bare uiround:: reference here would break lilygo-t-display-s3.
int px(int at240) {
#if HEXHOUND_PANEL_ROUND
    return uiround::scaled(at240);
#else
    return at240;
#endif
}

// 240-relative text size, matching the convention in ui_round.h: a "size 2"
// caption is size 2 on the small round panel and size 4 on the big one. Raw
// setTextSize() bypasses the uiround:: text helpers, so every one of them in
// this file has to go through here.
int tsz(int size240) {
#if HEXHOUND_PANEL_ROUND
    return uiround::ts(size240);
#else
    return size240;
#endif
}

// Text cell metrics for the one GLCD font the firmware loads. Use these rather
// than `6 * size` / `8 * size`, which miss the scale and were how the fanfare
// clear band ended up 8px tall against 16px glyphs.
int cellW(int size240) { return 6 * tsz(size240); }
int cellH(int size240) { return 8 * tsz(size240); }

// The ONE place that pairs the size the hero portrait is BAKED at with the
// size it is DRAWN at. Both used to be spelled HD_HERO_PX and handed to
// drawSprite, which takes its w/h as SOURCE dimensions and indexes
// sprite[row * w + col]. On the 480 panel those differ by 2x, so the cutscene
// asked for 320x320 = 102400 pixels out of a 25600-element array and read
// 153600 bytes past its end. That is what the owner saw as "there was no
// evolution sequence": the pet replaced by a mosaic of whatever art sits next
// to it in flash, and a read that far outside a mapped region can take the
// board down outright. Routing every hero blit through here means the pairing
// cannot be got wrong twice.
//
// drawHDArt() takes its two sizes as runtime ints, so on this toolchain
// (gnu++11, no `if constexpr`) referencing it from three call sites makes GCC
// keep an out-of-line copy that nothing ever calls. Selecting the branch with
// a template specialisation instead decides it at compile time: a panel whose
// hero art is already baked at the size it is drawn at emits exactly the
// drawSprite call it always did, byte for byte, and only the 480 links the
// resize path in at all.
template <bool NeedsResize>
struct HeroBlit {
    static void go(TFT_eSPI& tft, const uint16_t* art, int x, int y) {
        drawHDArt(tft, art, x, y, HD_HERO_SRC_PX, HD_HERO_PX);
    }
};

template <>
struct HeroBlit<false> {
    static void go(TFT_eSPI& tft, const uint16_t* art, int x, int y) {
        // Selected ONLY when HD_HERO_SRC_PX == HD_HERO_PX, so both arguments
        // below are the source size - which is the only thing drawSprite's w/h
        // have ever meant. Passing the DESTINATION size there was the bug.
        // The template condition is what makes this safe: it cannot be reached
        // on a panel where the two differ, so it cannot drift away from
        // drawHDArt's own src == dst branch.
        drawSprite(tft, art, x, y, HD_HERO_SRC_PX, HD_HERO_SRC_PX);
    }
};

// Takes the STAGE as well as the art, so the pet's worn cosmetics come with it
// through the whole cutscene without any call site having to remember. The
// evolution is the one place a wearer most needs to see them: the pet changes
// shape, and a hat that vanished for the length of the cutscene and came back
// afterwards would read as having been lost in the transformation.
void drawHero(TFT_eSPI& tft, const uint16_t* art, PetStage stage, int x, int y) {
    HeroBlit<HD_HERO_SRC_PX != HD_HERO_PX>::go(tft, art, x, y);
    drawWornOn(tft, stage, x, y, HD_HERO_PX);
}

bool bigScreen() {
    return SCREEN_H > 100;
}

int centerX() {
    return SCREEN_W / 2;
}

int centerY() {
    return SCREEN_H / 2;
}

// Hero sprite scale for the evolution reveal/hold. The evolved pet should fill
// the screen for a dramatic moment: ~100-112px on the big Waveshare panel, and
// as large as the 160x80 panel can hold while leaving room for the name and
// "LEVEL UP" banner.
int heroScale(int sprSize) {
#if HEXHOUND_PANEL_ROUND
    // Round 240x240: the hero fills the inscribed square (170px), matching the
    // 160px HD hero art it hands off to.
    return (sprSize >= 20) ? 8 : 10;                    // 20->160px, 16->160px
#else
    if (bigScreen()) return (sprSize >= 20) ? 5 : 7;   // 20->100px, 16->112px
    return (sprSize >= 20) ? 2 : 3;                     // 20->40px, 16->48px
#endif
}

int energyMaxRadius() {
    int radius = (SCREEN_H < SCREEN_W ? SCREEN_H : SCREEN_W) / 2 - px(10);
    return radius < px(24) ? px(24) : radius;
}

// Stroke width of an energy ring. Two 1px circles read as a ring on a 240
// panel; on a 480 the same two pixels are a hairline you cannot see from arm's
// length, which is most of why the surge looked like nothing was happening.
int energyRingTh() {
    return px(2);
}

// Fanfare text sits in a compact band at the very top so the large hero sprite
// has room below it. The 160x80 band is tighter to fit the bigger pet.
int fanfareTopY() {
#if HEXHOUND_PANEL_ROUND
    // y=4 is behind the bezel on a circle - the chord there is zero. The
    // fanfare band starts where the glass is actually wide enough for text.
    return px(30);
#else
    return bigScreen() ? 4 : 0;
#endif
}

// Line pitch. This is a walked offset, class 3 of the scaling bugs: at 480 an
// unscaled 16px pitch stacked three 16px-tall lines 16px apart on the 240
// panel's rhythm but 32px-tall lines 16px apart on the big one, so each line's
// clear band cut through the glyphs of the line above it.
int fanfareLinePitch() {
    return px(bigScreen() ? 16 : 9);
}

int fanfareMidY() {
    return fanfareTopY() + fanfareLinePitch();
}

int fanfareBottomY() {
    return fanfareMidY() + fanfareLinePitch();
}

int fanfareSpriteY() {
#if HEXHOUND_PANEL_ROUND
    return px(78);
#else
    return bigScreen() ? 46 : 28;
#endif
}

} // namespace

// ── Singleton ─────────────────────────────────────────────────────────────

EvolveScene& EvolveScene::instance() {
    static EvolveScene scene;
    return scene;
}

void EvolveScene::init(TFT_eSPI* tft) {
    _tft = tft;
}

// ── Begin cutscene ────────────────────────────────────────────────────────

void EvolveScene::begin(PetStage fromStage, PetStage toStage) {
    // Ensure landscape mode - evolution may trigger during patrol HUD (portrait)
    uint8_t rotation = StorageModule::instance().getLandscapeRotation();
    _tft->setRotation(rotation);
    TouchModule::instance().setDisplayRotation(rotation, _tft->width(), _tft->height());

    _fromStage  = fromStage;
    _toStage    = toStage;
    _sceneStart = millis();
    _flashedWhite = false;
    enterPhase(EVOLVE_FLASH);
    Serial.printf("[Evolve] Cutscene start: stage %d -> %d\n", fromStage, toStage);
}

void EvolveScene::enterPhase(EvolvePhase p) {
    _phase      = p;
    _phaseStart = millis();
    // Every phase that shows an HD portrait paints it once and sets this.
    _heroDrawn  = false;

    switch (p) {
    case EVOLVE_FLASH:
        _flashedWhite = false;
        break;
    case EVOLVE_DISSOLVE:
        _tft->fillScreen(TFT_BLACK);
        break;
    case EVOLVE_ENERGY:
        _tft->fillScreen(TFT_BLACK);
        break;
    case EVOLVE_NAME_REVEAL:
        _tft->fillScreen(TFT_BLACK);
        _charsRevealed = 0;
        _lastCharTime  = millis();
        break;
    case EVOLVE_SPRITE_REVEAL:
        _tft->fillScreen(TFT_BLACK);
        _lastRevealScale = 0;
        _bounceStep = 0;
        _bounceTime = 0;
        _lastBounceOffset = 999;
        break;
    case EVOLVE_FANFARE:
        // Clear the reveal sprite (drawn at screen center) before the fanfare
        // redraws its own sprite lower down. Without this, both are on screen
        // at once and it looks like two pets.
        _tft->fillScreen(TFT_BLACK);
        _linesRevealed = 0;
        _lastLineTime  = millis();
        break;
    case EVOLVE_HOLD:
        _tft->fillScreen(TFT_BLACK);
        _lastPulseTime = millis();
        _pulseOn = true;
        break;
    case EVOLVE_WIPE_OUT:
        break;
    case EVOLVE_DONE:
        break;
    }
}

// ── Main update dispatcher ────────────────────────────────────────────────

void EvolveScene::update() {
    if (_phase == EVOLVE_DONE || !_tft) return;

    switch (_phase) {
    case EVOLVE_FLASH:         updateFlash();        break;
    case EVOLVE_DISSOLVE:      updateDissolve();     break;
    case EVOLVE_ENERGY:        updateEnergy();       break;
    case EVOLVE_NAME_REVEAL:   updateNameReveal();   break;
    case EVOLVE_SPRITE_REVEAL: updateSpriteReveal(); break;
    case EVOLVE_FANFARE:       updateFanfare();      break;
    case EVOLVE_HOLD:          updateHold();         break;
    case EVOLVE_WIPE_OUT:      updateWipeOut();      break;
    default: break;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 0 - FLASH (200ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateFlash() {
    uint32_t elapsed = millis() - _phaseStart;

    if (!_flashedWhite) {
        _tft->fillScreen(TFT_WHITE);
        _flashedWhite = true;
        return;
    }

    if (elapsed >= 100 && elapsed < PHASE_FLASH_MS) {
        _tft->fillScreen(TFT_BLACK);
    }

    if (elapsed >= PHASE_FLASH_MS) {
        enterPhase(EVOLVE_DISSOLVE);
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 1 - HOLD THE OLD FORM, THEN DISSOLVE OUT
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateDissolve() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_DISSOLVE_HOLD_MS + PHASE_DISSOLVE_MS) {
        _tft->fillScreen(TFT_BLACK);
        enterPhase(EVOLVE_ENERGY);
        return;
    }

    const int size = HD_HERO_PX;
    int ox = centerX() - size / 2;
    int oy = centerY() - size / 2;

    // The old form is the HD portrait, dissolved away in cells rather than in
    // source pixels: at 112x112 a per-pixel pass would be 12544 draw calls per
    // frame. Paint it once, then black out each cell as its moment arrives.
    //
    // drawHero(), NOT drawSprite - see the note on drawHero() above. This was
    // the first of the two out-of-bounds reads.
    if (!_heroDrawn) {
        _tft->fillScreen(TFT_BLACK);
        drawHero(*_tft, getStageHeroHD(_fromStage), _fromStage, ox, oy);
        _heroDrawn = true;
        _lastDissolveMs = 0;
    }

    // Hold beat: the pet just stands there, whole, so you can see who is about
    // to change. Nothing dissolves until this has elapsed.
    if (elapsed < PHASE_DISSOLVE_HOLD_MS) {
        return;
    }
    uint32_t dissolveElapsed = elapsed - PHASE_DISSOLVE_HOLD_MS;

    const int cells = 16;
    const int cell  = size / cells;
    for (int i = 0; i < cells * cells; i++) {
        // Knuth multiplicative hash - determines when this cell dissolves
        uint32_t dissolveTime =
            ((uint32_t)(i * 2654435761u) >> 16) % PHASE_DISSOLVE_MS;
        if (dissolveTime >= _lastDissolveMs && dissolveTime < dissolveElapsed) {
            _tft->fillRect(ox + (i % cells) * cell, oy + (i / cells) * cell,
                           cell, cell, TFT_BLACK);
        }
    }
    _lastDissolveMs = dissolveElapsed;
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 2 - ENERGY SURGE (800ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateEnergy() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_ENERGY_MS) {
        _tft->fillScreen(TFT_BLACK);
        enterPhase(EVOLVE_NAME_REVEAL);
        return;
    }

    _tft->fillScreen(TFT_BLACK);

    // 3 rings expanding outward, offset by 200ms each
    uint16_t stageColor = STAGE_COLOR[_toStage - 1];

    for (int ring = 0; ring < 3; ring++) {
        int ringOffset = ring * 200;
        if ((int)elapsed < ringOffset) continue;

        int ringElapsed = (int)elapsed - ringOffset;
        int maxRadius   = energyMaxRadius();
        int radius      = (ringElapsed * maxRadius) / (PHASE_ENERGY_MS - ringOffset);
        if (radius > maxRadius) radius = maxRadius;

        // Color cycles: electric blue -> cyan -> white
        uint16_t ringColor;
        if (ringElapsed < 250) {
            ringColor = 0x035F;  // electric blue
        } else if (ringElapsed < 500) {
            ringColor = 0x07FF;  // cyan
        } else {
            ringColor = TFT_WHITE;
        }

        // Dim rings as they expand
        uint8_t brightness = 4 - (uint8_t)(radius * 3 / maxRadius);
        if (brightness < 1) brightness = 1;
        ringColor = dimColor(ringColor, brightness);

        drawRing(centerX(), centerY(), radius, ringColor);
    }

    // Center glow dot in stage color
    _tft->fillCircle(centerX(), centerY(), px(3), stageColor);
}

void EvolveScene::drawRing(int cx, int cy, int radius, uint16_t color) {
    if (radius < 1) return;
    // Draw ring as circle outline (approximated with drawCircle)
    _tft->drawCircle(cx, cy, radius, color);
    if (radius > 1) {
        _tft->drawCircle(cx, cy, radius - 1, color);
    }
    // Those two pixels ARE the ring on a 240 panel. On the 480 they are a
    // hairline you cannot see, so the stroke scales with the panel. Starting
    // at t=2 keeps the 240 build byte-identical: energyRingTh() is 2 there, so
    // this loop has an empty range and compiles away.
    for (int t = 2; t < energyRingTh(); t++) {
        if (radius - t < 1) break;
        _tft->drawCircle(cx, cy, radius - t, color);
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 3 - STAGE NAME REVEAL (1000ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateNameReveal() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_NAME_REVEAL_MS) {
        enterPhase(EVOLVE_SPRITE_REVEAL);
        return;
    }

    const char* name = STAGE_NAMES[_toStage - 1];
    int nameLen = strlen(name);

    // Typewriter: one char every 80ms
    uint32_t now = millis();
    if (_charsRevealed < nameLen && (now - _lastCharTime >= 80)) {
        _charsRevealed++;
        _lastCharTime = now;

        // Pick the largest text size whose full name fits the screen width, so
        // long names ("BEACON BEAST") do not run off both edges on 160px.
        // Sizes here are 240-RELATIVE: cellW() applies the panel's text scale,
        // so the fit test and the glyph metrics agree on both round panels.
        // Previously this measured 12px per char and then called
        // setTextSize(2) raw, which on the 480 drew the name at half the size
        // it should have been - a caption lost in the middle of the glass.
        int nameSize = 2;
        if (nameLen * cellW(2) > SCREEN_W - px(8)) nameSize = 1;
        int charW = cellW(nameSize);
        int fullW = nameLen * charW;
        int startX = centerX() - fullW / 2;
        if (startX < px(2)) startX = px(2);
        int startY = centerY() - cellH(nameSize) / 2;  // vertically center

        // Draw entire revealed portion (overdraw is fine, no flicker)
        _tft->setTextSize(tsz(nameSize));
        _tft->setTextColor(0x07FF, TFT_BLACK);  // electric blue on black
        _tft->setCursor(startX, startY);
        for (int i = 0; i < _charsRevealed; i++) {
            _tft->print(name[i]);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 4 - NEW SPRITE REVEAL (800ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateSpriteReveal() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_SPRITE_REVEAL_MS) {
        enterPhase(EVOLVE_FANFARE);
        return;
    }

    const uint16_t* sprite = getStageHeroHD(_toStage);

    // The reveal grows the new form from a spark to its full hero size. These
    // are HD pixel sizes now, not integer sprite scales, so the growth stays
    // high-resolution the whole way up instead of starting as chunky blocks.
    int s1 = HD_HERO_PX / 4;      // first pop
    int s2 = (HD_HERO_PX * 5) / 8;  // growing
    int s3 = HD_HERO_PX;            // full hero size

    // 4 steps: 200ms each: dot, small, growing, full + bounce.
    int step = (int)(elapsed / 200);
    if (step > 3) step = 3;

    if (step == 0) {
        // Glowing dot - energy coalescing
        if (_lastRevealScale != 0) {
            _tft->fillScreen(TFT_BLACK);
        }
        _lastRevealScale = 0;
        uint16_t color = STAGE_COLOR[_toStage - 1];
        // The spark the new form grows out of. Unscaled it is 4px on a panel
        // whose next reveal step is 80px, so the sequence appeared to start
        // from nothing at all.
        _tft->fillRect(centerX() - px(2), centerY() - px(2), px(4), px(4),
                       color);
        _tft->drawRect(centerX() - px(3), centerY() - px(3), px(6), px(6),
                       dimColor(color, 2));

    } else if (step == 1) {
        if (_lastRevealScale != s1) {
            _tft->fillScreen(TFT_BLACK);
            _lastRevealScale = s1;
            drawSpriteResized(*_tft, sprite, HD_HERO_SRC_PX, HD_HERO_SRC_PX,
                              centerX() - s1 / 2, centerY() - s1 / 2, s1, s1);
            drawWornOn(*_tft, _toStage, centerX() - s1 / 2, centerY() - s1 / 2, s1);
        }

    } else if (step == 2) {
        if (_lastRevealScale != s2) {
            _tft->fillScreen(TFT_BLACK);
            _lastRevealScale = s2;
            drawSpriteResized(*_tft, sprite, HD_HERO_SRC_PX, HD_HERO_SRC_PX,
                              centerX() - s2 / 2, centerY() - s2 / 2, s2, s2);
            drawWornOn(*_tft, _toStage, centerX() - s2 / 2, centerY() - s2 / 2, s2);
        }

    } else {
        // Step 3: land at full hero size with a bounce.
        if (_bounceStep == 0) {
            _bounceTime = millis();
            _bounceStep = 1;
            _tft->fillScreen(TFT_BLACK);  // clear the smaller step-2 sprite
            _lastRevealScale = s3;
        }

        uint32_t bounceElapsed = millis() - _bounceTime;
        // Bounce travel is a fraction of the sprite, so it scales with it: 8px
        // under a 320px hero is not a landing, it is a twitch.
        int amp = px(bigScreen() ? 8 : 3);
        int yOffset = 0;
        if (bounceElapsed < 70) {
            yOffset = -amp;  // up
        } else if (bounceElapsed < 140) {
            yOffset = amp;   // down
        } else {
            yOffset = 0;     // settle
        }

        // Only repaint when the bounce actually moves the sprite: a full
        // clear + HD redraw every frame would crawl over SPI and flicker.
        if (yOffset != _lastBounceOffset) {
            int ox = centerX() - s3 / 2;
            int oy = centerY() - s3 / 2 + yOffset;

            // Clear sprite area (slightly larger to cover bounce)
            int pad = amp + px(2);
            _tft->fillRect(centerX() - s3 / 2, centerY() - s3 / 2 - pad,
                           s3, s3 + 2 * pad, TFT_BLACK);
            drawSpriteResized(*_tft, sprite, HD_HERO_SRC_PX, HD_HERO_SRC_PX,
                              ox, oy, s3, s3);
            drawWornOn(*_tft, _toStage, ox, oy, s3);
            _lastBounceOffset = yOffset;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 5 - STAT FANFARE (1200ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateFanfare() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_FANFARE_MS) {
        enterPhase(EVOLVE_HOLD);
        return;
    }

    // 3 lines, each slides in from left over 200ms, 300ms apart
    uint32_t now = millis();

    const char* abilityText = STAGE_ABILITY[_toStage - 1];

    struct FanfareLine {
        const char* text;
        uint16_t color;
        int y;
    };

    FanfareLine lines[3] = {
        { "+ ABILITIES UNLOCKED", 0x07E0, fanfareTopY() },
        { abilityText,            0x07E0, fanfareMidY() },
        { "XP BONUS +50",         0xFD20, fanfareBottomY() },
    };

    _tft->setTextSize(tsz(1));

    for (int i = 0; i < 3; i++) {
        int lineDelay = i * 300;
        if ((int)elapsed < lineDelay) continue;

        int lineElapsed = (int)elapsed - lineDelay;
        int slideMs = 200;

        // Slide from offscreen-left to the final centered position.
        int textW = strlen(lines[i].text) * cellW(1);
        int finalX = centerX() - textW / 2;
        int slideFromX = px(-20);

        int currentX;
        if (lineElapsed < slideMs) {
            // Sliding in
            currentX = slideFromX +
                       ((finalX - slideFromX) * lineElapsed) / slideMs;
        } else {
            currentX = finalX;
        }

        // Clear this line's row first so the slide doesn't leave a trail of
        // overlapping copies as it moves across the screen.
        //
        // The band height MUST be the height of the glyphs it is clearing. A
        // literal 8 is the 240 panel's cell height; on the 480 the glyphs are
        // 16 tall, so the band cleared only the top half of its own line and
        // the next line's band cut a stripe through the one above it. This is
        // the same shape of bug that left the home-screen pet looking cropped.
        _tft->fillRect(0, lines[i].y, SCREEN_W, cellH(1), TFT_BLACK);

        _tft->setTextColor(lines[i].color, TFT_BLACK);
        _tft->setCursor(currentX, lines[i].y);
        _tft->print(lines[i].text);
    }

    // Keep the hero large and visible below the fanfare text. Drawn once per
    // phase, not per frame: only the text lines animate here, and repainting
    // 112x112 pixels every loop iteration is slow enough over SPI to make the
    // whole phase stutter.
    if (!_heroDrawn) {
        drawHero(*_tft, getStageHeroHD(_toStage), _toStage,
                 centerX() - HD_HERO_PX / 2, fanfareSpriteY());
        _heroDrawn = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 6 - HOLD (1500ms, button skippable)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateHold() {
    uint32_t elapsed = millis() - _phaseStart;
    uint32_t now = millis();

    // Button press skips remaining hold time
    if (elapsed > 200 &&
        (digitalRead(PIN_BUTTON) == LOW || TouchModule::instance().isPressed())) {
        enterPhase(EVOLVE_WIPE_OUT);
        return;
    }

    if (elapsed >= PHASE_HOLD_MS) {
        enterPhase(EVOLVE_WIPE_OUT);
        return;
    }

    // Top: stage name header (larger on the big panel)
    const char* name = STAGE_NAMES[_toStage - 1];
#if HEXHOUND_PANEL_ROUND
    // The longest name at size 2 is 144px ("BEACON BEAST"); the chord near
    // the top of the glass is under 100px, so the round panel always uses
    // size 1 here and sits the name low enough to clear the bezel.
    int nameSize = 1;
#else
    int nameSize = bigScreen() ? 2 : 1;
#endif
    int nameW = strlen(name) * cellW(nameSize);
    _tft->setTextSize(tsz(nameSize));
    _tft->setTextColor(STAGE_COLOR[_toStage - 1], TFT_BLACK);
#if HEXHOUND_PANEL_ROUND
    // y=2 is behind the bezel; y=24 gives a 122px chord, enough for the
    // longest stage name at size 1 (18 chars = 108px). Both the row and the
    // width are panel-relative, so the 480 gets the same fraction of glass.
    _tft->setCursor(centerX() - nameW / 2, px(24));
#else
    _tft->setCursor(centerX() - nameW / 2, 2);
#endif
    _tft->print(name);

    // Center: the HD hero portrait. It replaces the old idle-frame cycling -
    // the HD set has no motion frames, and repainting 112x112 pixels twice a
    // second over SPI would flicker anyway. The LEVEL UP banner below still
    // pulses, so the screen is not static.
    // Draw once, tracked by a flag rather than "elapsed < 50ms": on hardware a
    // single loop iteration can easily exceed 50ms, and then the hero never
    // gets painted at all.
    if (!_heroDrawn) {
        _heroDrawn = true;
        int ox = centerX() - HD_HERO_PX / 2;
        // Centered vertically on the big panel (nudged up to clear LEVEL UP);
        // near the top on the compact panel.
#if HEXHOUND_PANEL_ROUND
        // 160px hero in a 240 circle: 36..196 leaves the name above and the
        // LEVEL UP banner below, both on rows wide enough to hold them. The
        // 480 doubles both the row and the hero, so the composition holds.
        int oy = px(36);
#else
        int oy = bigScreen() ? ((SCREEN_H - HD_HERO_PX) / 2 - 6) : 10;
#endif
        _tft->fillRect(ox, oy, HD_HERO_PX, HD_HERO_PX, TFT_BLACK);
        // drawHero(), NOT drawSprite - see the note on drawHero() above. This
        // was the second of the two out-of-bounds reads, and the one that
        // wrecked the frame the owner was meant to remember.
        drawHero(*_tft, getStageHeroHD(_toStage), _toStage, ox, oy);
    }

    // Bottom: "LEVEL UP" pulsing between cyan and green
    if (now - _lastPulseTime >= 300) {
        _lastPulseTime = now;
        _pulseOn = !_pulseOn;
    }

    // "LEVEL UP" banner at the bottom (sits just under the hero sprite).
    uint16_t pulseColor = _pulseOn ? 0x07FF : 0x07E0;
    const char* levelUp = "LEVEL UP";
    int luW = strlen(levelUp) * cellW(2);
    _tft->setTextSize(tsz(2));
    _tft->setTextColor(pulseColor, TFT_BLACK);
#if HEXHOUND_PANEL_ROUND
    // SCREEN_H-26 = 214 would clip: the chord at row 229 is only 66px and the
    // banner is 96px. y=202 is the lowest row that fits it whole (119px). At
    // 480 that is row 404 with a 192px banner against a 236px chord.
    _tft->setCursor(centerX() - luW / 2, px(202));
#else
    _tft->setCursor(centerX() - luW / 2, SCREEN_H - (bigScreen() ? 26 : 18));
#endif
    _tft->print(levelUp);
}

// ══════════════════════════════════════════════════════════════════════════
//  PHASE 7 - TRANSITION OUT (300ms)
// ══════════════════════════════════════════════════════════════════════════

void EvolveScene::updateWipeOut() {
    uint32_t elapsed = millis() - _phaseStart;

    if (elapsed >= PHASE_WIPE_MS) {
        _tft->fillScreen(TFT_BLACK);
        enterPhase(EVOLVE_DONE);
        Serial.println("[Evolve] Cutscene complete");
        return;
    }

    // Top-to-bottom wipe: fill rows progressively
    int rowsToFill = (int)((elapsed * SCREEN_H) / PHASE_WIPE_MS);

    // Only fill new rows since last call (avoid redrawing)
    static int lastRow = 0;
    if (elapsed < 10) lastRow = 0;  // reset on phase entry

    if (rowsToFill > lastRow) {
        _tft->fillRect(0, lastRow, SCREEN_W, rowsToFill - lastRow, TFT_BLACK);
        lastRow = rowsToFill;
    }
}
