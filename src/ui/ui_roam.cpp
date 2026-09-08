#include "ui_roam.h"
#include "../pet/pet_inventory.h"
#include "../config.h"
#include "ui_utils.h"
#include "ui_round.h"

#include <stdio.h>

// ── HexHound - Roam Screens Implementation ──────────────────────

namespace {

// Fixed-width field widths. Every numeric row is printed at a constant width
// so a value shrinking from 100 to 99 cannot leave a stale digit on the glass,
// and so the centered round rows do not shuffle sideways as they count up.
constexpr int ROW_BUF = 40;

void formatClock(char* out, size_t n, uint32_t ms) {
    uint32_t secs = ms / 1000u;
    uint32_t mins = secs / 60u;
    secs %= 60u;
    if (mins > 99) { mins = 99; secs = 59; }
    snprintf(out, n, "%02u:%02u", (unsigned)mins, (unsigned)secs);
}

} // namespace

UIRoam& UIRoam::instance() {
    static UIRoam ui;
    return ui;
}

void UIRoam::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIRoam::open() {
    _full  = true;
    _shown = Shown();
}

void UIRoam::draw() {
    if (!_tft) return;
#if HEXHOUND_PANEL_ROUND
    drawLiveRound();
#else
    drawLive();
#endif
}

void UIRoam::drawReport(const RoamReport& r) {
    if (!_tft) return;
#if HEXHOUND_PANEL_ROUND
    drawReportRound(r);
#else
    drawReportBody(r);
#endif
}

#if !HEXHOUND_PANEL_ROUND

// ══════════════════════════════════════════════════════════════════════════
// Rectangular panels: 160x80 (size 1) and 320x172 (size 2)
// ══════════════════════════════════════════════════════════════════════════

