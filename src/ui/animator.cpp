#include "animator.h"
#include "ui_utils.h"  // drawScaledSprite
#include "sprites.h"  // include sprite data from ONE .cpp only
// HD stage art, sized for this board's panel. Same one-TU rule as sprites.h.
#if HEXHOUND_PANEL_ROUND
#if SCREEN_W == 480
// The 480 round panel has its own set, cut from the 400px masters. Sharing the
// 240 one would mean drawing 88px of art across 176px of glass.
#include "sprites_hd_round480.h"
// ...and the only set of HD MOTION frames, for the idle breath.
#include "sprites_hd_idle_round480.h"
#else
#include "sprites_hd_round.h"
#endif
#elif (SCREEN_H > 100)
#include "sprites_hd_big.h"
#else
#include "sprites_hd_small.h"
#endif
// Checked against the BAKED size, not the drawn size. Every panel now has art
// baked at its drawn size, so these also prove that a board wired to the wrong
// art header fails to compile rather than silently drawing at half scale.
static_assert(HD_HOME_SIZE == HD_HOME_SRC_PX,
              "HD art header does not match HD_HOME_SRC_PX - re-run scripts/gen_hd_sprites.py");
static_assert(HD_HERO_SIZE == HD_HERO_SRC_PX,
              "HD art header does not match HD_HERO_SRC_PX - re-run scripts/gen_hd_sprites.py");
#if HEXHOUND_HD_IDLE
// Same reason, and the same failure it prevents: drawSprite() is handed
// HD_IDLE_PX as a SOURCE dimension, so a header baked at a different size would
// read past the end of the array rather than draw small. That exact mistake
// once read 153 KB past the hero art and hard-faulted the board.
static_assert(HD_IDLE_SIZE == HD_IDLE_PX,
              "HD idle header does not match HD_IDLE_PX - re-run scripts/gen_hd_idle.py");
// Callers reduce the animator's frame index modulo this, so an empty set would
// be a division by zero rather than a still pet.
static_assert(HD_IDLE_COUNT > 0, "HD idle header has no frames");
#endif
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// ── HexHound - Animator Implementation ──────────────────────────

// ── drawSprite: render with magenta transparency ──────────────────────────

void drawSprite(TFT_eSPI& tft, const uint16_t* sprite,
                int x, int y, int w, int h) {
    // Runs of the same colour go out as one fillRect instead of one call per
    // pixel. It costs nothing on the 16x16 pixel art, and it is what makes the
    // HD portraits usable: on the vendor esp_lcd path every drawPixel is a
    // separate bus transaction, so a 112x112 hero shot would otherwise be
    // 12544 of them and visibly paint itself in.
#ifdef HEXHOUND_DIAG_SPRITE_PER_PIXEL
    // Diagnostic: the pre-HD per-pixel path, for isolating vendor-panel issues.
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t color = pgm_read_word(&sprite[row * w + col]);
            if (color != TRANSPARENT_COLOR) {
                tft.drawPixel(x + col, y + row, color);
            }
        }
    }
#else
    for (int row = 0; row < h; row++) {
        int col = 0;
        while (col < w) {
            uint16_t color = pgm_read_word(&sprite[row * w + col]);
            int runStart = col;
            while (col < w && pgm_read_word(&sprite[row * w + col]) == color) {
                col++;
            }
            if (color != TRANSPARENT_COLOR) {
                tft.fillRect(x + runStart, y + row, col - runStart, 1, color);
            }
        }
    }
#endif
}

void drawSpriteResized(TFT_eSPI& tft, const uint16_t* sprite,
                       int srcW, int srcH,
                       int x, int y, int dstW, int dstH) {
    if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) return;

    for (int row = 0; row < dstH; row++) {
        const uint16_t* srcRow = &sprite[((row * srcH) / dstH) * srcW];
        int col = 0;
        while (col < dstW) {
            uint16_t color = pgm_read_word(&srcRow[(col * srcW) / dstW]);
            int runStart = col;
            while (col < dstW &&
                   pgm_read_word(&srcRow[(col * srcW) / dstW]) == color) {
                col++;
            }
            if (color != TRANSPARENT_COLOR) {
                tft.fillRect(x + runStart, y + row, col - runStart, 1, color);
            }
        }
    }
}

// ── Animator methods ──────────────────────────────────────────────────────

