#include "ui_report.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../board/capabilities.h"
#include "../pet/pet_core.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Field Report Implementation ──────────────────────
//
// The honesty rules this file exists to enforce are documented in ui_report.h.
// The short version, because it is the part that is easy to break later:
//
//   * the day span is the only elapsed number, and it comes from questDay
//   * every count is a lifetime total and is never labelled as a delta
//   * steps are omitted, not zeroed, on a board with no IMU
//   * findings are flagged, never confirmed
//   * an empty report is words, not a blank screen

#define COL_HEADER   0x07FF  // cyan
#define COL_DIVIDER  0x07FF  // cyan
#define COL_LABEL    0xC618  // light grey - the name of a number
#define COL_VALUE    0xFFFF  // white      - the number itself
#define COL_NOTE     0xC618  // light grey - a sentence, not a statistic
#define COL_WARN     0xFD20  // amber      - something flagged for review
#define COL_GOOD     0x07E0  // green      - an encouragement or a settled form
#define COL_FOOTER   0xFFFF  // white

// Behavioural form display names, indexed by (PetForm - 1). Held locally
// rather than added to config.h because this screen is the only thing that
// currently renders a form name, and config.h is shared Phase 1 ground that no
// feature branch should be widening on its own. If a second screen ever needs
// these, that is the moment to promote the table, not before.
static const char* const kFormNames[] = {
    "Pathfinder",
    "Guardian",
    "Archivist",
    "Cipher",
    "Gremlin",
    "Packmaster"
};

// Copy src into dst, clipped to maxPx pixels at charPx per glyph, ending in
// ".." when it did not fit. Rows here are authored to REPORT_TEXT_COLS so this
// is a backstop rather than the normal path, but when it does fire the cut has
// to be VISIBLE: a label that simply stops mid-word looks like the label.
static void fitText(char* dst, size_t dstSz, const char* src,
                    int maxPx, int charPx) {
    if (!dst || dstSz == 0) return;
    dst[0] = '\0';
    if (!src || charPx <= 0) return;

    int maxChars = maxPx / charPx;
    if (maxChars < 1) return;
    if ((size_t)maxChars > dstSz - 1) maxChars = (int)dstSz - 1;

    const int len = (int)strlen(src);
    if (len <= maxChars) {
        memcpy(dst, src, (size_t)len);
        dst[len] = '\0';
        return;
    }
    if (maxChars <= 2) {
        memcpy(dst, src, (size_t)maxChars);
        dst[maxChars] = '\0';
        return;
    }
    memcpy(dst, src, (size_t)(maxChars - 2));
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
}

// Which form the report may mention, and whether the pet has actually settled
// into it.
//
// A settled form is read straight off PetState::form, which is owned elsewhere
// - this screen reports it, it never decides it.
//
// An unsettled form is reported only as a LEAN, and only when the leading
// behaviour is both meaningfully large and meaningfully ahead. The bar is
// deliberately lower than FORM_MIN_EVIDENCE (which is the bar for actually
// BECOMING a form) because a lean is a weaker claim than an identity, and it
// is deliberately not zero because "you are leaning Gremlin" off two data
// points is noise wearing a hat.
static PetForm reportForm(const PetState& s, bool& settled) {
    settled = false;
    if (s.form != FORM_UNSET && s.form < FORM_COUNT) {
        settled = true;
        return s.form;
    }

    uint16_t top = 0, second = 0;
    int topIdx = -1;
    for (int i = 0; i < FORM_BEHAVIOUR_COUNT; i++) {
        const uint16_t v = s.behaviour[i];
        if (v > top) {
            second = top;
            top    = v;
            topIdx = i;
        } else if (v > second) {
            second = v;
        }
    }

    if (topIdx < 0 || top == 0) return FORM_UNSET;
    if (top < (uint16_t)(FORM_MIN_EVIDENCE / 2)) return FORM_UNSET;
    // top must lead the runner-up by FORM_LEAD_PERCENT. 32-bit maths so a
    // saturated uint16_t counter cannot overflow the comparison.
    if ((uint32_t)top * 100UL < (uint32_t)second * (uint32_t)FORM_LEAD_PERCENT) {
        return FORM_UNSET;
    }
    return (PetForm)(topIdx + 1);
}

