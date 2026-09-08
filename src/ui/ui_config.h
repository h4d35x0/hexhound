#pragma once
#include "../board/board_profile.h"   // HEXHOUND_HAS_SOFT_SLEEP
#include "../hal/tft_compat.h"

// ── HexHound - Config Screen (Trusted SSIDs & Hosts) ──────────

enum ConfigTab : uint8_t {
    TAB_SSIDS = 0,
    TAB_HOSTS
};

class UIConfig {
public:
    static UIConfig& instance();

    void init(TFT_eSPI* tft);

    // Open the config screen (resets cursor, loads data)
    void open();

    // Draw the current state
    void draw();
    void update();

    // Short press: scroll down through list items, execute action row
    // Returns true if a confirmation message is showing (don't navigate away)
    bool onShortPress();

    // Long press: switch tabs (SSID->HOSTS) or signal navigate-away
    // Returns true if caller should navigate back to menu (already on last tab)
    bool onLongPress();

    // Current tab
    ConfigTab tab() const { return _tab; }

private:
    UIConfig() = default;
    void drawTabBar();
    void drawSSIDList();
    void drawHostList();
    void drawSSIDActionRow(int y, bool selected);
    void drawHostActionRow(int y, bool selected);
#if HEXHOUND_HAS_SOFT_SLEEP
    // Only declared on a board that can actually sleep. Kept inside the guard
    // rather than compiled-and-unused, because an out-of-line copy of a helper
    // in this shared file has already moved three other boards' images by 16
    // bytes each without changing a pixel.
    void drawSleepRow(int y, bool selected);
#endif
    void drawFooter();
    void drawConfirmation(const char* msg);
    int  actionRowY() const;
    int  totalRows() const;  // total interactive rows (tab + entries + action)

    TFT_eSPI* _tft = nullptr;

    ConfigTab _tab     = TAB_SSIDS;
    int       _cursor  = 0;    // 0=tab bar, 1..N=list entries, N+1=add action
    bool      _confirmActive = false;
    uint32_t  _confirmEnd    = 0;
    int       _lastPulsePhase = -1;
    char      _confirmMsg[24];
};
