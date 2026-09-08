#pragma once
#include "../hal/tft_compat.h"
#include <cstdint>
#include "../config.h"

// ── HexHound - Sprite Animator ───────────────────────────────────
//
// ANIMATION TRIGGERS (used by main loop to select animations):
//   idle   - default when no activity
//   scan   - during patrol (WiFi/BLE scanning)
//   happy  - XP or food gained
//   hungry - hunger < 30
//   alert  - WARNING or CRITICAL notification active
//   hatch  - stage 1->2 transition (one-shot, non-looping)
//   type   - USB mission active (Gremlin Mode)
//   laugh  - mission complete
//
// Usage:
//   Animator anim;
//   anim.setAnimation(pup_idle_frames, PUP_IDLE_COUNT, 16, 16, 500);
//   // in loop:
//   anim.update();
//   anim.draw(tft, x, y);
// ────────────────────────────────────────────────────────────────────────────

// Draw a single sprite frame with transparency (magenta = 0xF81F skipped)
void drawSprite(TFT_eSPI& tft, const uint16_t* sprite,
                int x, int y, int w, int h);

// Draw a sprite resampled to an arbitrary destination size (nearest
// neighbour), same chroma-key transparency. drawScaledSprite() can only
// enlarge by a whole-number factor, which is fine for 16x16 pixel art but
// useless for the HD images - the evolution cutscene has to grow one from a
// spark to full size, which means arbitrary intermediate sizes.
void drawSpriteResized(TFT_eSPI& tft, const uint16_t* sprite,
                       int srcW, int srcH,
                       int x, int y, int dstW, int dstH);

// Stage sprite data accessors (for evolve cutscene - defined in animator.cpp)
const uint16_t* getStageIdleFrame(PetStage stage, int frameIdx = 0);
int getStageIdleFrameCount(PetStage stage);
int getStageSpriteSize(PetStage stage);

// ── HD stage art ───────────────────────────────────────────────────────────
// One high-resolution image per stage, baked into flash from assets/hd/ by
// scripts/gen_hd_sprites.py. It covers the RESTING pet and the evolution hero
// shot; every animated state still comes from the pixel sprites above, because
// the HD set has no motion frames.
//
// Sizes match the layout slots that already existed, so these are drop-in
// replacements rather than a re-layout. Panel size decides which set is
// compiled in (animator.cpp static_asserts that the header agrees).
#include "../board/board_profile.h"   // SCREEN_H, HEXHOUND_PANEL_ROUND
// SRC is the size the art is actually BAKED at; PX is the size it is DRAWN at.
// Every panel now has art baked for it, so the two are equal everywhere and
// drawHDArt() always takes its plain-blit path. The split is kept because it is
// what lets a panel whose art has NOT been baked yet still draw correctly, only
// softly, instead of drawing the wrong number of pixels; and because the SRC
// values are what animator.cpp checks the art header against, so a board wired
// to the wrong header fails to compile.
//
// The 480 round panel draws at exactly twice the 240 sizes, and its art is cut
// from the 400px masters, so it is true resolution rather than a 2x smear of
// the 240 set. The DRAWN sizes are unchanged from before it was baked, so no
// screen's layout moves; only the pixels got sharper.
#if HEXHOUND_PANEL_ROUND
constexpr int HD_ART_SCALE   = SCREEN_W / 240;   // 1 at 240, 2 at 480
constexpr int HD_HOME_SRC_PX = 88 * HD_ART_SCALE;
constexpr int HD_HERO_SRC_PX = 160 * HD_ART_SCALE;
constexpr int HD_HOME_PX = HD_HOME_SRC_PX;
constexpr int HD_HERO_PX = HD_HERO_SRC_PX;
#else
constexpr int HD_HOME_SRC_PX = (SCREEN_H > 100) ? 64 : 24;
constexpr int HD_HERO_SRC_PX = (SCREEN_H > 100) ? 112 : 48;
constexpr int HD_HOME_PX = HD_HOME_SRC_PX;
constexpr int HD_HERO_PX = HD_HERO_SRC_PX;
#endif

// Resting-pet image for the home screen, HD_HOME_SRC_PX square. Never null.
const uint16_t* getStageHomeHD(PetStage stage);
// Evolution hero image, HD_HERO_SRC_PX square. Never null.
const uint16_t* getStageHeroHD(PetStage stage);

