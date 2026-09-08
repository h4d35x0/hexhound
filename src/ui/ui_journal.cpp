#include "ui_journal.h"
#include "../modules/storage_module.h"
#include "../config.h"
#include "ui_utils.h"
#include "ui_round.h"

// ── HexHound - Journal Screen Implementation ────────────────────

// Colors
#define COL_TIMESTAMP  0xFFFF  // white
#define COL_TITLE_DEF  0xFFFF  // white (default)
#define COL_DETAIL     0xFFFF  // white
#define COL_EVOLVED    0xFD20  // amber
#define COL_ALERT      0xF800  // red
#define COL_MISSION    0x07E0  // green
#define COL_HATCH      0x07FF  // cyan
#define COL_DIVIDER    0x07FF  // cyan
#define COL_HEADER     0x07FF  // cyan
#define COL_PAGE       0xFFFF  // white
#define COL_FOOTER     0xFFFF  // white

UIJournal& UIJournal::instance() {
    static UIJournal ui;
    return ui;
}

void UIJournal::init(TFT_eSPI* tft) {
    _tft = tft;
}

// ── Parse journal entries from storage ────────────────────────────────────

void UIJournal::loadEntries() {
    _count = 0;
    _scrollPos = 0;

    String raw = StorageModule::instance().readLastJournalEntries(MAX_JOURNAL_DISPLAY);
    if (raw.length() == 0) return;

    // Split by newlines and parse each line
    int start = 0;
    for (int i = 0; i <= (int)raw.length(); i++) {
        if (i == (int)raw.length() || raw[i] == '\n') {
            if (i > start && _count < MAX_JOURNAL_DISPLAY) {
                String line = raw.substring(start, i);
                line.trim();
                if (line.length() > 0) {
                    parseLine(line.c_str(), _entries[_count]);
                    _count++;
                }
            }
            start = i + 1;
        }
    }

    // Scroll to bottom (most recent) if more than a page
    if (_count > ENTRIES_PER_PAGE) {
        _scrollPos = _count - ENTRIES_PER_PAGE;
    }
}

