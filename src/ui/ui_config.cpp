#include "ui_config.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../modules/storage_module.h"
#include "../modules/wifi_module.h"
#include "../modules/usb_module.h"
#include "../config.h"
#if HEXHOUND_HAS_SOFT_SLEEP
#include "../power/soft_sleep.h"
#endif

// ── HexHound - Config Screen Implementation ─────────────────────

#define COL_HEADER     0x07FF  // cyan
#define COL_TAB_ACTIVE 0x07FF  // cyan (active tab)
#define COL_TAB_DIM    0xFFFF  // white (inactive tab)
#define COL_ENTRY      0xFFFF  // white
#define COL_CHECK      0x07E0  // green check
#define COL_ACTION     0xFD20  // amber (action row)
#define COL_CURSOR_BG  0x2945  // dark blue highlight
#define COL_DIVIDER    0x07FF  // cyan
#define COL_FOOTER     0xFFFF  // white
#define COL_CONFIRM    0x07E0  // green (confirmation)
#if HEXHOUND_HAS_SOFT_SLEEP
#define COL_SLEEP      0xF800  // red (soft power off - the one destructive row)
#endif

// Where the cursor is parked whenever this screen puts it somewhere by itself:
// on open, after a tab switch, and after the add row runs.
//
// It is the tab bar rather than the first list entry ONLY on a board with the
// sleep row, and the reason is a collision that is invisible until the list is
// empty. Row 1 is the first list ENTRY when there are entries and the SLEEP row
// when there are none, and a hold on this screen already means "switch tab /
// go back". A fresh device with no trusted hosts would therefore have answered
// the owner's hold-to-leave gesture by powering itself off. Parking on row 0,
// where hold keeps its old meaning, means the sleep row is only ever reached by
// a press the owner deliberately made while looking at it.
//
// Expands to the original literal 1 on every other board, so their generated
// code is unchanged.
#if HEXHOUND_HAS_SOFT_SLEEP
#define CFG_HOME_ROW   0
#else
#define CFG_HOME_ROW   1
#endif

// Cursor index of the FIRST list entry, and therefore the offset that every
// row index below the tab bar is built from.
//
// The sleep row takes the slot directly under the tab bar rather than sitting
// at the bottom with the add row, and that placement is the difference between
// a feature and a trap. Six rows fit between the header and the footer on the
// 480 panel, and "[+ ADD SCAN SSIDS]" adds every network in range in one press,
// so a list long enough to push the last row under the footer is the NORMAL
// state after using this screen once. A row that is scrolled to but not drawn
// is bad enough for an add action; for the only way to turn the board off it is
// unacceptable. Putting it first makes it the one row that can never be clipped.
//
// Expands to the original literal 1 everywhere else, so `i + CFG_FIRST_ENTRY_ROW`
// and `count + CFG_FIRST_ENTRY_ROW` are textually the expressions those boards
// already compiled.
#if HEXHOUND_HAS_SOFT_SLEEP
#define CFG_FIRST_ENTRY_ROW 2
#else
#define CFG_FIRST_ENTRY_ROW 1
#endif

// Layout: size-aware so drawing and hit-testing/scroll math stay consistent.
// Small (160x80) values are preserved exactly when SCREEN_H <= 100.
#if HEXHOUND_PANEL_ROUND
// Round 240x240: entries render at size 1 because a 17-char SSID at size 2 is
// 204px, wider than the chord at every list row.
#define TAB_Y    uiround::scaled(26)
#define LIST_Y   uiround::scaled(58)
#define ROW_H    uiround::scaled(20)
#define FOOTER_Y (uiround::FOOTER_Y)
#else
#define TAB_Y    ((SCREEN_H > 100) ? 6  : 2)
#define LIST_Y   ((SCREEN_H > 100) ? 34 : 16)
#define ROW_H    ((SCREEN_H > 100) ? 24 : 12)
#define FOOTER_Y ((SCREEN_H > 100) ? (SCREEN_H - 22) : (SCREEN_H - 10))
#endif

// Row chrome, factored so the round panel can clip bands and align text to the
// chord while every other board keeps byte-identical full-width behaviour.
static inline void cfgRowBand(TFT_eSPI& tft, int y, int h, uint16_t color) {
#if HEXHOUND_PANEL_ROUND
    uiround::rowFill(tft, y, h, color, uiround::scaled(4));
#else
    tft.fillRect(0, y, SCREEN_W, h, color);
#endif
}

