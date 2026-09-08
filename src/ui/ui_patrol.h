#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"

// ── HexHound - Patrol Results Screen ────────────────────────────

// A patrol grants at most three distinct materials - scrap from networks seen,
// signal frags from new Wi-Fi, data shards from new BLE - so four is headroom.
#define PATROL_MAX_MATERIALS 4

struct PatrolSummary {
    // What the patrol actually put in the kit, as ITEM ids. Empty is the normal
    // case for a thin scan: every yield is integer division, so a patrol that
    // saw five networks legitimately banks nothing.
    uint8_t  materialId[PATROL_MAX_MATERIALS]  = { 0 };
    uint16_t materialQty[PATROL_MAX_MATERIALS] = { 0 };
    uint8_t  materialCount = 0;

    int wifiCount;
    int newWifiCount;
    int bleCount;
    int newBleCount;
    bool bleScanned;
    int openAPCount;
    int dupeCount;
    int trackerCount;
    int foodGained;
    int xpGained;
};

class UIPatrol {
public:
    static UIPatrol& instance();

    void init(TFT_eSPI* tft);
    void draw(const PatrolSummary& summary);

private:
    UIPatrol() = default;
#if HEXHOUND_PANEL_ROUND
    void drawRound(const PatrolSummary& summary);
#endif
    TFT_eSPI* _tft = nullptr;
};