UIReport& UIReport::instance() {
    static UIReport ui;
    return ui;
}

void UIReport::init(TFT_eSPI* tft) {
    _tft = tft;
}

// ── Span ──────────────────────────────────────────────────────────────────

uint16_t UIReport::daysSinceLastReport() const {
    const PetState& s = PetCore::instance().state();
    // Saturate rather than wrap. questDay can legitimately be BEHIND
    // lastReportDay: a save restored from another unit, or a counter that has
    // wrapped after 65535 quest-days. A wrapped subtraction there would print
    // a five-digit day count that never happened.
    if (s.questDay <= s.lastReportDay) return 0;
    return (uint16_t)(s.questDay - s.lastReportDay);
}

bool UIReport::isFirstReport() const {
    return PetCore::instance().state().lastReportDay == 0;
}

bool UIReport::isEmpty() const {
    const PetState& s = PetCore::instance().state();
    return s.wifiScans == 0
        && s.seenWifiCount == 0
        && s.threatOpenCount == 0
        && s.threatDuplicateCount == 0
        && s.threatTrackerCount == 0
        && s.questsCompleted == 0
        && s.roamPoints == 0
        && s.stepCount == 0
        && s.roamSessions == 0
        && s.bestGameScore == 0
        && s.form == FORM_UNSET;
}

bool UIReport::markReported() {
    PetState& s = PetCore::instance().state();
    if (s.lastReportDay == s.questDay) return false;
    s.lastReportDay = s.questDay;
    s.dirty = true;
    return true;
}

// ── Line building ─────────────────────────────────────────────────────────

void UIReport::addNote(const char* text, uint16_t color) {
    if (_count >= REPORT_MAX_LINES || !text) return;
    ReportLine& l = _lines[_count++];
    snprintf(l.label, sizeof(l.label), "%s", text);
    l.value[0] = '\0';
    l.color    = color;
    l.kind     = REPORT_NOTE;
}

void UIReport::addPair(const char* label, uint32_t value, uint16_t color) {
    if (_count >= REPORT_MAX_LINES || !label) return;
    ReportLine& l = _lines[_count++];
    snprintf(l.label, sizeof(l.label), "%s", label);
    snprintf(l.value, sizeof(l.value), "%lu", (unsigned long)value);
    l.color = color;
    l.kind  = REPORT_PAIR;
}

