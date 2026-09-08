#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H
#include "../config.h"               // PetForm, Screen

// ── HexHound - Field Report Screen ──────────────────────────────
//
// SCREEN_REPORT. The pet hands back a summary of what the two of you have
// actually done: patrols run, networks met, findings flagged, quests finished,
// exploration, steps (only where steps are real), the best minigame round, and
// where its behavioural form appears to be heading.
//
// ── What this screen is allowed to claim ─────────────────────────────────
//
// 1. THERE IS NO RTC. The device cannot tell a week from an afternoon, so the
//    report never says "this week" or "in the last 7 days". The only elapsed
//    quantity it has any right to state is PetState::questDay minus
//    PetState::lastReportDay, and questDay is itself an approximation Phase 0
//    already established (it advances on power-on and after 24h of powered
//    ticks). That difference is worded as "days since last report" and nothing
//    stronger. No second time approximation is invented here.
//
// 2. THE COUNTERS ARE LIFETIME TOTALS, AND ARE LABELLED AS SUCH. PetState
//    keeps running totals (wifiScans, seenWifiCount, the three threat counts,
//    questsCompleted, roamPoints, stepCount, roamSessions, bestGameScore) and
//    does NOT keep a snapshot of what those totals were at the previous
//    report. Without that snapshot a per-span delta cannot be computed, and a
//    delta faked from a RAM-only baseline would read as truth right up until
//    the first reboot silently turned it back into a lifetime total. So the
//    body is presented as totals and the header carries the day span. See
//    docs/p1w6-wiring.md for what a schema v3 baseline would need if real
//    deltas are wanted later.
//
// 3. STEPS AND EXPLORATION ARE DIFFERENT NUMBERS. stepCount is real physical
//    movement and only ever advances on a board with an IMU. roamPoints is a
//    derived exploration heuristic from radio-environment change and advances
//    everywhere. They get separate lines with separate labels, and on a board
//    that cannot measure motion the steps line is OMITTED entirely rather than
//    printed as "0 steps" (which reads as "you did not walk") or relabelled as
//    distance (which would be a lie).
//
// 4. FINDINGS ARE FLAGGED, NOT CONFIRMED. An open network is an open network,
//    a repeated SSID is a repeated SSID, and a BLE tracker count is a count of
//    CANDIDATES. The wording says so, every time, on every panel.
//
// 5. AN EMPTY REPORT IS A LEGITIMATE STATE. A pet that has done nothing says
//    so in words. It never renders a blank body and never divides by a count
//    it did not check.
//
// ── Interaction ──────────────────────────────────────────────────────────
// One button. Short press scrolls one line (wrapping back to the top after the
// last), long press exits. Lines that do not fit scroll; nothing is truncated
// off the bottom without the footer saying how much is left.
//
// markReported() is deliberately NOT called from the draw path. Drawing is
// re-entrant (a redraw on scroll, a redraw after a repaint) and a draw that
// mutates persisted state would move the span every time the screen refreshed.
// The integrator calls it once, when the report is genuinely consumed.

// A body row is either a full-width sentence or a label/value pair.
enum ReportLineKind : uint8_t {
    REPORT_NOTE = 0,   // full-width text
    REPORT_PAIR        // label left, value right
};

// The text budget, in characters, for one row. It is set by the NARROWEST
// panel in the family: 160x80 at size 1 leaves (160 - 2*4) / 6 = 25 glyphs.
// 320x172 at size 2 gives (320 - 2*6) / 12 = 25, and the round panel's
// narrowest body chord gives 29, so 25 is what every layout can show.
//
// Every literal in ui_report.cpp is written to this budget, and the buffers
// below are sized to hold it. This is load-bearing: an over-long note used to
// be cut off mid-word by snprintf with nothing on screen to say it had been,
// which is exactly the silent truncation this screen is not allowed to do.
#define REPORT_TEXT_COLS 25

// One rendered row. Fixed-size char arrays rather than String: the T-Dongle S3
// has no PSRAM, and a screen that heap-allocates a dozen small strings every
// redraw is how a fragmentation failure shows up somewhere unrelated.
struct ReportLine {
    char     label[REPORT_TEXT_COLS + 1];
    char     value[12];   // uint32_t is at most 10 digits
    uint16_t color;
    uint8_t  kind;
};

// Upper bound on rows. Sized to the longest report this screen can produce:
// span + patrols + networks + findings total + 3 findings breakdowns +
// caveat + quests + exploration + roams + steps + best score + form = 14.
#define REPORT_MAX_LINES 14

// Rows that fit on this panel, derived from SCREEN_W/SCREEN_H rather than
// hardcoded per board. The round panel gets more rows because it renders at
// size 1 with a taller usable band; the 320x172 panel gets fewer because its
// size-2 font is 16px tall.
#if HEXHOUND_PANEL_ROUND
  #define REPORT_ROWS_PER_PAGE 8
#elif SCREEN_H > 100
  #define REPORT_ROWS_PER_PAGE 5
#else
  #define REPORT_ROWS_PER_PAGE 5
#endif

class UIReport {
public:
    static UIReport& instance();

    void init(TFT_eSPI* tft);

    // Rebuild the report from current pet state, reset the scroll, and draw.
    void open();

    // Redraw the current state. Pure render: touches no pet state.
    void draw();

    // Short press: scroll one line, wrapping at the end. Redraws.
    void scrollDown();

    // Long press target. Always true - there is nothing else to select on this
    // screen - but it is a named question so the integrator's switch reads the
    // same as every other screen's.
    bool backSelected() const { return true; }

    // Mark the report consumed: lastReportDay = questDay, pet marked dirty.
    // Call this from the integrator when the user actually leaves the report,
    // never from draw(). Returns true if the span actually moved.
    bool markReported();

    // Whole quest-days between the last report and now, saturating at 0. This
    // is the ONLY elapsed quantity this screen is entitled to state.
    uint16_t daysSinceLastReport() const;

    // True when the pet has never filed a report before.
    bool isFirstReport() const;

    // True when every reportable counter is still zero, i.e. the empty state.
    bool isEmpty() const;

    // Rows currently built, for tests and for the integrator's scroll logic.
    int lineCount() const { return _count; }

private:
    UIReport() = default;

    void buildLines();
    void addNote(const char* text, uint16_t color);
    void addPair(const char* label, uint32_t value, uint16_t color);

    void drawHeader();
    void drawFooter();
#if HEXHOUND_PANEL_ROUND
    void drawRound();
#endif

    TFT_eSPI*  _tft    = nullptr;
    ReportLine _lines[REPORT_MAX_LINES];
    int        _count  = 0;
    int        _scroll = 0;   // index of the top visible row
};