void UIJournal::parseLine(const char* line, JournalEntry& entry) {
    memset(&entry, 0, sizeof(entry));
    entry.titleColor = COL_TITLE_DEF;

    // Try CSV format: millis|TYPE|detail1|detail2
    const char* p1 = strchr(line, '|');
    if (p1) {
        // Parse millis -> HH:MM
        long ms = atol(line);
        unsigned long sec = ms / 1000;
        snprintf(entry.timestamp, sizeof(entry.timestamp), "%02lu:%02lu",
                 (sec / 3600) % 24, (sec / 60) % 60);

        const char* typeStart = p1 + 1;
        const char* p2 = strchr(typeStart, '|');
        if (p2) {
            int typeLen = min((int)(p2 - typeStart), (int)sizeof(entry.title) - 1);
            strncpy(entry.title, typeStart, typeLen);
            entry.title[typeLen] = '\0';

            const char* d1Start = p2 + 1;
            const char* p3 = strchr(d1Start, '|');
            if (p3) {
                int d1Len = min((int)(p3 - d1Start), (int)sizeof(entry.detail1) - 1);
                strncpy(entry.detail1, d1Start, d1Len);
                entry.detail1[d1Len] = '\0';
                strlcpy(entry.detail2, p3 + 1, sizeof(entry.detail2));
            } else {
                strlcpy(entry.detail1, d1Start, sizeof(entry.detail1));
            }
        } else {
            strlcpy(entry.title, typeStart, sizeof(entry.title));
        }
    } else {
        // Legacy format: [HH:MM:SS] text
        if (line[0] == '[') {
            // Extract HH:MM from [HH:MM:SS]
            strncpy(entry.timestamp, line + 1, 5);
            entry.timestamp[5] = '\0';
            // Rest is title
            const char* textStart = strchr(line, ']');
            if (textStart) {
                textStart++;
                while (*textStart == ' ') textStart++;
                strlcpy(entry.title, textStart, sizeof(entry.title));
            }
        } else {
            strlcpy(entry.timestamp, "??:??", sizeof(entry.timestamp));
            strlcpy(entry.title, line, sizeof(entry.title));
        }
    }

    // Map type to color
    if (strstr(entry.title, "EVOLVED") || strstr(entry.title, "Evolved")) {
        entry.titleColor = COL_EVOLVED;
    } else if (strstr(entry.title, "ALERT") || strstr(entry.title, "Alert")) {
        entry.titleColor = COL_ALERT;
    } else if (strstr(entry.title, "MISSION") || strstr(entry.title, "Mission")) {
        entry.titleColor = COL_MISSION;
    } else if (strstr(entry.title, "HATCH") || strstr(entry.title, "Hatch")) {
        entry.titleColor = COL_HATCH;
    } else if (strstr(entry.title, "BOOT") || strstr(entry.title, "Boot")) {
        entry.titleColor = COL_TITLE_DEF;
    } else if (strstr(entry.title, "PATROL") || strstr(entry.title, "Patrol")) {
        entry.titleColor = COL_TITLE_DEF;
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────

void UIJournal::draw() {
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);
    drawHeader();

    if (_count == 0) {
        _tft->setTextColor(COL_DETAIL, TFT_BLACK);
        if (SCREEN_H > 100) {
            // Centered-ish size-2 empty state on the big panel
            _tft->setTextSize(2);
            const char* msg = "No entries yet";
            int w = (int)strlen(msg) * uilg::CHAR_W;
            _tft->setCursor((SCREEN_W - w) / 2, 80);
            _tft->print(msg);
        } else {
            _tft->setTextSize(1);
            _tft->setCursor(30, 35);
            _tft->print("No entries yet");
        }
        drawFooter();
        return;
    }

    if (SCREEN_H > 100) {
        // ── Big-screen (320x172) layout: size-2 font, more entries ──────────
        int y = uilg::BODY_Y;
        int visible = min(ENTRIES_PER_PAGE, _count - _scrollPos);
        for (int i = 0; i < visible; i++) {
            int idx = _scrollPos + i;
            if (y >= uilg::FOOTER_DIV) break;   // no room for another entry

            drawEntry(idx, y);

            // Height: title row (20px) + 20px per present detail line
            int entryH = 20;
            if (strlen(_entries[idx].detail1) > 0) entryH += 20;
            if (strlen(_entries[idx].detail2) > 0) entryH += 20;
            y += entryH + 4;   // small gap between entries

            // Divider between entries while there is still body room
            if (i < visible - 1 && y < uilg::FOOTER_DIV) {
                _tft->drawFastHLine(uilg::PAD, y - 2,
                                    SCREEN_W - 2 * uilg::PAD, COL_DIVIDER);
            }
        }

        drawFooter();
        return;
    }

    // ── Compact 160x80 layout (unchanged) ───────────────────────────────────
    int y = 14;
    int visible = min(ENTRIES_PER_PAGE, _count - _scrollPos);
    for (int i = 0; i < visible; i++) {
        int idx = _scrollPos + i;
        drawEntry(idx, y);

        // Calculate entry height (title + detail lines)
        int entryH = 10;  // title line
        if (strlen(_entries[idx].detail1) > 0) entryH += 9;
        if (strlen(_entries[idx].detail2) > 0) entryH += 9;
        y += entryH + 2;

        // Divider (if not last visible entry)
        if (i < visible - 1 && y < SCREEN_H - 14) {
            _tft->drawFastHLine(4, y - 1, SCREEN_W - 8, COL_DIVIDER);
        }
    }

    drawFooter();
#endif // !HEXHOUND_PANEL_ROUND
}

#if HEXHOUND_PANEL_ROUND

// ── Round journal (240x240) ───────────────────────────────────────────────
// Entries render at size 1 and centered. Size 2 is unusable here: a 20-char
// title is 240px wide, which exceeds the chord at every row.

void UIJournal::drawRound() {
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "JOURNAL", COL_HEADER);

    if (_count > ENTRIES_PER_PAGE) {
        int page = (_scrollPos / ENTRIES_PER_PAGE) + 1;
        int totalPages = ((_count - 1) / ENTRIES_PER_PAGE) + 1;
        uiround::centerPrintf(*_tft, uiround::scaled(54), 1, COL_PAGE, TFT_BLACK,
                              "%d/%d", page, totalPages);
    }

    if (_count == 0) {
        uiround::centerText(*_tft, uiround::scaled(112), "No entries yet", 1,
                            COL_DETAIL, TFT_BLACK);
        uiround::footer(*_tft, "hold=back", COL_FOOTER);
        return;
    }

    int y = uiround::scaled(68);
    const int maxY = uiround::FOOTER_DIV - uiround::scaled(4);
    int visible = min(ENTRIES_PER_PAGE, _count - _scrollPos);

    for (int i = 0; i < visible && y < maxY; i++) {
        const JournalEntry& e = _entries[_scrollPos + i];

        char titleLine[32];
        snprintf(titleLine, sizeof(titleLine), "%s %s", e.timestamp, e.title);
        uiround::centerText(*_tft, y, titleLine, 1, e.titleColor, TFT_BLACK);
        y += uiround::textH(1) + uiround::scaled(2);

        if (strlen(e.detail1) > 0 && y < maxY) {
            uiround::centerText(*_tft, y, e.detail1, 1, COL_DETAIL, TFT_BLACK);
            y += uiround::textH(1) + uiround::scaled(2);
        }
        if (strlen(e.detail2) > 0 && y < maxY) {
            uiround::centerText(*_tft, y, e.detail2, 1, COL_DETAIL, TFT_BLACK);
            y += uiround::textH(1) + uiround::scaled(2);
        }

        y += uiround::scaled(4);
        if (i < visible - 1 && y < maxY) {
            uiround::hLine(*_tft, y - uiround::scaled(2), COL_DIVIDER, uiround::scaled(40));
        }
    }

    uiround::footer(*_tft,
                    (_count > ENTRIES_PER_PAGE) ? "press=scroll  hold=back"
                                                : "hold=back",
                    COL_FOOTER);
}