static inline int cfgRowTextX(int y) {
#if HEXHOUND_PANEL_ROUND
    return uiround::rowLeft(y + ROW_H / 2) + uiround::scaled(10);
#else
    (void)y;
    return 4;
#endif
}

static inline int cfgRowCheckX(int y, bool big) {
#if HEXHOUND_PANEL_ROUND
    (void)big;
    return uiround::rowRight(y + ROW_H / 2) - uiround::scaled(14);
#else
    (void)y;
    return SCREEN_W - (big ? 20 : 10);
#endif
}

// Text size for list rows and action rows.
static inline int cfgTextSize(bool big) {
#if HEXHOUND_PANEL_ROUND
    (void)big;
    // Fed straight to setTextSize(), bypassing the uiround helpers, so this is
    // the one place a raw size has to be scaled by hand.
    return uiround::ts(1);
#else
    return big ? 2 : 1;
#endif
}

// Vertical offset that centers a glyph inside ROW_H.
static inline int cfgTextOff(bool big) {
#if HEXHOUND_PANEL_ROUND
    (void)big;
    return (ROW_H - uiround::textH(1)) / 2;
#else
    return big ? (ROW_H - 16) / 2 : 2;
#endif
}

UIConfig& UIConfig::instance() {
    static UIConfig ui;
    return ui;
}

void UIConfig::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIConfig::open() {
    _tab = TAB_SSIDS;
    _cursor = CFG_HOME_ROW;  // see CFG_HOME_ROW above
    _confirmActive = false;
    _lastPulsePhase = -1;
    draw();
}

int UIConfig::totalRows() const {
    auto& stor = StorageModule::instance();
    int listCount = (_tab == TAB_SSIDS) ? stor.getTrustedSSIDCount()
                                        : stor.getTrustedHostCount();
#if HEXHOUND_HAS_SOFT_SLEEP
    // rows: 0=tab bar, 1=sleep, 2..listCount+1=entries, listCount+2=add
    //
    // The sleep row goes before the entries rather than after them; see
    // CFG_FIRST_ENTRY_ROW for why the placement is load-bearing.
    return listCount + 3;  // tab + sleep + entries + action
#else
    // rows: 0=tab bar, 1..listCount=entries, listCount+1=add action
    return listCount + 2;  // tab + entries + action
#endif
}

int UIConfig::actionRowY() const {
    auto& stor = StorageModule::instance();
    int listCount = (_tab == TAB_SSIDS) ? stor.getTrustedSSIDCount()
                                        : stor.getTrustedHostCount();
    return LIST_Y + (listCount * ROW_H);
}

// ── Drawing ───────────────────────────────────────────────────────────────

void UIConfig::draw() {
    if (!_tft) return;

    // Check if confirmation has expired
    if (_confirmActive && millis() > _confirmEnd) {
        _confirmActive = false;
    }

    _tft->fillScreen(TFT_BLACK);
    drawTabBar();

#if HEXHOUND_PANEL_ROUND
    uiround::hLine(*_tft, uiround::HEADER_DIV, COL_DIVIDER, 8);
#else
    _tft->drawFastHLine(0, (SCREEN_H > 100) ? 28 : 14, SCREEN_W, COL_DIVIDER);
#endif

    if (_tab == TAB_SSIDS) {
        drawSSIDList();
    } else {
        drawHostList();
    }

    drawFooter();

    if (_cursor == totalRows() - 1) {
        _lastPulsePhase = (millis() / 500) % 2;
    } else {
        _lastPulsePhase = -1;
    }

    if (_confirmActive) {
        drawConfirmation(_confirmMsg);
    }
}