void UIReport::buildLines() {
    _count = 0;
    const PetState& s = PetCore::instance().state();

    // ── The span ─────────────────────────────────────────────────────────
    // "days since last report" and nothing stronger. Never "this week": there
    // is no RTC, and questDay is a powered-tick approximation, so a calendar
    // claim would be unsupportable.
    if (isFirstReport()) {
        addNote("First field report", COL_HEADER);
    } else {
        const uint16_t days = daysSinceLastReport();
        if (days == 0) {
            addNote("Same day as last report", COL_NOTE);
        } else if (days == 1) {
            addNote("1 day since last report", COL_NOTE);
        } else {
            addPair("Days since report", days, COL_NOTE);
        }
    }

    // ── The empty report ─────────────────────────────────────────────────
    // A legitimate state, not an edge case. A pet that has done nothing says
    // so; it does not present a screen of zeroes and let the reader work it
    // out, and it does not render an empty body.
    if (isEmpty()) {
        addNote("Nothing logged yet", COL_NOTE);
        addNote("Take me out on patrol", COL_GOOD);
        return;
    }

    // ── Totals ───────────────────────────────────────────────────────────
    // Lifetime running totals. PetState keeps no snapshot of what these were
    // at the previous report, so a per-span delta cannot be computed and is
    // therefore not claimed. See the header comment.
    addPair("Patrols run",  s.wifiScans,     COL_VALUE);
    addPair("Networks met", s.seenWifiCount, COL_VALUE);

    const uint32_t flagged = (uint32_t)s.threatOpenCount
                           + (uint32_t)s.threatDuplicateCount
                           + (uint32_t)s.threatTrackerCount;

    // "Findings flagged", never "threats found". Everything under this
    // heading is something the pet thought was worth a human look, which is a
    // much weaker and much more defensible claim than a detection.
    addPair("Findings flagged", flagged, flagged ? COL_WARN : COL_VALUE);
    if (s.threatOpenCount) {
        addPair("- open networks", s.threatOpenCount, COL_WARN);
    }
    if (s.threatDuplicateCount) {
        addPair("- repeat SSIDs", s.threatDuplicateCount, COL_WARN);
    }
    if (s.threatTrackerCount) {
        // Candidates. The BLE heuristic matches a manufacturer/RSSI pattern; a
        // match is a lead worth checking, not a tracker proven to exist.
        addPair("- tracker leads", s.threatTrackerCount, COL_WARN);
    }
    if (flagged) {
        addNote("Flagged, not confirmed", COL_NOTE);
    }

    addPair("Quests completed", s.questsCompleted, COL_VALUE);

    // ── Exploration and steps: two numbers, never one ────────────────────
    // roamPoints is a DERIVED heuristic (radio-environment change), available
    // on every board. It is labelled in points and is never presented as a
    // distance, because nothing here measures distance.
    addPair("Exploration pts", s.roamPoints, COL_VALUE);
    if (s.roamSessions) {
        addPair("Roams", s.roamSessions, COL_VALUE);
    }

    // stepCount is REAL steps and only ever advances where there is an IMU. On
    // a board that cannot measure motion the line is omitted outright: "Steps
    // 0" reads as "you did not walk", which is a different and false claim
    // from "this device cannot count steps". The constexpr test folds away, so
    // the omitted branch costs those boards nothing.
    if (Caps::canMeasureMotion()) {
        addPair("Steps walked", s.stepCount, COL_VALUE);
    }

    if (s.bestGameScore) {
        addPair("Best game score", s.bestGameScore, COL_VALUE);
    }

    // ── Form direction ───────────────────────────────────────────────────
    // Reported only when there is something to report. A settled form is
    // stated; an emerging one is offered as a lean and worded as a lean.
    bool settled = false;
    const PetForm f = reportForm(s, settled);
    if (f != FORM_UNSET) {
        char buf[REPORT_TEXT_COLS + 1];
        snprintf(buf, sizeof(buf), "%s %s",
                 settled ? "Form:" : "Leaning",
                 kFormNames[(int)f - 1]);
        addNote(buf, settled ? COL_GOOD : COL_WARN);
    }
}

void UIReport::open() {
    buildLines();
    _scroll = 0;
    draw();
}

void UIReport::scrollDown() {
    if (_count <= REPORT_ROWS_PER_PAGE) {
        // Everything is already on screen. Scrolling would move nothing, so
        // the press does nothing visible rather than flickering a redraw.
        return;
    }
    _scroll++;
    if (_scroll > _count - REPORT_ROWS_PER_PAGE) _scroll = 0;
    draw();
}

// ── Rendering ─────────────────────────────────────────────────────────────

void UIReport::draw() {
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;
    if (_count == 0) buildLines();   // never render a blank body

    _tft->fillScreen(TFT_BLACK);
    drawHeader();

    // Panel-family geometry. The 160x80 and 320x172 layouts are the same code
    // at two scales, derived from SCREEN_W/SCREEN_H rather than per board.
    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  charW = big ? uilg::CHAR_W : 6;
    const int  rowH  = big ? 20 : 9;
    const int  bodyY = big ? uilg::BODY_Y : 15;
    const int  padX  = big ? uilg::PAD : 4;

    int visible = _count - _scroll;
    if (visible > REPORT_ROWS_PER_PAGE) visible = REPORT_ROWS_PER_PAGE;
    if (visible < 0) visible = 0;

    _tft->setTextSize(ts);
    for (int i = 0; i < visible; i++) {
        const ReportLine& l = _lines[_scroll + i];
        const int y = bodyY + i * rowH;

        if (l.kind == REPORT_NOTE) {
            _tft->setTextColor(l.color, TFT_BLACK);
            _tft->setCursor(padX, y);
            _tft->print(l.label);
            continue;
        }

        // Value first: it is right-aligned, so the space left for the label is
        // whatever it does not take, and that width is not a constant (a
        // six-digit step count is three glyphs wider than a three-digit one).
        const int vw = (int)strlen(l.value) * charW;
        const int vx = SCREEN_W - padX - vw;
        _tft->setTextColor(l.color, TFT_BLACK);
        _tft->setCursor(vx, y);
        _tft->print(l.value);

        // Clip the label into the gap. Every label here is authored to the
        // REPORT_TEXT_COLS budget, so this only bites on an implausibly large
        // value, and when it does it ends in ".." rather than just stopping.
        char lbuf[REPORT_TEXT_COLS + 1];
        fitText(lbuf, sizeof(lbuf), l.label, vx - padX - charW, charW);
        _tft->setTextColor(COL_LABEL, TFT_BLACK);
        _tft->setCursor(padX, y);
        _tft->print(lbuf);
    }

    drawFooter();
#endif // !HEXHOUND_PANEL_ROUND
}

