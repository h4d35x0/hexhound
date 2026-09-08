#pragma once
#include "../hal/tft_compat.h"
#include "animator.h"
#include "../config.h"

// ── HexHound - Live Patrol HUD ──────────────────────────────────
//
// Runs DURING active scanning. Portrait mode (80×160).
// Shows animated radar ring, live discovery dots, real-time counters,
// phase progress bars, and pet sprite reacting to discoveries.
//
// LAYOUT (80×160 portrait):
//   y=0-11    Header bar: "◈ HEXHOUND [stage]"
//   y=12-95   Radar ring (radius 18, center 40,56)
//             + live counters below ring
//   y=110-159 Pet sprite (bottom-left) + progress bars
// ────────────────────────────────────────────────────────────────────────────

// Patrol HUD scan phases
enum HudPhase : uint8_t {
    HUD_PHASE_WIFI = 0,
    HUD_PHASE_BLE,
    HUD_PHASE_COMPLETE,
    HUD_PHASE_DONE
};

// Radar dot - a discovered network/device plotted on the ring
#define MAX_HUD_DOTS 24
struct RadarDot {
    float    angle;   // degrees on the ring
    uint16_t color;   // dot color
    bool     active;
};

class UIPatrolHUD {
public:
    static UIPatrolHUD& instance();

    void init(TFT_eSPI* tft);

    // Lifecycle
    void begin(PetStage stage);   // enter portrait mode, draw initial HUD
    void end();                    // exit back to landscape mode

    // Called every main loop iteration (manages own timing internally)
    void update();

    // Feed live discovery data from scan modules
    void onWiFiNetwork(const char* ssid, bool isDuplicate, bool isOpen, int channel);
    void onBLEDevice(const char* addr, bool isTracker);

    // Phase control (called by patrol state machine)
    void setPhase(HudPhase phase);
    HudPhase phase() const { return _phase; }
    bool isComplete() const { return _phase == HUD_PHASE_DONE; }

    // Access the HUD pet animator
    Animator& petAnimator() { return _petAnim; }

private:
    UIPatrolHUD() = default;

    // Drawing sub-routines
    void drawHeader();
    void drawRadarFrame();
    void drawCrosshairs(uint16_t color);
    void drawSweep();
    void drawDots();
    void drawCounters(bool force);
    void drawProgressBars();
    void drawPetCorner();
    void drawPhaseLabel();

    // Dot management
    void addDot(float angle, uint16_t color);

    // Completion animation
    void updateCompletion();

    bool isBig() const;            // true on the large Waveshare panel
    int hudWidth() const;
    int hudHeight() const;
    int headerHeight() const;
    int ringCX() const;
    int ringCY() const;
    int ringRadius() const;
    int hubRadius() const;
    int counterY() const;
    int counterRowGap() const;
    int leftCounterX() const;
    int rightCounterX() const;
    int progressY() const;
    int barWidth() const;
    int barHeight() const;
    int barX() const;
    int petDrawWidth() const;
    int petDrawHeight() const;
    int petX() const;
    int petY() const;
    int scanLabelX() const;
    int scanLabelY() const;
    bool bleUnlocked() const;

    TFT_eSPI* _tft = nullptr;
    Animator  _petAnim;

    float    _sweepAngle   = 0;
    HudPhase _phase        = HUD_PHASE_WIFI;

    // Counters
    int _wifiCount   = 0;
    int _bleCount    = 0;
    int _openCount   = 0;
    int _trackCount  = 0;
    int _lastDrawnWifi  = -1;
    int _lastDrawnBle   = -1;
    int _lastDrawnOpen  = -1;
    int _lastDrawnTrack = -1;

    // Dots on radar
    RadarDot _dots[MAX_HUD_DOTS];
    int      _dotCount = 0;

    // Progress
    float _wifiProgress = 0;  // 0.0 -> 1.0
    float _bleProgress  = 0;

    // Timing
    uint32_t _startTime      = 0;
    uint32_t _phaseStartTime = 0;
    uint32_t _lastFrameTime  = 0;

    // Completion pulses
    int      _completionPulses = 0;
    uint32_t _lastPulseTime    = 0;

    // Pet animation override
    uint32_t _petAnimOverrideEnd = 0;

    // Counter flash timers
    uint32_t _wifiFlashEnd  = 0;
    uint32_t _bleFlashEnd   = 0;
    uint32_t _openFlashEnd  = 0;
    uint32_t _trackFlashEnd = 0;
    uint32_t _ringFlashEnd  = 0;
    uint16_t _ringFlashColor = 0;

    PetStage _stage = STAGE_EGG;
};

