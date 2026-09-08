#include "ui_patrol.h"
#include "../pet/pet_inventory.h"
#include "../config.h"
#include "ui_utils.h"
#include "ui_round.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Patrol Results Implementation ────────────────────

UIPatrol& UIPatrol::instance() {
    static UIPatrol ui;
    return ui;
}

void UIPatrol::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIPatrol::draw(const PatrolSummary& s) {
#if HEXHOUND_PANEL_ROUND
    drawRound(s);
#else
    // ── Rectangular panels (320x172 and 160x80) ───────────────────────────
    //
    // One code path for both, sized from the panel the way the roam report
    // already does it. They used to be two near-identical blocks, and the
    // consequence was the usual one: the materials row was added to neither,
    // because adding it meant getting the same arithmetic right twice.
    //
    // The scan result is two FAMILIES, not one list - what Wi-Fi saw and what
    // BLE saw - so it is drawn as two columns. That is worth doing for its own
    // sake on a 320 px panel whose entire right half was black, and it is also
    // what buys the space for the yield: five rows stacked down the left edge
    // reached y=134 of a body that ends at 146, so there was nowhere to put the
    // materials and this screen has never shown them. The round panels have
    // listed them since the 480 pass; this closes that gap.
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);

    const bool big     = (SCREEN_H > 100);
    const int  ts      = big ? 2 : 1;
    const int  chW     = big ? uilg::CHAR_W : 6;
    const int  chH     = big ? uilg::CHAR_H : 8;
    const int  pad     = big ? uilg::PAD : 4;
    const int  topY    = big ? uilg::BODY_Y : 15;
    // 18 on the big panel, not the 20 this screen used to use. Six rows of
    // size-2 text at 18 fit the 34..146 body exactly, and six is what the worst
    // case needs: three stat rows plus three materials. Identical number and
    // identical reason to the roam report, which hit this first.
    const int  pitch   = big ? 18 : 9;
    const int  footDiv = big ? uilg::FOOTER_DIV : (SCREEN_H - 14);
    const int  footY   = big ? uilg::FOOTER_TEXT : (SCREEN_H - 12);

    // Where the BLE column starts. Twelve cells is the widest column A can
    // ever need: every counter on this screen is derived from a scan capped at
    // MAX_SSIDS / MAX_BLE_DEVICES, which are both 32, so two digits is the
    // ceiling and "WiFi 32+32" (10 cells) is the longest string it can hold.
    // Two cells of gutter is real margin, not a rounding accident.
    const int  colA    = pad;
    const int  colB    = pad + 12 * chW;

    if (big) {
        uiBigHeader(*_tft, "PATROL COMPLETE", TFT_CYAN);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(TFT_CYAN, TFT_BLACK);
        _tft->setCursor(pad, 2);
        _tft->print("PATROL COMPLETE");
        _tft->drawFastHLine(pad, 12, SCREEN_W - 2 * pad, TFT_CYAN);
    }

    _tft->setTextSize(ts);

    // ── Column A: what Wi-Fi saw ──────────────────────────────────────────
    // Named the way the round panels already name it. One product, one
    // vocabulary; "Open AP: 3" and "Open 3" cannot both be the right label for
    // the same number, and the short form is the one that fits two columns.
    int ya = topY;

    _tft->setTextColor(TFT_WHITE, TFT_BLACK);
    _tft->setCursor(colA, ya);
    if (s.newWifiCount > 0) {
        _tft->printf("WiFi %d+%d", s.wifiCount, s.newWifiCount);
    } else {
        _tft->printf("WiFi %d", s.wifiCount);
    }
    ya += pitch;

    if (s.openAPCount > 0) {
        _tft->setTextColor(TFT_YELLOW, TFT_BLACK);
        _tft->setCursor(colA, ya);
        _tft->printf("Open %d", s.openAPCount);
        ya += pitch;
    }

    if (s.dupeCount > 0) {
        _tft->setTextColor(TFT_RED, TFT_BLACK);
        _tft->setCursor(colA, ya);
        _tft->printf("Dupes %d", s.dupeCount);
        ya += pitch;
    }

    // ── Column B: what BLE saw ────────────────────────────────────────────
    int yb = topY;

    _tft->setCursor(colB, yb);
    if (!s.bleScanned) {
        // A board with no BLE radio, or a stage that has not unlocked it. The
        // row still renders: "we did not look" and "we looked and saw nothing"
        // are different answers and a missing row states neither.
        _tft->setTextColor(TFT_ORANGE, TFT_BLACK);
        _tft->print("BLE LOCKED");
    } else {
        _tft->setTextColor(TFT_WHITE, TFT_BLACK);
        if (s.newBleCount > 0) {
            _tft->printf("BLE %d+%d", s.bleCount, s.newBleCount);
        } else {
            _tft->printf("BLE %d", s.bleCount);
        }
    }
    yb += pitch;

    if (s.bleScanned && s.trackerCount > 0) {
        _tft->setTextColor(TFT_ORANGE, TFT_BLACK);
        _tft->setCursor(colB, yb);
        _tft->printf("Track %d", s.trackerCount);
        yb += pitch;
    }

    // ── The yield, full width under both columns ──────────────────────────
    //
    // Counted before anything is drawn, because the last row that FITS is the
    // one that has to carry the count of the rows that did not. The scan counts
    // are the report and the materials are the bonus, so the bonus is what
    // gives way when the body runs out - but it gives way out loud. A row that
    // vanishes silently is how an owner concludes a patrol banked nothing,
    // which is the same wrong conclusion this screen has been inviting by
    // showing no materials at all.
    int y = (ya > yb) ? ya : yb;

    int room = 0;
    for (int probe = y; probe + chH <= footDiv; probe += pitch) room++;

    int shown = (int)s.materialCount;
    if (shown > room) shown = room;
    const int dropped = (int)s.materialCount - shown;

    // A patrol that found too little to cross a divisor legitimately banks
    // nothing and draws no rows at all. That is the honest answer, not a "+0".
    const int maxCells = (SCREEN_W - colA - pad) / chW;
    _tft->setTextColor(TFT_CYAN, TFT_BLACK);
    for (int m = 0; m < shown; m++) {
        char row[40];
        int n = snprintf(row, sizeof(row), "%s x%u",
                         Inventory::instance().nameFor(s.materialId[m]),
                         (unsigned)s.materialQty[m]);
        if (n < 0) n = 0;
        if (dropped > 0 && m == shown - 1 && n < (int)sizeof(row)) {
            snprintf(row + n, sizeof(row) - (size_t)n, " +%d", dropped);
        }
        // Clip to the room actually left rather than trusting a name to fit.
        if (maxCells > 0 && (int)strlen(row) > maxCells) row[maxCells] = '\0';
        _tft->setCursor(colA, y);
        _tft->print(row);
        y += pitch;
    }

    // ── Rewards, at a fixed footer position so they never overflow ─────────
    char rewards[48];
    snprintf(rewards, sizeof(rewards), "Food +%d  XP +%d",
             s.foodGained, s.xpGained);
    if (big) {
        uiBigFooter(*_tft, rewards, nullptr, TFT_GREEN);
    } else {
        _tft->drawFastHLine(pad, footDiv, SCREEN_W - 2 * pad, TFT_CYAN);
        _tft->setTextSize(1);
        _tft->setTextColor(TFT_GREEN, TFT_BLACK);
        _tft->setCursor(pad, footY);
        _tft->print(rewards);
    }
