#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H
#include <math.h>
#include <string.h>

// ── HexHound - UI Trig & Drawing Utilities ──────────────────────

// Convert polar (angle in degrees, radius) to screen (x, y) from center
inline void polarToXY(float angleDeg, float radius,
                      int cx, int cy, int& x, int& y) {
    float rad = angleDeg * PI / 180.0f;
    x = cx + (int)(radius * cosf(rad));
    y = cy + (int)(radius * sinf(rad));
}

// Draw a 3×3 filled dot (visible at small scale)
inline void drawDot(TFT_eSPI& tft, int x, int y, uint16_t color) {
    tft.fillRect(x - 1, y - 1, 3, 3, color);
}

// Draw a 2×2 dot (tighter)
inline void drawSmallDot(TFT_eSPI& tft, int x, int y, uint16_t color) {
    tft.fillRect(x, y, 2, 2, color);
}

// Linearly interpolate between two RGB565 colors (crude but fast)
inline uint16_t dimColor(uint16_t color, uint8_t level) {
    // level 0=black, 4=full brightness
    if (level == 0) return TFT_BLACK;
    if (level >= 4) return color;
    uint16_t r = ((color >> 11) & 0x1F) * level / 4;
    uint16_t g = ((color >> 5)  & 0x3F) * level / 4;
    uint16_t b = ( color        & 0x1F) * level / 4;
    return (r << 11) | (g << 5) | b;
}

// Hash a string to a value in [0, maxVal)
inline int hashToRange(const char* str, int maxVal) {
    uint32_t h = 5381;
    while (*str) {
        h = ((h << 5) + h) + (uint8_t)*str++;
    }
    return (int)(h % maxVal);
}

// ── Large-panel screen chrome ─────────────────────────────────────────────
// Shared helpers for the Waveshare 320x172 layout so every screen shares the
// same header/footer geometry and size-2 font. The 160x80 T-Dongle keeps its
// own hand-tuned compact layouts (these are only called under SCREEN_H > 100).
//
// Layout budget (320x172):
//   title row   y=6   (size 2, 16px tall)
//   header rule y=28
//   body        y=34 .. 145
//   footer rule y=146
//   footer text y=150 (size 2)
namespace uilg {
    constexpr int PAD         = 6;
    constexpr int TITLE_Y     = 6;
    constexpr int HEADER_DIV  = 28;
    constexpr int BODY_Y      = 34;
    constexpr int FOOTER_DIV  = SCREEN_H - 26;   // 146
    constexpr int FOOTER_TEXT = SCREEN_H - 22;   // 150
    constexpr int CHAR_W      = 12;              // size-2 glyph width
    constexpr int CHAR_H      = 16;              // size-2 glyph height
}

// Standard header: size-2 title at top-left + full-width underline.
inline void uiBigHeader(TFT_eSPI& tft, const char* title, uint16_t color) {
    tft.setTextSize(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(uilg::PAD, uilg::TITLE_Y);
    tft.print(title);
    tft.drawFastHLine(0, uilg::HEADER_DIV, SCREEN_W, color);
}

// Standard footer: full-width rule + left/right size-2 hints (either may be null).
inline void uiBigFooter(TFT_eSPI& tft, const char* left, const char* right,
                        uint16_t color) {
    tft.drawFastHLine(0, uilg::FOOTER_DIV, SCREEN_W, color);
    tft.setTextSize(2);
    tft.setTextColor(color, TFT_BLACK);
    if (left && *left) {
        tft.setCursor(uilg::PAD, uilg::FOOTER_TEXT);
        tft.print(left);
    }
    if (right && *right) {
        int w = (int)strlen(right) * uilg::CHAR_W;
        tft.setCursor(SCREEN_W - w - uilg::PAD, uilg::FOOTER_TEXT);
        tft.print(right);
    }
}

// Draw a sprite scaled by integer factor (nearest-neighbor)
// Each source pixel becomes scale×scale filled rect. Skips transparentColor.
inline void drawScaledSprite(TFT_eSPI& tft, const uint16_t* sprite,
                             int srcW, int srcH,
                             int destX, int destY, int scale,
                             uint16_t transparentColor = 0xF81F) {
    for (int row = 0; row < srcH; row++) {
        for (int col = 0; col < srcW; col++) {
            uint16_t color = pgm_read_word(&sprite[row * srcW + col]);
            if (color != transparentColor) {
                int dx = destX + col * scale;
                int dy = destY + row * scale;
                if (scale == 1) {
                    tft.drawPixel(dx, dy, color);
                } else {
                    tft.fillRect(dx, dy, scale, scale, color);
                }
            }
        }
    }
}
