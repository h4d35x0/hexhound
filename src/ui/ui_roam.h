#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"   // SCREEN_W / SCREEN_H
#include "../modules/roam_module.h"

// ── HexHound - Roam Screens ─────────────────────────────────────
//
// Two screens: SCREEN_ROAM while the expedition is running, and
// SCREEN_ROAM_REPORT for what it brought back.
//
// The live screen redraws on a value-change basis, not on a timer. That is not
// polish, it is a bug fix carried forward: a screen that repaints because a
// noisy analogue reading moved by one unit flashes continuously, and this one
// is showing a battery-bounded progress bar next to counters that only move
// every twenty seconds. Elapsed time is deadbanded to whole seconds, progress
// to whole percent, and every numeric field is printed at a fixed width so a
// shrinking number cannot leave a stale digit behind.
//
// ── The naming rule ───────────────────────────────────────────────────────
// Exploration points are labelled EXPLORE / pts and NOTHING else. They are
// never called steps, distance, metres, paces or ground covered, because they
// are a derived heuristic over radio churn and none of those words would be
// true. Steps are labelled STEPS and appear ONLY on a board that can actually
// measure them: the step row is gated on RoamModule::stepsMeasured(), never on
// `steps() > 0`. A board with an IMU that has not moved yet should say "0",
// and a board that will never know should say nothing at all. Those are
// different statements and the screen must not merge them.

class UIRoam {
public:
    static UIRoam& instance();

    void init(TFT_eSPI* tft);

    // Entering SCREEN_ROAM. Forces the next draw() to repaint the chrome.
    void open();

    // Live expedition. Cheap when nothing changed.
    void draw();

    // Full repaint of the expedition report. Static screen, so no partial
    // path and no state.
    void drawReport(const RoamReport& r);

private:
    UIRoam() = default;

    void drawLive();
    void drawReportBody(const RoamReport& r);
#if HEXHOUND_PANEL_ROUND
    void drawLiveRound();
    void drawReportRound(const RoamReport& r);
#endif

    // Last values actually painted. Sentinels are deliberately impossible
    // values so the first draw after open() always paints everything.
    struct Shown {
        uint32_t    points  = 0xFFFFFFFFu;
        uint32_t    steps   = 0xFFFFFFFFu;
        uint16_t    samples = 0xFFFFu;
        uint16_t    secs    = 0xFFFFu;
        int         pct     = -2;
        const char* status  = nullptr;
    };

    TFT_eSPI* _tft  = nullptr;
    bool      _full = true;
    Shown     _shown;
};