void UIRoam::drawLive() {
    RoamModule& rm = RoamModule::instance();

    // Compile-time constant. On a board with no IMU the step row does not
    // exist at all - it is not drawn as "STEPS 0", because that would invite
    // the reading that this board counts steps and simply has not moved.
    const bool hasSteps = RoamModule::stepsMeasured();

    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  ch    = 8 * ts;
    const int  padx  = big ? uilg::PAD : 4;
    const int  topY  = big ? uilg::BODY_Y : 15;
    const int  pitch = big ? 22 : 11;
    const int  footDiv = big ? uilg::FOOTER_DIV : (SCREEN_H - 14);
    const int  footY   = big ? uilg::FOOTER_TEXT : (SCREEN_H - 12);

    int y = topY;
    const int rowExplore = y; y += pitch;
    const int rowSteps   = hasSteps ? y : -1; if (hasSteps) y += pitch;
    const int rowSamples = y; y += pitch;
    const int rowStatus  = y; y += pitch;

    const int barH = big ? 6 : 3;
    int barY = footDiv - barH - 3;
    const int lastBottom = rowStatus + ch;
    if (barY < lastBottom + 2) barY = -1;   // no room: the bar is optional

    const uint32_t pts   = rm.points();
    const uint32_t stp   = rm.steps();
    const uint16_t smp   = rm.sampleCount();
    const uint32_t elMs  = rm.elapsedMs();
    const int      pct   = rm.progressPct();
    const char*    st    = rm.statusLine();

    uint16_t secs = (uint16_t)(elMs / 1000u);
    if (secs > 5999) secs = 5999;

    char buf[ROW_BUF];

    if (_full) {
        _tft->fillScreen(TFT_BLACK);

        if (big) {
            uiBigHeader(*_tft, "ROAM", TFT_CYAN);
            _tft->drawFastHLine(0, footDiv, SCREEN_W, TFT_CYAN);
        } else {
            _tft->setTextSize(1);
            _tft->setTextColor(TFT_CYAN, TFT_BLACK);
            _tft->setCursor(padx, 2);
            _tft->print("ROAM");
            _tft->drawFastHLine(padx, 12, SCREEN_W - 2 * padx, TFT_CYAN);
            _tft->drawFastHLine(padx, footDiv, SCREEN_W - 2 * padx, TFT_CYAN);
        }

        // Static footer hint on the right. The left half is the clock and is
        // repainted per second below.
        _tft->setTextSize(ts);
        _tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
#if HEXHOUND_HAS_TOUCH
        // Touch is suppressed for the whole expedition - a pocket is a
        // conductor and a hold on this screen ends the roam - so on a board
        // with a panel the only thing that ends it is the BUTTON. Saying
        // "hold=end" without saying what to hold sent someone pressing the
        // glass and wondering why nothing happened.
        const char* hint = "btn=end";
#else
        const char* hint = "hold=end";
#endif
        int w = (int)strlen(hint) * 6 * ts;
        _tft->setCursor(SCREEN_W - w - padx, footY);
        _tft->print(hint);

        if (barY >= 0 && pct >= 0) {
            _tft->drawRect(padx, barY, SCREEN_W - 2 * padx, barH, TFT_DARKGREY);
        }
        _full = false;
    }

    _tft->setTextSize(ts);

    if (pts != _shown.points) {
        _shown.points = pts;
        // EXPLORE, never "steps" and never a distance: this number comes from
        // radio-environment change, not from anything that moved a person.
        snprintf(buf, sizeof(buf), "EXPLORE %5lu pts", (unsigned long)pts);
        _tft->setTextColor(TFT_GREEN, TFT_BLACK);
        _tft->setCursor(padx, rowExplore);
        _tft->print(buf);
    }

    if (hasSteps && stp != _shown.steps) {
        _shown.steps = stp;
        snprintf(buf, sizeof(buf), "STEPS   %5lu", (unsigned long)stp);
        _tft->setTextColor(TFT_WHITE, TFT_BLACK);
        _tft->setCursor(padx, rowSteps);
        _tft->print(buf);
    }

    if (smp != _shown.samples) {
        _shown.samples = smp;
        snprintf(buf, sizeof(buf), "SAMPLES %5u", (unsigned)smp);
        _tft->setTextColor(TFT_WHITE, TFT_BLACK);
        _tft->setCursor(padx, rowSamples);
        _tft->print(buf);
    }

    if (st != _shown.status) {
        _shown.status = st;
        snprintf(buf, sizeof(buf), "%-13s", st);
        _tft->setTextColor(TFT_CYAN, TFT_BLACK);
        _tft->setCursor(padx, rowStatus);
        _tft->print(buf);
    }

    // Deadbanded to whole seconds. Without this the clock would repaint on
    // every loop iteration and drag the rest of the screen with it.
    if (secs != _shown.secs) {
        _shown.secs = secs;
        formatClock(buf, sizeof(buf), elMs);
        _tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
        _tft->setCursor(padx, footY);
        _tft->print(buf);
    }

    // pct is -1 on a board that cannot measure charge. That is not "0%": no
    // ceiling was invented for it, so no progress bar is drawn at all.
    if (barY >= 0 && pct >= 0 && pct != _shown.pct) {
        _shown.pct = pct;
        int innerW = SCREEN_W - 2 * padx - 2;
        int fill = innerW * pct / 100;
        if (fill > 0) {
            _tft->fillRect(padx + 1, barY + 1, fill, barH - 2, TFT_CYAN);
        }
        if (fill < innerW) {
            _tft->fillRect(padx + 1 + fill, barY + 1, innerW - fill, barH - 2,
                           TFT_BLACK);
        }
    }
}