void UIConfig::update() {
    if (!_tft) return;

    if (_confirmActive && millis() > _confirmEnd) {
        _confirmActive = false;
        draw();
        return;
    }

    if (_cursor != totalRows() - 1) {
        _lastPulsePhase = -1;
        return;
    }

    // totalRows() - 1 is the add row on every board; the sleep row sits above
    // it and is deliberately NOT animated, because the pulse is this screen's
    // "a press here does something" affordance and the sleep row answers to a
    // hold instead.
#if HEXHOUND_HAS_SOFT_SLEEP
    int y = actionRowY() + ROW_H;
#else
    int y = actionRowY();
#endif
    if (y >= FOOTER_Y - ROW_H) {
        _lastPulsePhase = -1;
        return;
    }

    int pulsePhase = (millis() / 500) % 2;
    if (pulsePhase == _lastPulsePhase) {
        return;
    }

    if (_tab == TAB_SSIDS) {
        drawSSIDActionRow(y, true);
    } else {
        drawHostActionRow(y, true);
    }

    if (_confirmActive) {
        drawConfirmation(_confirmMsg);
    }

    _lastPulsePhase = pulsePhase;
}

void UIConfig::drawTabBar() {
#if HEXHOUND_PANEL_ROUND
    // Round: the tab pair is centered rather than run out from the left edge,
    // and the highlight band is chord-clipped. "SSID | HOSTS" is 12 chars, so
    // size 1 keeps it comfortably inside the 128px chord at the title row.
    if (_cursor == 0) {
        uiround::rowFill(*_tft, TAB_Y - uiround::scaled(4), uiround::scaled(18),
                         COL_CURSOR_BG, uiround::scaled(4));
    }
    uint16_t tabBg = (_cursor == 0) ? COL_CURSOR_BG : TFT_BLACK;

    const int gap = 3 * uiround::cellW(1);    // " | "
    const int ssidW = 4 * uiround::cellW(1);
    const int hostsW = 5 * uiround::cellW(1);
    const int totalW = ssidW + gap + hostsW;
    const int x0 = uiround::CX - totalW / 2;

    _tft->setTextSize(uiround::ts(1));
    _tft->setTextColor((_tab == TAB_SSIDS) ? COL_TAB_ACTIVE : COL_TAB_DIM, tabBg);
    _tft->setCursor(x0, TAB_Y);
    _tft->print("SSID");
    _tft->setTextColor(COL_TAB_DIM, tabBg);
    _tft->print(" | ");
    _tft->setTextColor((_tab == TAB_HOSTS) ? COL_TAB_ACTIVE : COL_TAB_DIM, tabBg);
    _tft->print("HOSTS");

    if (_tab == TAB_SSIDS) {
        _tft->drawFastHLine(x0, TAB_Y + 10, ssidW, COL_TAB_ACTIVE);
    } else {
        _tft->drawFastHLine(x0 + ssidW + gap, TAB_Y + 10, hostsW, COL_TAB_ACTIVE);
    }
    return;
#else
    bool big = (SCREEN_H > 100);
    _tft->setTextSize(big ? 2 : 1);

    // Highlight tab bar row if cursor is on it
    if (_cursor == 0) {
        _tft->fillRect(0, 0, SCREEN_W, big ? 26 : 13, COL_CURSOR_BG);
    }

    // Header icon
    _tft->setTextColor(COL_HEADER, (_cursor == 0) ? COL_CURSOR_BG : TFT_BLACK);
    _tft->setCursor(big ? uilg::PAD : 2, TAB_Y);
    _tft->print("\x04 ");

    // SSID tab
    uint16_t ssidBg = (_cursor == 0) ? COL_CURSOR_BG : TFT_BLACK;
    _tft->setTextColor((_tab == TAB_SSIDS) ? COL_TAB_ACTIVE : COL_TAB_DIM, ssidBg);
    _tft->print("SSID");

    _tft->setTextColor(COL_TAB_DIM, ssidBg);
    _tft->print(" | ");

    // HOSTS tab
    _tft->setTextColor((_tab == TAB_HOSTS) ? COL_TAB_ACTIVE : COL_TAB_DIM, ssidBg);
    _tft->print("HOSTS");

    // Underline active tab
    if (big) {
        // size-2: icon+space = 24px, "SSID"=48px, " | "=36px, "HOSTS"=60px
        if (_tab == TAB_SSIDS) {
            _tft->drawFastHLine(30, 24, 48, COL_TAB_ACTIVE);
        } else {
            _tft->drawFastHLine(114, 24, 60, COL_TAB_ACTIVE);
        }
    } else {
        if (_tab == TAB_SSIDS) {
            _tft->drawFastHLine(14, 12, 24, COL_TAB_ACTIVE);
        } else {
            _tft->drawFastHLine(50, 12, 30, COL_TAB_ACTIVE);
        }
    }
#endif // !HEXHOUND_PANEL_ROUND
}