#if !HEXHOUND_PANEL_ROUND

void UIReport::drawHeader() {
    if (SCREEN_H > 100) {
        uiBigHeader(*_tft, "\x04 FIELD REPORT", COL_HEADER);
        return;
    }
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 FIELD REPORT");
    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
}

void UIReport::drawFooter() {
    // Left: how much of the report is on screen, so a reader can see that more
    // exists rather than having it truncated silently. Right: the way out.
    char range[16];
    range[0] = '\0';
    if (_count > REPORT_ROWS_PER_PAGE) {
        int last = _scroll + REPORT_ROWS_PER_PAGE;
        if (last > _count) last = _count;
        snprintf(range, sizeof(range), "%d-%d/%d", _scroll + 1, last, _count);
    }

    if (SCREEN_H > 100) {
        uiBigFooter(*_tft, range, "hold=back", COL_FOOTER);
        return;
    }

    const int footerY = SCREEN_H - 10;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    if (range[0]) {
        _tft->setCursor(4, footerY);
        _tft->print(range);
    }
    const char* hint = "hold=back";
    _tft->setCursor(SCREEN_W - (int)strlen(hint) * 6 - 4, footerY);
    _tft->print(hint);
}

#endif // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ── Round field report (240x240) ──────────────────────────────────────────
// Rows are centered rather than edge-aligned: on a circle a left margin that
// works for the middle rows runs under the bezel at the top and bottom. A
// label/value pair is composed into one centered string for the same reason -
// right-aligning a value to the chord would make the value column wander from
// row to row.

void UIReport::drawHeader() {}
void UIReport::drawFooter() {}

void UIReport::drawRound() {
    if (!_tft) return;
    if (_count == 0) buildLines();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "REPORT", COL_HEADER);

    const int rowH  = uiround::scaled(15);
    const int bodyY = uiround::BODY_Y;

    int visible = _count - _scroll;
    if (visible > REPORT_ROWS_PER_PAGE) visible = REPORT_ROWS_PER_PAGE;
    if (visible < 0) visible = 0;

    // Centre a short report on the widest part of the glass instead of letting
    // it hug the header.
    const int avail = uiround::FOOTER_DIV - bodyY;
    int y = bodyY + (avail - rowH * visible) / 2;
    if (y < bodyY) y = bodyY;

    for (int i = 0; i < visible; i++) {
        const ReportLine& l = _lines[_scroll + i];

        char buf[REPORT_TEXT_COLS + 16];
        if (l.kind == REPORT_NOTE) {
            snprintf(buf, sizeof(buf), "%s", l.label);
        } else {
            snprintf(buf, sizeof(buf), "%s  %s", l.label, l.value);
        }

        // Clip to the chord at THIS row, not to a fixed column count: the
        // usable width of a round panel is a function of y, so a string that
        // fits in the middle of the glass can run under the bezel near the top.
        char fitted[REPORT_TEXT_COLS + 16];
        fitText(fitted, sizeof(fitted), buf,
                uiround::chordHalfW(y) * 2 - 12, uiround::cellW(1));
        uiround::centerText(*_tft, y, fitted, 1, l.color, TFT_BLACK);
        y += rowH;
    }

    char hint[32];
    if (_count > REPORT_ROWS_PER_PAGE) {
        int last = _scroll + REPORT_ROWS_PER_PAGE;
        if (last > _count) last = _count;
        snprintf(hint, sizeof(hint), "%d-%d/%d  hold=back",
                 _scroll + 1, last, _count);
    } else {
        snprintf(hint, sizeof(hint), "hold=back");
    }
    uiround::footer(*_tft, hint, COL_FOOTER);
}

#endif // HEXHOUND_PANEL_ROUND
