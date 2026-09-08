#pragma once
#include "../hal/tft_compat.h"
#include "../config.h"

// ── HexHound - Alert Overlay UI ──────────────────────────────────

class UIAlert {
public:
    static UIAlert& instance();

    void init(TFT_eSPI* tft);
    void draw(NotifLevel level, const char* message, const char* source);

private:
    UIAlert() = default;
    TFT_eSPI* _tft = nullptr;
};