void UIConfig::drawSSIDList() {
    auto& stor = StorageModule::instance();
    int count = stor.getTrustedSSIDCount();

    bool big = (SCREEN_H > 100);
    int txtOff = cfgTextOff(big);
    _tft->setTextSize(cfgTextSize(big));
    int y = LIST_Y;

#if HEXHOUND_HAS_SOFT_SLEEP
    // Before the entries, so a long trusted list can never push the only way to
    // stop this board under the footer. LIST_Y is far above FOOTER_Y - ROW_H on
    // the only panel that compiles this, so it is always drawn.
    drawSleepRow(y, _cursor == 1);
    y += ROW_H;
#endif

    for (int i = 0; i < count && y < FOOTER_Y - ROW_H; i++) {
        int row = i + CFG_FIRST_ENTRY_ROW;  // 0 = tab bar

        // Highlight if cursor is on this row
        if (_cursor == row) {
            cfgRowBand(*_tft, y, ROW_H, COL_CURSOR_BG);
        }

        uint16_t bg = (_cursor == row) ? COL_CURSOR_BG : TFT_BLACK;
        _tft->setTextColor(COL_ENTRY, bg);
        _tft->setCursor(cfgRowTextX(y), y + txtOff);

        // Truncate SSID to fit (leave room for check)
        char ssidBuf[18];
        strlcpy(ssidBuf, stor.getTrustedSSID(i), sizeof(ssidBuf));
        _tft->print(ssidBuf);

        // Green checkmark
        _tft->setTextColor(COL_CHECK, bg);
        _tft->setCursor(cfgRowCheckX(y, big), y + txtOff);
        _tft->print("\x1A");  // right arrow as check substitute

        y += ROW_H;
    }

    // Action row: [+ ADD CURRENT SSIDS]
    if (y < FOOTER_Y - ROW_H) {
        drawSSIDActionRow(y, _cursor == (count + CFG_FIRST_ENTRY_ROW));
    }
}

void UIConfig::drawSSIDActionRow(int y, bool selected) {
    bool big = (SCREEN_H > 100);
    _tft->setTextSize(cfgTextSize(big));
    uint16_t bg = TFT_BLACK;
    if (selected) {
        bg = ((millis() / 500) % 2 == 0) ? COL_CURSOR_BG : 0x3186;
    }
    cfgRowBand(*_tft, y, ROW_H, bg);
    _tft->setTextColor(COL_ACTION, bg);
    _tft->setCursor(cfgRowTextX(y), y + cfgTextOff(big));
    _tft->print("[+ ADD SCAN SSIDS]");
}

void UIConfig::drawHostList() {
    auto& stor = StorageModule::instance();
    int count = stor.getTrustedHostCount();

    bool big = (SCREEN_H > 100);
    int txtOff = cfgTextOff(big);
    _tft->setTextSize(cfgTextSize(big));
    int y = LIST_Y;

#if HEXHOUND_HAS_SOFT_SLEEP
    // Before the entries, so a long trusted list can never push the only way to
    // stop this board under the footer. LIST_Y is far above FOOTER_Y - ROW_H on
    // the only panel that compiles this, so it is always drawn.
    drawSleepRow(y, _cursor == 1);
    y += ROW_H;
#endif

    for (int i = 0; i < count && y < FOOTER_Y - ROW_H; i++) {
        int row = i + CFG_FIRST_ENTRY_ROW;  // 0 = tab bar

        if (_cursor == row) {
            cfgRowBand(*_tft, y, ROW_H, COL_CURSOR_BG);
        }

        uint16_t bg = (_cursor == row) ? COL_CURSOR_BG : TFT_BLACK;
        _tft->setTextColor(COL_ENTRY, bg);
        _tft->setCursor(cfgRowTextX(y), y + txtOff);

        char hostBuf[18];
        strlcpy(hostBuf, stor.getTrustedHost(i), sizeof(hostBuf));
        _tft->print(hostBuf);

        _tft->setTextColor(COL_CHECK, bg);
        _tft->setCursor(cfgRowCheckX(y, big), y + txtOff);
        _tft->print("\x1A");

        y += ROW_H;
    }

    // Action row: [+ ADD CURRENT HOST]
    if (y < FOOTER_Y - ROW_H) {
        drawHostActionRow(y, _cursor == (count + CFG_FIRST_ENTRY_ROW));
    }
}

