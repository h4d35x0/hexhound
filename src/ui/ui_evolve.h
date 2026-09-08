#pragma once
#include "../hal/tft_compat.h"
#include "../config.h"

// ── HexHound - Evolution Cutscene ──────────────────────────────
// Full-screen cinematic transition when the pet evolves.
// Fires at most 4 times per device lifetime. Make it memorable.

enum EvolvePhase : uint8_t {
    EVOLVE_FLASH = 0,       // 200ms - white/black camera flash
    EVOLVE_DISSOLVE,        // 600ms - old sprite pixel dissolve to black
    EVOLVE_ENERGY,          // 800ms - expanding energy rings from center
    EVOLVE_NAME_REVEAL,     // 1000ms - typewriter stage name
    EVOLVE_SPRITE_REVEAL,   // 800ms - new sprite scales up + bounce
    EVOLVE_FANFARE,         // 1200ms - stat lines slide in
    EVOLVE_HOLD,            // 1500ms - final composition, button skippable
    EVOLVE_WIPE_OUT,        // 300ms - top-to-bottom wipe
    EVOLVE_DONE
};

class EvolveScene {
public:
    static EvolveScene& instance();

    void init(TFT_eSPI* tft);

    // Start cutscene. Call once when evolution is triggered.
    void begin(PetStage fromStage, PetStage toStage);

    // Call every loop iteration while active. Handles timing + drawing.
    void update();

    // True when the entire cutscene is finished.
    bool isComplete() const { return _phase == EVOLVE_DONE; }

    // Current phase (for LED control from main loop)
    EvolvePhase phase() const { return _phase; }

private:
    EvolveScene() = default;

    void enterPhase(EvolvePhase p);

    // Phase renderers
    void updateFlash();
    void updateDissolve();
    void updateEnergy();
    void updateNameReveal();
    void updateSpriteReveal();
    void updateFanfare();
    void updateHold();
    void updateWipeOut();

    // Ring drawing helper
    void drawRing(int cx, int cy, int radius, uint16_t color);

    TFT_eSPI* _tft = nullptr;

    EvolvePhase _phase     = EVOLVE_DONE;
    PetStage    _fromStage = STAGE_EGG;
    PetStage    _toStage   = STAGE_PACKET_PUP;
    uint32_t    _phaseStart = 0;   // millis() when current phase began
    uint32_t    _sceneStart = 0;   // millis() when begin() was called

    // Dissolve state
    bool _flashedWhite = false;

    // Name reveal state
    int _charsRevealed = 0;
    uint32_t _lastCharTime = 0;

    // Sprite reveal state
    int _lastRevealScale = 0;
    int _bounceStep = 0;
    uint32_t _bounceTime = 0;
    int _lastBounceOffset = 999;   // forces the first bounce frame to draw

    // HD hero state. The HD portraits are expensive enough that each phase
    // paints one exactly once; enterPhase() clears this.
    bool _heroDrawn = false;
    uint32_t _lastDissolveMs = 0;

    // Fanfare state
    int _linesRevealed = 0;
    uint32_t _lastLineTime = 0;

    // Hold state
    uint32_t _lastPulseTime = 0;
    bool _pulseOn = true;
};

