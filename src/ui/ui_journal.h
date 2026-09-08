#pragma once
#include "../hal/tft_compat.h"
#include "../config.h"          // pulls board_profile.h -> SCREEN_W / SCREEN_H

// ── HexHound - Journal Log Screen ───────────────────────────────

#define MAX_JOURNAL_DISPLAY 50
// Runtime expression (never used as a compile-time array bound): 4 entries fit
// the 320x172 panel at size-2 font, 2 on the compact 160x80 T-Dongle.
// Round 240x240 renders entries at size 1 (size-2 detail lines do not fit the
// chord once you are near the rim), so more of them fit per page.
#define ENTRIES_PER_PAGE    (HEXHOUND_PANEL_ROUND ? 4 : ((SCREEN_H > 100) ? 4 : 2))

struct JournalEntry {
    char     timestamp[6];    // "HH:MM"
    char     title[20];
    char     detail1[28];
    char     detail2[28];
    uint16_t titleColor;
};

class UIJournal {
public:
    static UIJournal& instance();

    void init(TFT_eSPI* tft);

    // Load and parse journal entries from storage
    void loadEntries();

    // Draw the current page
    void draw();

    // Scroll down one entry (short press). Wraps at bottom.
    void scrollDown();

    int entryCount() const { return _count; }

private:
    UIJournal() = default;
    void parseLine(const char* line, JournalEntry& entry);
    void drawEntry(int idx, int y);
    void drawHeader();
    void drawFooter();
#if HEXHOUND_PANEL_ROUND
    void drawRound();
#endif

    TFT_eSPI* _tft = nullptr;

    JournalEntry _entries[MAX_JOURNAL_DISPLAY];
    int _count     = 0;   // total entries loaded
    int _scrollPos = 0;   // index of top visible entry
};