void UIRoam::drawReportBody(const RoamReport& r) {
    _tft->fillScreen(TFT_BLACK);

    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  ch    = 8 * ts;
    const int  padx  = big ? uilg::PAD : 4;
    const int  topY  = big ? uilg::BODY_Y : 15;
    // 18, not the 20 every other big-panel screen uses: the worst-case report
    // is exploration + steps + all four materials, and at 20 the last material
    // fell past the footer rule and was silently dropped. Six rows of size-2
    // text at 18 fit the 34..146 body exactly.
    const int  pitch = big ? 18 : 9;
    const int  footDiv = big ? uilg::FOOTER_DIV : (SCREEN_H - 14);
    const int  footY   = big ? uilg::FOOTER_TEXT : (SCREEN_H - 12);

    if (big) {
        uiBigHeader(*_tft, "ROAM REPORT", TFT_CYAN);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(TFT_CYAN, TFT_BLACK);
        _tft->setCursor(padx, 2);
        _tft->print("ROAM REPORT");
        _tft->drawFastHLine(padx, 12, SCREEN_W - 2 * padx, TFT_CYAN);
    }

    _tft->setTextSize(ts);
    int y = topY;
    char buf[ROW_BUF];

    _tft->setTextColor(TFT_GREEN, TFT_BLACK);
    _tft->setCursor(padx, y);
    _tft->printf("Explore +%lu pts", (unsigned long)r.pointsEarned);
    y += pitch;

    // Only a board that actually measured steps says anything about steps.
    if (r.stepsMeasured) {
        _tft->setTextColor(TFT_WHITE, TFT_BLACK);
        _tft->setCursor(padx, y);
        _tft->printf("Steps +%lu", (unsigned long)r.stepsTaken);
        y += pitch;
    }

    _tft->setTextColor(TFT_YELLOW, TFT_BLACK);
    if (r.findCount == 0) {
        if (y + ch <= footDiv) {
            _tft->setCursor(padx, y);
            _tft->print("No materials");
            y += pitch;
        }
    } else if (big) {
        for (int i = 0; i < r.findCount && y + ch <= footDiv; i++) {
            _tft->setCursor(padx, y);
            _tft->printf("%s x%u", Inventory::instance().nameFor(r.finds[i].material),
                         (unsigned)r.finds[i].count);
            y += pitch;
        }
    } else {
        // 160x80 is 26 columns, so materials pack two to a line with the
        // short names rather than pushing the footer off the panel.
        for (int i = 0; i < r.findCount && y + ch <= footDiv; i += 2) {
            int n = snprintf(buf, sizeof(buf), "%s x%u",
                             Inventory::instance().nameFor(r.finds[i].material),
                             (unsigned)r.finds[i].count);
            if (i + 1 < r.findCount && n > 0 && n < (int)sizeof(buf)) {
                snprintf(buf + n, sizeof(buf) - n, "  %s x%u",
                         Inventory::instance().nameFor(r.finds[i + 1].material),
                         (unsigned)r.finds[i + 1].count);
            }
            _tft->setCursor(padx, y);
            _tft->print(buf);
            y += pitch;
        }
    }

    // How long, and WHY it stopped. The sample count used to sit here and has
    // given up its place to the reason, which was previously readable NOWHERE.
    // Nothing is lost by that: the count is on the live screen for the whole
    // expedition, and endRoam() now writes it into the journal entry, which
    // outlives this screen. Both could be squeezed onto one line at 24 of the
    // 25 columns the 320x172 panel has at text size 2, and one column of margin
    // is not enough to bet a layout on.
    formatClock(buf, sizeof(buf), r.durationMs);
    _tft->drawFastHLine(padx, footDiv, SCREEN_W - 2 * padx, TFT_CYAN);
    _tft->setTextColor(TFT_DARKGREY, TFT_BLACK);
    _tft->setCursor(padx, footY);
    _tft->printf("%s  %s", buf, roamEndReasonName(r.endReason));
}

#endif // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ══════════════════════════════════════════════════════════════════════════
// Round panel (240x240)
// ══════════════════════════════════════════════════════════════════════════
//
// This is the only board in the fleet with an IMU, so it is the only one that
// ever renders a step row. The layout is built around that: exploration gets
// the large hero number, steps get the second field, and the rim ring carries
// the battery-derived progress that only this family of board can compute.