void Animator::setAnimation(const uint16_t* const* frames, int frameCount,
                            int sprW, int sprH,
                            int frameDelayMs, bool loop) {
    // Don't restart if same animation is already playing
    if (_frames == frames) return;

    _frames     = frames;
    _frameCount = frameCount;
    _sprW       = sprW;
    _sprH       = sprH;
    _delayMs    = frameDelayMs;
    _loop       = loop;
    _frame      = 0;
    _finished   = false;
    _lastFrame  = millis();
}

void Animator::update() {
    if (!_frames || _finished) return;

    uint32_t now = millis();
    if (now - _lastFrame < (uint32_t)_delayMs) return;

    _lastFrame = now;
    _frame++;

    if (_frame >= _frameCount) {
        if (_loop) {
            _frame = 0;
        } else {
            _frame = _frameCount - 1;
            _finished = true;
        }
    }
}

void Animator::draw(TFT_eSPI& tft, int x, int y, int scale) {
    if (!_frames || _frame >= _frameCount) return;
    if (scale <= 1) {
        drawSprite(tft, _frames[_frame], x, y, _sprW, _sprH);
    } else {
        drawScaledSprite(tft, _frames[_frame], _sprW, _sprH, x, y, scale,
                         TRANSPARENT_COLOR);
    }
}

void Animator::clear(TFT_eSPI& tft, int x, int y, uint16_t bgColor, int scale) {
    if (scale < 1) scale = 1;
    tft.fillRect(x, y, _sprW * scale, _sprH * scale, bgColor);
}

bool Animator::isFinished() const {
    return _finished;
}

// ── Stage sprite data accessors (for evolve cutscene) ─────────────────────

const uint16_t* getStageIdleFrame(PetStage stage, int frameIdx) {
    switch (stage) {
    case STAGE_EGG:          return egg_idle_frames[frameIdx % EGG_IDLE_COUNT];
    case STAGE_PACKET_PUP:   return pup_idle_frames[frameIdx % PUP_IDLE_COUNT];
    case STAGE_BEACON_BEAST: return beast_idle_frames[frameIdx % BEAST_IDLE_COUNT];
    case STAGE_GREMLIN:      return gremlin_idle_frames[frameIdx % GREMLIN_IDLE_COUNT];
    case STAGE_SENTINEL:     return sentinel_idle_frames[frameIdx % SENTINEL_IDLE_COUNT];
    default:                 return egg_idle_frames[0];
    }
}

int getStageIdleFrameCount(PetStage stage) {
    switch (stage) {
    case STAGE_EGG:          return EGG_IDLE_COUNT;
    case STAGE_PACKET_PUP:   return PUP_IDLE_COUNT;
    case STAGE_BEACON_BEAST: return BEAST_IDLE_COUNT;
    case STAGE_GREMLIN:      return GREMLIN_IDLE_COUNT;
    case STAGE_SENTINEL:     return SENTINEL_IDLE_COUNT;
    default:                 return EGG_IDLE_COUNT;
    }
}

int getStageSpriteSize(PetStage stage) {
    return (stage == STAGE_SENTINEL) ? 20 : 16;
}

// ── HD stage art accessors ────────────────────────────────────────────────

static int hdStageIndex(PetStage stage) {
    switch (stage) {
    case STAGE_EGG:          return 0;
    case STAGE_PACKET_PUP:   return 1;
    case STAGE_BEACON_BEAST: return 2;
    case STAGE_GREMLIN:      return 3;
    case STAGE_SENTINEL:     return 4;
    default:                 return 0;
    }
}

const uint16_t* getStageHomeHD(PetStage stage) {
    return hd_home_by_stage[hdStageIndex(stage)];
}

const uint16_t* getStageHeroHD(PetStage stage) {
    return hd_hero_by_stage[hdStageIndex(stage)];
}

#if HEXHOUND_HD_IDLE
int getStageIdleHDCount() {
    return HD_IDLE_COUNT;
}

const uint16_t* getStageIdleHD(PetStage stage, int frameIdx) {
    // The caller passes the animator's frame index straight through, and that
    // index belongs to whichever pixel animation is loaded - so wrap it here
    // rather than trusting it to be in range.
    if (frameIdx < 0) frameIdx = 0;
    return hd_idle_by_stage[hdStageIndex(stage)][frameIdx % HD_IDLE_COUNT];
}
#endif

// ── selectForStage: map stage + animation ID to correct frame data ────────

