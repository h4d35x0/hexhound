#include "ui_alert.h"
#include "ui_round.h"

// ── HexHound - Alert Overlay Implementation ─────────────────────

#define COL_BG_ALERT TFT_BLACK

UIAlert& UIAlert::instance() {
    static UIAlert ui;
    return ui;
}

void UIAlert::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIAlert::draw(NotifLevel level, const char* message, const char* source) {
    if (!_tft) return;

    // Choose border/text color by level
    uint16_t borderCol;
    const char* levelStr;
    switch (level) {
        case NOTIF_CRIT:
            borderCol = TFT_RED;
            levelStr  = "!! CRITICAL !!";
            break;
        case NOTIF_WARN:
            borderCol = TFT_YELLOW;
            levelStr  = "WARNING";
            break;
        default:
            borderCol = TFT_BLUE;
            levelStr  = "INFO";
            break;
    }

    _tft->fillScreen(COL_BG_ALERT);

#if HEXHOUND_PANEL_ROUND
    // ── Round layout (240x240) ────────────────────────────────────────────
    // The rectangular border below is the one piece of chrome that vanishes
    // completely on a circular panel - all four corners and most of each edge
    // sit behind the bezel. Here the border IS the rim, which makes the alert
    // level read at a glance from across a room.
    _tft->drawCircle(uiround::CX, uiround::CY, uiround::R - 1, borderCol);
    _tft->drawCircle(uiround::CX, uiround::CY, uiround::R - 2, borderCol);
    _tft->drawCircle(uiround::CX, uiround::CY, uiround::R - 3, borderCol);

    uiround::centerText(*_tft, uiround::scaled(40), levelStr, 2, borderCol, COL_BG_ALERT);
    uiround::hLine(*_tft, uiround::scaled(62), borderCol, 10);

    // 186 was a bare 240-era bound while everything around it scaled, so the
    // 480 panel got exactly one 16-column line and EVERY alert on the T-RGB was
    // truncated mid-sentence: the Wi-Fi scan's "Duplicate SSID detected" has
    // been rendering as "Duplicate SSID". Worked through at 480, where
    // TEXT_SCALE is 2: the body starts at scaled(76) = 152 and textBlock loops
    // while y + textH(2) <= maxY, so 152 + 32 <= 186 passes once and
    // 188 + 32 <= 186 fails.
    //
    // The source line had a second bug in the same expression: the guard tested
    // the SCALED offset but the else branch applied the UNSCALED one, so the
    // label was drawn 6 px high at 480. Both are invisible at 240, where
    // scaled() is the identity - which is exactly why neither was ever seen on
    // the Waveshare 1.28, and why that board's layout is unchanged to the pixel.
    const int bodyMaxY = uiround::scaled(186);
    int bodyEnd = uiround::textBlock(*_tft, uiround::scaled(76), bodyMaxY, message, 2,
                                     TFT_WHITE, COL_BG_ALERT);
    if (source && strlen(source) > 0 && bodyEnd < bodyMaxY) {
        int srcY = bodyEnd + uiround::scaled(6);
        if (srcY > bodyMaxY) srcY = bodyMaxY;
        uiround::centerText(*_tft, srcY, source, 1, borderCol, COL_BG_ALERT);
    }
#else

    // Border
    _tft->drawRect(0, 0, SCREEN_W, SCREEN_H, borderCol);
    _tft->drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, borderCol);

    if (SCREEN_H > 100) {
        // ── Large-screen layout (e.g. Waveshare 320x172, size 2 font) ──────
        // Level header at size 2
        _tft->setTextColor(borderCol, COL_BG_ALERT);
        _tft->setTextSize(2);
        _tft->setCursor(8, 8);
        _tft->print(levelStr);

        // Divider line
        _tft->drawFastHLine(4, 30, SCREEN_W - 8, borderCol);

        // Message (word-wrapped at size 2: ~24 chars/line, 20px line height)
        _tft->setTextColor(TFT_WHITE, COL_BG_ALERT);
        _tft->setTextSize(2);
        int y = 38;
        _tft->setCursor(8, y);

        int len = strlen(message);
        int col = 0;
        for (int i = 0; i < len && y < SCREEN_H - 24; i++) {
            if (message[i] == '\n' || col >= 24) {
                y += 20;
                col = 0;
                _tft->setCursor(8, y);
                if (message[i] == '\n') continue;
                if (y >= SCREEN_H - 24) break;
            }
            _tft->print(message[i]);
            col++;
        }

        // Source at bottom (size 2)
        if (strlen(source) > 0) {
            _tft->setTextColor(borderCol, COL_BG_ALERT);
            _tft->setTextSize(2);
            _tft->setCursor(8, SCREEN_H - 22);
            _tft->print(source);
        }
    } else {
        // ── Small-screen layout (160x80, size 1 font), unchanged ───────────
        // Level header
        _tft->setTextColor(borderCol, COL_BG_ALERT);
        _tft->setTextSize(1);
        _tft->setCursor(6, 6);
        _tft->print(levelStr);

        // Divider line
        _tft->drawFastHLine(4, 18, SCREEN_W - 8, borderCol);

        // Message (word-wrapped manually for small display)
        _tft->setTextColor(TFT_WHITE, COL_BG_ALERT);
        _tft->setCursor(6, 24);

        // Simple word wrap for 160px wide display at font size 1 (~26 chars)
        int len = strlen(message);
        int col = 0;
        int y   = 24;
        for (int i = 0; i < len && y < SCREEN_H - 16; i++) {
            if (message[i] == '\n' || col >= 25) {
                y += 10;
                col = 0;
                _tft->setCursor(6, y);
                if (message[i] == '\n') continue;
            }
            _tft->print(message[i]);
            col++;
        }

        // Source at bottom
        if (strlen(source) > 0) {
            _tft->setTextColor(borderCol, COL_BG_ALERT);
            _tft->setCursor(6, SCREEN_H - 12);
            _tft->print(source);
        }
    }
#endif // !HEXHOUND_PANEL_ROUND
}