void UIRoam::drawLiveRound() {
    RoamModule& rm = RoamModule::instance();
    const bool hasSteps = RoamModule::stepsMeasured();

    const uint32_t pts  = rm.points();
    const uint32_t stp  = rm.steps();
    const uint16_t smp  = rm.sampleCount();
    const uint32_t elMs = rm.elapsedMs();
    const int      pct  = rm.progressPct();
    const char*    st   = rm.statusLine();

    uint16_t secs = (uint16_t)(elMs / 1000u);
    if (secs > 5999) secs = 5999;

    char buf[ROW_BUF];

    if (_full) {
        _tft->fillScreen(TFT_BLACK);
        uiround::header(*_tft, "ROAM", TFT_CYAN);
        uiround::centerText(*_tft, uiround::scaled(60), "EXPLORATION", 1, TFT_DARKGREY, TFT_BLACK);
        uiround::centerText(*_tft, uiround::scaled(106), hasSteps ? "STEPS" : "SAMPLES", 1,
                            TFT_DARKGREY, TFT_BLACK);
        _full = false;
    }

    // The rectangular panels pad each field to a fixed width so a shrinking
    // number cannot leave a stale digit. That trick does not work on a
    // centered layout: padding a centered field pushes the digits off the
    // optical centre and they visibly drift as the count grows. Here the row
    // band is erased first - chord-clipped, so nothing is painted under the
    // bezel - and the value is then drawn centered at its natural width.
    if (pts != _shown.points) {
        _shown.points = pts;
        uiround::rowFill(*_tft, uiround::scaled(72), uiround::scaled(24), TFT_BLACK);
        uiround::centerPrintf(*_tft, uiround::scaled(72), 3, TFT_GREEN, TFT_BLACK,
                              "%lu", (unsigned long)pts);
    }

    if (hasSteps) {
        if (stp != _shown.steps) {
            _shown.steps = stp;
            uiround::rowFill(*_tft, uiround::scaled(118), uiround::scaled(16), TFT_BLACK);
            uiround::centerPrintf(*_tft, uiround::scaled(118), 2, TFT_WHITE, TFT_BLACK,
                                  "%lu", (unsigned long)stp);
        }
        if (smp != _shown.samples) {
            _shown.samples = smp;
            uiround::rowFill(*_tft, uiround::scaled(158), uiround::scaled(8), TFT_BLACK);
            uiround::centerPrintf(*_tft, uiround::scaled(158), 1, TFT_DARKGREY, TFT_BLACK,
                                  "SAMPLES %u", (unsigned)smp);
        }
    } else if (smp != _shown.samples) {
        _shown.samples = smp;
        uiround::rowFill(*_tft, uiround::scaled(118), uiround::scaled(16), TFT_BLACK);
        uiround::centerPrintf(*_tft, uiround::scaled(118), 2, TFT_WHITE, TFT_BLACK,
                              "%u", (unsigned)smp);
    }

    if (st != _shown.status) {
        _shown.status = st;
        uiround::rowFill(*_tft, uiround::scaled(140), uiround::scaled(8), TFT_BLACK);
        uiround::centerPrintf(*_tft, uiround::scaled(140), 1, TFT_CYAN, TFT_BLACK, "%s", st);
    }

    if (secs != _shown.secs) {
        _shown.secs = secs;
        formatClock(buf, sizeof(buf), elMs);
        char foot[24];
#if HEXHOUND_HAS_TOUCH
        // See the note on the rectangular footer: touch is off for the whole
        // expedition, so the button is the only thing that ends it.
        snprintf(foot, sizeof(foot), "%s  btn=end", buf);
#else
        snprintf(foot, sizeof(foot), "%s  hold=end", buf);
#endif
        uiround::footer(*_tft, foot, TFT_DARKGREY);
    }

    // Unbounded expeditions get no ring, because there is no ceiling to show
    // progress against and a ring that never fills is worse than no ring.
    if (pct >= 0 && pct != _shown.pct) {
        _shown.pct = pct;
        uiround::progressRing(*_tft, pct, TFT_CYAN, 0x2104);
    }
}

void UIRoam::drawReportRound(const RoamReport& r) {
    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "REPORT", TFT_CYAN);

    int rows = 1 + (r.stepsMeasured ? 1 : 0) + (r.findCount ? r.findCount : 1);
    // Scaled: the row pitch has to grow with the text, or six size-2 lines
    // land on top of each other on the 480 panel.
    const int pitch = uiround::scaled(22);
    const int bodyTop = uiround::BODY_Y;
    const int bodyH   = uiround::FOOTER_DIV - bodyTop;
    int y = bodyTop + (bodyH - rows * pitch) / 2;
    if (y < bodyTop) y = bodyTop;

    uiround::centerPrintf(*_tft, y, 2, TFT_GREEN, TFT_BLACK,
                          "Explore +%lu", (unsigned long)r.pointsEarned);
    y += pitch;

    if (r.stepsMeasured) {
        uiround::centerPrintf(*_tft, y, 2, TFT_WHITE, TFT_BLACK,
                              "Steps +%lu", (unsigned long)r.stepsTaken);
        y += pitch;
    }

    if (r.findCount == 0) {
        uiround::centerText(*_tft, y, "No materials", 2, TFT_DARKGREY, TFT_BLACK);
        y += pitch;
    } else {
        for (int i = 0; i < r.findCount && y + uiround::textH(2) <= uiround::FOOTER_DIV; i++) {
            uiround::centerPrintf(*_tft, y, 2, TFT_YELLOW, TFT_BLACK,
                                  "%s x%u", Inventory::instance().nameFor(r.finds[i].material),
                                  (unsigned)r.finds[i].count);
            y += pitch;
        }
    }

    // Duration and reason, for the same reason as the rectangular layout: the
    // report is where someone stands when they find an expedition already over,
    // and it has to be able to say why.
    char clock[16];
    formatClock(clock, sizeof(clock), r.durationMs);
    char foot[32];
    snprintf(foot, sizeof(foot), "%s  %s", clock,
             roamEndReasonName(r.endReason));
    uiround::footer(*_tft, foot, TFT_DARKGREY);
}

#endif // HEXHOUND_PANEL_ROUND