#endif // HEXHOUND_PANEL_ROUND

void UIJournal::drawHeader() {
    if (SCREEN_H > 100) {
        // Big panel: shared size-2 title + underline, page indicator top-right.
        uiBigHeader(*_tft, "\x04 JOURNAL", COL_HEADER);

        if (_count > ENTRIES_PER_PAGE) {
            int page = (_scrollPos / ENTRIES_PER_PAGE) + 1;
            int totalPages = ((_count - 1) / ENTRIES_PER_PAGE) + 1;
            char pgBuf[12];
            snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
            int pgW = (int)strlen(pgBuf) * uilg::CHAR_W;
            _tft->setTextSize(2);
            _tft->setTextColor(COL_PAGE, TFT_BLACK);
            _tft->setCursor(SCREEN_W - pgW - uilg::PAD, uilg::TITLE_Y);
            _tft->print(pgBuf);
        }
        return;
    }

    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 JOURNAL");  // diamond char

    // Page indicator
    if (_count > ENTRIES_PER_PAGE) {
        int page = (_scrollPos / ENTRIES_PER_PAGE) + 1;
        int totalPages = ((_count - 1) / ENTRIES_PER_PAGE) + 1;
        _tft->setTextColor(COL_PAGE, TFT_BLACK);
        char pgBuf[8];
        snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
        int pgW = strlen(pgBuf) * 6;
        _tft->setCursor(SCREEN_W - pgW - 4, 2);
        _tft->print(pgBuf);
    }

    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
}

void UIJournal::drawFooter() {
    if (SCREEN_H > 100) {
        const char* left = (_count > ENTRIES_PER_PAGE) ? "scroll" : nullptr;
        uiBigFooter(*_tft, left, "hold=back", COL_FOOTER);
        return;
    }

    int footerY = SCREEN_H - 10;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);

    if (_count > ENTRIES_PER_PAGE) {
        _tft->setCursor(4, footerY);
        _tft->print("scroll");
    }
    _tft->setCursor(SCREEN_W - 54, footerY);
    _tft->print("hold=back");
}