void UIConfig::drawHostActionRow(int y, bool selected) {
    bool big = (SCREEN_H > 100);
    _tft->setTextSize(cfgTextSize(big));
    uint16_t bg = TFT_BLACK;
    if (selected) {
        bg = ((millis() / 500) % 2 == 0) ? COL_CURSOR_BG : 0x3186;
    }
    cfgRowBand(*_tft, y, ROW_H, bg);
    _tft->setTextColor(COL_ACTION, bg);
    _tft->setCursor(cfgRowTextX(y), y + cfgTextOff(big));
    _tft->print("[+ ADD CUR. HOST]");
}

#if HEXHOUND_HAS_SOFT_SLEEP
// The one row on this screen that does not answer to a press.
//
// It says HOLD in the label rather than relying on the footer, because the
// footer here reads "press=scroll  hold=tab" and is telling the truth about
// every other row. A row whose verb differs from the footer's has to carry its
// own instructions; a menu that never printed how to get out of it is a defect
// this project has already shipped once.
void UIConfig::drawSleepRow(int y, bool selected) {
    bool big = (SCREEN_H > 100);
    _tft->setTextSize(cfgTextSize(big));
    uint16_t bg = selected ? COL_CURSOR_BG : TFT_BLACK;
    cfgRowBand(*_tft, y, ROW_H, bg);
    _tft->setTextColor(COL_SLEEP, bg);
    _tft->setCursor(cfgRowTextX(y), y + cfgTextOff(big));
    _tft->print(selected ? "[SLEEP] HOLD=OFF" : "[SLEEP]");
}
#endif