// ── HD idle animation ──────────────────────────────────────────────────────
// The 480 round panel is the only panel with HD MOTION frames baked for it.
// Everywhere else the resting pet is a single frozen image, and it is the
// 480 panel where that reads worst: the pixel fallback there is a 16x16 sprite
// at scale 8, so the step down from a 176px portrait to 8px blocks is the
// largest quality drop on the board. Two frames per stage - a breath - cost
// 605 KB, which the 6.25 MB partition can carry; all eight animations would be
// 2.13 MB, so only idle is baked. See scripts/gen_hd_idle.py.
//
// Guarded because animator.h compiles on all eight boards and an unguarded
// declaration here has already been shown to move other boards' images through
// literal-pool padding alone.
#if HEXHOUND_PANEL_ROUND && (SCREEN_W == 480)
#define HEXHOUND_HD_IDLE 1
// Baked at, and drawn at, exactly the size of the resting portrait it replaces,
// so the pet does not change size when it starts breathing. animator.cpp
// static_asserts the header against this.
constexpr int HD_IDLE_PX = HD_HOME_SRC_PX;
// Frame count of the HD idle loop. It matches the pixel idle count for every
// stage, so the animator's own frame index selects an HD frame directly and the
// per-stage idle cadence in selectForStage() carries over unchanged.
int getStageIdleHDCount();
// One frame of the idle breath, HD_IDLE_PX square. Never null.
//
// These frames are OPAQUE: their background is black, not the 0xF81F chroma
// key. drawSprite() therefore writes every pixel, which is what lets two
// slightly different silhouettes alternate forever without either leaving a
// fringe of the previous frame or needing the slot cleared between them.
const uint16_t* getStageIdleHD(PetStage stage, int frameIdx);
#else
#define HEXHOUND_HD_IDLE 0
#endif

// Draws square HD art at its display size. The branch is on compile-time
// constants, so a panel whose art matches its layout still takes the plain
// drawSprite path and is unaffected.
inline void drawHDArt(TFT_eSPI& tft, const uint16_t* art,
                      int x, int y, int srcPx, int dstPx) {
    if (srcPx == dstPx) {
        drawSprite(tft, art, x, y, srcPx, srcPx);
    } else {
        drawSpriteResized(tft, art, srcPx, srcPx, x, y, dstPx, dstPx);
    }
}

// Named animation IDs for easy switching
enum AnimID : uint8_t {
    ANIM_IDLE = 0,
    ANIM_SCAN,
    ANIM_HAPPY,
    ANIM_HUNGRY,
    ANIM_ALERT,
    ANIM_HATCH,
    ANIM_TYPE,
    ANIM_LAUGH,
    ANIM_COUNT
};

class Animator {
public:
    Animator() = default;

    // Set a new animation. Resets frame counter.
    //   frames     - pointer to array of frame pointers (e.g. pup_idle_frames)
    //   frameCount - number of frames
    //   sprW, sprH - sprite dimensions (16 or 20)
    //   frameDelayMs - ms per frame
    //   loop       - true=loop forever, false=play once then hold last frame
    void setAnimation(const uint16_t* const* frames, int frameCount,
                      int sprW, int sprH,
                      int frameDelayMs, bool loop = true);

    // Advance frame if enough time has passed. Call every loop iteration.
    void update();

    // Draw current frame at screen position (x, y) with transparency.
    // scale > 1 renders each source pixel as a scale×scale block (nearest-neighbor).
    //
    // WORN COSMETICS ARE NOT DRAWN OVER THIS, on purpose. drawWornOn() places
    // overlays from an anchor table normalised to the HD art's 400x400 source
    // canvas, and the pixel sprites are framed differently inside their own box
    // (108 to 200 thousandths tighter to the figure, per stage), so the same
    // fractions land on the pup's eyes and the beast's face. Their silhouettes
    // also move between frames, which a per-stage table cannot follow. The
    // measurements and the rejected alternatives are in ui_worn.h; read that
    // before adding a drawWornOn() call after this one.
    void draw(TFT_eSPI& tft, int x, int y, int scale = 1);

    // Clear the sprite area (fill with background color). Pass the same scale
    // used for draw() so the whole enlarged footprint is cleared.
    void clear(TFT_eSPI& tft, int x, int y, uint16_t bgColor = TFT_BLACK, int scale = 1);

    // True if a one-shot animation has finished (last frame reached)
    bool isFinished() const;

    // Current frame index
    int currentFrame() const { return _frame; }

    // Sprite dimensions of current animation
    int spriteWidth()  const { return _sprW; }
    int spriteHeight() const { return _sprH; }

    // Check if an animation is set
    bool hasAnimation() const { return _frames != nullptr; }

    // Which animation is currently selected. The home screen uses this to
    // decide between the static HD portrait (resting) and the pixel-art
    // animation (everything else).
    AnimID currentAnim() const { return _anim; }

    // Select the right animation for a given stage + trigger
    // Populates the animator with the correct frames/timing
    void selectForStage(PetStage stage, AnimID anim);

    // Direct frame access (for evolve cutscene)
    const uint16_t* const* frames() const { return _frames; }

private:
    const uint16_t* const* _frames = nullptr;
    AnimID   _anim       = ANIM_IDLE;
    int      _frameCount = 0;
    int      _frame      = 0;
    int      _sprW       = 16;
    int      _sprH       = 16;
    int      _delayMs    = 500;
    bool     _loop       = true;
    bool     _finished   = false;
    uint32_t _lastFrame  = 0;
};