void UIJournal::drawEntry(int idx, int y) {
    if (idx >= _count) return;
    const JournalEntry& e = _entries[idx];

    if (SCREEN_H > 100) {
        // ── Big-screen (320x172) size-2 entry ───────────────────────────────
        // Timestamp (5 chars) at x=PAD; title begins after it. 5*12 = 60,
        // + PAD + small gap -> ~78.
        const int titleX = uilg::PAD + 6 * uilg::CHAR_W;   // 6 => ""HH:MM"" + gap
        // Max chars per line from an x offset to the right pad.
        const int titleCols = (SCREEN_W - uilg::PAD - titleX) / uilg::CHAR_W;

        // Timestamp
        _tft->setTextSize(2);
        _tft->setTextColor(COL_TIMESTAMP, TFT_BLACK);
        _tft->setCursor(uilg::PAD, y);
        _tft->print(e.timestamp);

        // Title (right of timestamp, its own color)
        _tft->setTextColor(e.titleColor, TFT_BLACK);
        _tft->setCursor(titleX, y);
        char titleBuf[24];
        strlcpy(titleBuf, e.title, sizeof(titleBuf));
        if ((int)strlen(titleBuf) > titleCols && titleCols > 0)
            titleBuf[titleCols] = '\0';
        _tft->print(titleBuf);

        // Detail lines: indented to titleX, each on its own ~20px row.
        // Prefer size 2; drop to size 1 if the line is too wide to fit.
        const char* details[2] = { e.detail1, e.detail2 };
        for (int d = 0; d < 2; d++) {
            if (strlen(details[d]) == 0) continue;
            y += 20;
            _tft->setTextColor(COL_DETAIL, TFT_BLACK);
            if ((int)strlen(details[d]) > titleCols) {
                // Too wide for size 2 -> render at size 1 (more chars fit).
                _tft->setTextSize(1);
                int cols1 = (SCREEN_W - uilg::PAD - titleX) / 6;
                char dBuf[40];
                strlcpy(dBuf, details[d], sizeof(dBuf));
                if ((int)strlen(dBuf) > cols1 && cols1 > 0) dBuf[cols1] = '\0';
                _tft->setCursor(titleX, y + 4);   // vertically center-ish in row
                _tft->print(dBuf);
            } else {
                _tft->setTextSize(2);
                char dBuf[24];
                strlcpy(dBuf, details[d], sizeof(dBuf));
                _tft->setCursor(titleX, y);
                _tft->print(dBuf);
            }
        }
        return;
    }

    _tft->setTextSize(1);

    // Timestamp
    _tft->setTextColor(COL_TIMESTAMP, TFT_BLACK);
    _tft->setCursor(4, y);
    _tft->print(e.timestamp);

    // Title (right of timestamp)
    _tft->setTextColor(e.titleColor, TFT_BLACK);
    _tft->setCursor(38, y);
    // Truncate to fit
    char titleBuf[18];
    strlcpy(titleBuf, e.title, sizeof(titleBuf));
    _tft->print(titleBuf);

    // Detail line 1 (indented)
    if (strlen(e.detail1) > 0) {
        y += 9;
        _tft->setTextColor(COL_DETAIL, TFT_BLACK);
        _tft->setCursor(38, y);
        char d1Buf[20];
        strlcpy(d1Buf, e.detail1, sizeof(d1Buf));
        _tft->print(d1Buf);
    }

    // Detail line 2 (indented further)
    if (strlen(e.detail2) > 0) {
        y += 9;
        _tft->setTextColor(COL_DETAIL, TFT_BLACK);
        _tft->setCursor(38, y);
        char d2Buf[20];
        strlcpy(d2Buf, e.detail2, sizeof(d2Buf));
        _tft->print(d2Buf);
    }
}

void UIJournal::scrollDown() {
    if (_count <= ENTRIES_PER_PAGE) return;
    _scrollPos++;
    if (_scrollPos > _count - ENTRIES_PER_PAGE) {
        _scrollPos = 0;  // wrap to top
    }
    draw();
}