void Animator::selectForStage(PetStage stage, AnimID anim) {
    _anim = anim;

    // Default timing
    int delay = 500;
    bool loop = true;

    switch (stage) {

    // ── Stage 1: Egg ──────────────────────────────────────────────────
    case STAGE_EGG:
        switch (anim) {
        case ANIM_HATCH:
            setAnimation(egg_hatch_frames, EGG_HATCH_COUNT, 16, 16, 300, false);
            return;
        default: // idle/everything else
            setAnimation(egg_idle_frames, EGG_IDLE_COUNT, 16, 16, 800);
            return;
        }

    // ── Stage 2: Packet Pup ──────────────────────────────────────────
    case STAGE_PACKET_PUP:
        switch (anim) {
        case ANIM_SCAN:
            setAnimation(pup_scan_frames, PUP_SCAN_COUNT, 16, 16, 400);
            return;
        case ANIM_HAPPY:
            setAnimation(pup_happy_frames, PUP_HAPPY_COUNT, 16, 16, 300);
            return;
        case ANIM_HUNGRY:
            setAnimation(pup_hungry_frames, PUP_HUNGRY_COUNT, 16, 16, 700);
            return;
        default:
            setAnimation(pup_idle_frames, PUP_IDLE_COUNT, 16, 16, 600);
            return;
        }

    // ── Stage 3: Beacon Beast ────────────────────────────────────────
    case STAGE_BEACON_BEAST:
        switch (anim) {
        case ANIM_SCAN:
            setAnimation(beast_scan_frames, BEAST_SCAN_COUNT, 16, 16, 350);
            return;
        case ANIM_ALERT:
            setAnimation(beast_alert_frames, BEAST_ALERT_COUNT, 16, 16, 250);
            return;
        case ANIM_HAPPY:
            setAnimation(beast_scan_frames, BEAST_SCAN_COUNT, 16, 16, 200);
            return;
        case ANIM_HUNGRY:
            setAnimation(beast_idle_frames, BEAST_IDLE_COUNT, 16, 16, 900);
            return;
        default:
            setAnimation(beast_idle_frames, BEAST_IDLE_COUNT, 16, 16, 600);
            return;
        }

    // ── Stage 4: Gremlin Mode ────────────────────────────────────────
    case STAGE_GREMLIN:
        switch (anim) {
        case ANIM_TYPE:
            setAnimation(gremlin_type_frames, GREMLIN_TYPE_COUNT, 16, 16, 200);
            return;
        case ANIM_LAUGH:
            setAnimation(gremlin_laugh_frames, GREMLIN_LAUGH_COUNT, 16, 16, 250);
            return;
        case ANIM_SCAN:
            setAnimation(gremlin_idle_frames, GREMLIN_IDLE_COUNT, 16, 16, 300);
            return;
        case ANIM_ALERT:
            setAnimation(gremlin_idle_frames, GREMLIN_IDLE_COUNT, 16, 16, 150);
            return;
        case ANIM_HAPPY:
            setAnimation(gremlin_laugh_frames, GREMLIN_LAUGH_COUNT, 16, 16, 300);
            return;
        case ANIM_HUNGRY:
            setAnimation(gremlin_idle_frames, GREMLIN_IDLE_COUNT, 16, 16, 800);
            return;
        default:
            setAnimation(gremlin_idle_frames, GREMLIN_IDLE_COUNT, 16, 16, 500);
            return;
        }

    // ── Stage 5: Sentinel ──────────────────────────────────
    case STAGE_SENTINEL:
        switch (anim) {
        case ANIM_SCAN:
            setAnimation(sentinel_guard_frames, SENTINEL_GUARD_COUNT, 20, 20, 400);
            return;
        case ANIM_ALERT:
            setAnimation(sentinel_alert_frames, SENTINEL_ALERT_COUNT, 20, 20, 200);
            return;
        case ANIM_HAPPY:
            setAnimation(sentinel_guard_frames, SENTINEL_GUARD_COUNT, 20, 20, 250);
            return;
        case ANIM_HUNGRY:
            setAnimation(sentinel_idle_frames, SENTINEL_IDLE_COUNT, 20, 20, 1000);
            return;
        default:
            setAnimation(sentinel_idle_frames, SENTINEL_IDLE_COUNT, 20, 20, 700);
            return;
        }

    default:
        setAnimation(egg_idle_frames, EGG_IDLE_COUNT, 16, 16, 800);
        return;
    }
}