void UIConfig::drawFooter() {
#if HEXHOUND_PANEL_ROUND
    uiround::footer(*_tft, "press=scroll  hold=tab", COL_DIVIDER);
    return;
#else
    if (SCREEN_H > 100) {
        uiBigFooter(*_tft, "scroll", "hold=tab", COL_DIVIDER);
        return;
    }
    _tft->drawFastHLine(0, FOOTER_Y - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(4, FOOTER_Y);
    _tft->print("scroll");
    _tft->setCursor(SCREEN_W - 48, FOOTER_Y);
    _tft->print("hold=tab");
#endif // !HEXHOUND_PANEL_ROUND
}

void UIConfig::drawConfirmation(const char* msg) {
#if HEXHOUND_PANEL_ROUND
    // Centered capsule rather than a near-full-width box: an 8px-inset rect on
    // a circle loses both ends behind the bezel.
    const int boxH = 26;
    const int boxY = uiround::CY - boxH / 2;
    uiround::rowFill(*_tft, boxY, boxH, 0x1082, 16);
    uiround::hLine(*_tft, boxY, COL_CONFIRM, 16);
    uiround::hLine(*_tft, boxY + boxH - 1, COL_CONFIRM, 16);
    uiround::centerText(*_tft, boxY + (boxH - 8) / 2, msg, 1, COL_CONFIRM, 0x1082);
    return;
#else
    bool big = (SCREEN_H > 100);

    // Overlay confirmation at center
    int boxW = SCREEN_W - 16;
    int boxH = big ? 36 : 20;
    int boxX = 8;
    int boxY = (SCREEN_H - boxH) / 2;

    _tft->fillRect(boxX, boxY, boxW, boxH, 0x1082);  // dark bg
    _tft->drawRect(boxX, boxY, boxW, boxH, COL_CONFIRM);
    _tft->setTextSize(big ? 2 : 1);
    _tft->setTextColor(COL_CONFIRM, 0x1082);

    int textW = strlen(msg) * (big ? 12 : 6);
    int textOff = big ? (boxH - 16) / 2 : 6;  // vertically center size-2 glyph
    _tft->setCursor(SCREEN_W / 2 - textW / 2, boxY + textOff);
    _tft->print(msg);
#endif // !HEXHOUND_PANEL_ROUND
}

// ── Input Handling ────────────────────────────────────────────────────────

bool UIConfig::onShortPress() {
    // Dismiss confirmation overlay if expired
    if (_confirmActive) {
        if (millis() > _confirmEnd) {
            _confirmActive = false;
        } else {
            // Still showing - consume the press
            draw();
            return true;
        }
    }

    auto& stor = StorageModule::instance();
    int listCount = (_tab == TAB_SSIDS) ? stor.getTrustedSSIDCount()
                                        : stor.getTrustedHostCount();
    // Row 1 is the sleep row on a board that has one, and a short press does
    // nothing there but move past it. Committing a power off on the same verb
    // that scrolls the list would put a dead device one mis-hit away, which is
    // the trade this codebase already refused for crafting and rerolls in the
    // sub-lists. See onLongPress().
    int actionRow = listCount + CFG_FIRST_ENTRY_ROW;

    if (_cursor == actionRow) {
        // Action row - execute add action, then wrap to top of list
        if (_tab == TAB_SSIDS) {
            auto& wifi = WiFiModule::instance();
            int added = 0;
            for (int i = 0; i < wifi.resultCount(); i++) {
                const char* ssid = wifi.results()[i].ssid;
                if (strlen(ssid) > 0 && !stor.isSSIDTrusted(ssid)) {
                    stor.addTrustedSSID(ssid);
                    added++;
                }
            }
            if (added > 0) {
                snprintf(_confirmMsg, sizeof(_confirmMsg), "ADDED %d NETWORKS", added);
            } else {
                strlcpy(_confirmMsg, "NO NEW SSIDS", sizeof(_confirmMsg));
            }
        } else {
            if (USBModule::instance().isConnected()) {
                int num = stor.getTrustedHostCount() + 1;
                char hostName[32];
                snprintf(hostName, sizeof(hostName), "USB Host #%d", num);
                stor.addTrustedHost(hostName);
                strlcpy(_confirmMsg, "HOST ADDED", sizeof(_confirmMsg));
            } else {
                strlcpy(_confirmMsg, "NO HOST CONNECTED", sizeof(_confirmMsg));
            }
        }
        _confirmActive = true;
        _confirmEnd = millis() + 1500;
        _cursor = CFG_HOME_ROW;  // wrap back to the top of the screen
        draw();
        return true;
    }

    // Scroll down through list items, wrapping at bottom
    _cursor++;
    if (_cursor > actionRow) {
        _cursor = CFG_HOME_ROW;  // wrap back to the top of the screen
    }
    draw();
    return false;
}

bool UIConfig::onLongPress() {
    // Dismiss any active confirmation
    _confirmActive = false;

#if HEXHOUND_HAS_SOFT_SLEEP
    // Row 1 is the sleep row, and a HOLD is the only verb that commits it. Hold
    // still means "switch tab / go back" on every other row, so this override is
    // as narrow as it can be: one row, reached only on purpose.
    if (_cursor == 1) {
        auto& stor = StorageModule::instance();

        // The pet is the only thing on the device that cannot be rebuilt from
        // the repo, and this is a planned shutdown, so save now rather than
        // hoping the debounced autosave window already closed.
        stor.savePetState();
        stor.appendJournal("SYSTEM", "SLEEP", "Config hold");

        // Say so on the glass BEFORE the panel goes dark. Without this the owner
        // cannot tell a sleep that worked from a gesture that was never seen,
        // and "did the gesture and nothing happened" has been a false-hang
        // symptom on this project twice.
        if (_tft) {
            _tft->fillScreen(TFT_BLACK);
            drawConfirmation("SLEEPING");
#ifdef HEXHOUND_RGB_PANEL
            // This board draws into a PSRAM canvas that loop() flushes, and
            // control never returns to loop() from here. Without this push the
            // message would be composed and then thrown away along with the
            // panel, which is exactly the failure it exists to prevent.
            _tft->present();
#endif
        }
        delay(600);   // long enough to read, short enough not to feel stuck

        hexhound::enterSoftSleep();   // does not return on hardware
        return false;
    }
#endif

    if (_tab == TAB_SSIDS) {
        // Switch to HOSTS tab
        _tab = TAB_HOSTS;
        _cursor = CFG_HOME_ROW;
        draw();
        return false;  // stay on config screen
    }

    // Already on HOSTS tab - signal caller to navigate back
    return true;
}