#endif // !HEXHOUND_PANEL_ROUND
}

#if HEXHOUND_PANEL_ROUND

// ── Round patrol results (240x240) ────────────────────────────────────────
// Centered result rows. Rows are conditional, so the block is laid out from a
// start y chosen to keep whatever rows exist centered on the widest part of
// the glass rather than top-aligned under the header.

void UIPatrol::drawRound(const PatrolSummary& s) {
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "PATROL", TFT_CYAN);

    // Count the rows this summary will actually produce so the block can be
    // vertically centered.
    int rows = 2;   // WiFi and BLE always render
    if (s.openAPCount > 0) rows++;
    if (s.dupeCount > 0) rows++;
    if (s.bleScanned && s.trackerCount > 0) rows++;
    // Materials share the same block, so they have to be counted before the
    // start y is chosen or the whole thing sits off centre.
    const int statRows = rows;
    int matShown = (int)s.materialCount;
    rows += matShown;

    // The row pitch has to follow the TEXT, not a number hand-tuned at 240.
    // A size-2 row is uiround::textH(2) tall, which is 16 px at 240 but 32 px
    // at 480, while this constant stayed 22 - so on the 480 panel every row was
    // drawn 10 px into the one above it and the whole block read as a smear.
    // Same class as the pitches already fixed on the roam and update screens.
    constexpr int stepPref = uiround::textH(2) + uiround::scaled(6);
    static_assert(uiround::DIAM != 240 || stepPref == 22,
                  "240 patrol row pitch shifted");
    const int bodyTop = uiround::BODY_Y;
    const int bodyH = uiround::FOOTER_DIV - bodyTop;

    // Everything above plus every material is more rows than this chord has
    // ever had to hold - a full report with three materials is eight - so the
    // pitch tightens to fit, and if even the tightest pitch would overlap the
    // TEXT then material rows are dropped until it does not.
    //
    // The scan counts are the report and the materials are the bonus, so the
    // bonus is what gives way. Dropping a row is honest; drawing two rows on
    // top of each other is not, and drawing one under the bezel is worse.
    int step = stepPref;
    for (;;) {
        if (rows * step <= bodyH) break;
        step = bodyH / (rows > 0 ? rows : 1);
        if (step >= uiround::textH(2)) break;      // tighter, still legible
        if (matShown <= 0) { step = uiround::textH(2); break; }
        matShown--;
        rows--;
        step = stepPref;
    }

    int y = bodyTop + (bodyH - rows * step) / 2;
    if (y < bodyTop) y = bodyTop;
    (void)statRows;

    if (s.newWifiCount > 0) {
        uiround::centerPrintf(*_tft, y, 2, TFT_WHITE, TFT_BLACK,
                              "WiFi %d+%d", s.wifiCount, s.newWifiCount);
    } else {
        uiround::centerPrintf(*_tft, y, 2, TFT_WHITE, TFT_BLACK,
                              "WiFi %d", s.wifiCount);
    }
    y += step;

    if (!s.bleScanned) {
        uiround::centerText(*_tft, y, "BLE LOCKED", 2, TFT_ORANGE, TFT_BLACK);
    } else if (s.newBleCount > 0) {
        uiround::centerPrintf(*_tft, y, 2, TFT_WHITE, TFT_BLACK,
                              "BLE %d+%d", s.bleCount, s.newBleCount);
    } else {
        uiround::centerPrintf(*_tft, y, 2, TFT_WHITE, TFT_BLACK,
                              "BLE %d", s.bleCount);
    }
    y += step;

    if (s.openAPCount > 0) {
        uiround::centerPrintf(*_tft, y, 2, TFT_YELLOW, TFT_BLACK,
                              "Open %d", s.openAPCount);
        y += step;
    }

    if (s.dupeCount > 0) {
        uiround::centerPrintf(*_tft, y, 2, TFT_RED, TFT_BLACK,
                              "Dupes %d", s.dupeCount);
        y += step;
    }

    if (s.bleScanned && s.trackerCount > 0) {
        uiround::centerPrintf(*_tft, y, 2, TFT_ORANGE, TFT_BLACK,
                              "Track %d", s.trackerCount);
        y += step;
    }

    // What the run banked, named the way the Kit names it. A patrol that found
    // too little to cross a divisor legitimately banks nothing and shows no
    // rows at all, which is the honest answer rather than a "+0".
    for (int m = 0; m < matShown; m++) {
        uiround::centerPrintf(*_tft, y, 2, TFT_CYAN, TFT_BLACK,
                              "%s x%u",
                              Inventory::instance().nameFor(s.materialId[m]),
                              (unsigned)s.materialQty[m]);
        y += step;
    }

    char rewards[40];
    snprintf(rewards, sizeof(rewards), "Food +%d   XP +%d",
             s.foodGained, s.xpGained);
    uiround::footer(*_tft, rewards, TFT_GREEN);
}

#endif // HEXHOUND_PANEL_ROUND
