#pragma once
#include "../hal/tft_compat.h"
#include "animator.h"

// ── HexHound - Home Screen UI ────────────────────────────────────

class UIHome {
public:
    static UIHome& instance();

    void init(TFT_eSPI* tft);
    void draw(bool forceRedraw = false);

    // Access the pet animator for triggering animations from main
    Animator& animator() { return _animator; }

private:
    UIHome() = default;
    void drawStatBar(int x, int y, int w, int h, int val,
                     uint16_t color, const char* label, int labelW = 26);
#if HEXHOUND_PANEL_ROUND
    // Round-panel layout family (240x240 GC9A01). Compiled only for round
    // boards; the rectangular layouts above are left completely untouched.
    void drawRound(bool forceRedraw);
    void drawRoundPet(PetStage stage, bool resting, int8_t pose, int frameNow);
#endif

    TFT_eSPI* _tft = nullptr;
    Animator  _animator;
    PetStage  _lastStage    = static_cast<PetStage>(0);
    int       _lastDrawnFrame = -1;
    // Stage whose HD portrait is currently on screen, -1 when the pixel-art
    // animator owns the pet area instead. Stops the HD image being redrawn
    // pixel-by-pixel every loop.
    int       _hdDrawnStage  = -1;
    // What the pet was WEARING when the HD slot was last painted. The resting
    // pet is cached on its stage alone, so without this a hat put on in the
    // closet would not appear until the pet next changed stage or animated.
    // 0xFFFF is "nothing drawn yet"; wornSignature() packs two item ids and
    // can never produce it, because 0xFF is not a valid id.
    uint16_t  _wornDrawn     = 0xFFFF;
    // WHICH pose the HD slot was last painted with, as a PetPose, or -1 for
    // the plain resting portrait. The pet is cached on its stage, so without
    // this a pet that got hungry, was fed, or reacted to something would keep
    // the portrait it already had. -2 is "nothing drawn yet".
    int8_t    _hdDrawnPose = -2;
    uint32_t  _lastXP = 0xFFFFFFFF;
    int16_t   _lastHunger   = -1;
    int16_t   _lastMood      = -1;
    int16_t   _lastTrust     = -1;
    int16_t   _lastMischief  = -1;
    int16_t   _lastEnergy    = -1;
    int16_t   _lastBatteryPct = -1;
};
