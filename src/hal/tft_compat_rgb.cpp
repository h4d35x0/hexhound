// ── HexHound - TFT_eSPI backend for the T-RGB RGB parallel panel ──
//
// Only the functions that actually touch hardware live here. Everything the
// UI calls that is composed from drawPixel()/fillRect() - drawLine, drawCircle,
// drawRect, the GLCD font, print/printf/drawString - is shared verbatim with
// tft_compat.cpp, which fences off its SPI-only blocks with
// #ifndef HEXHOUND_RGB_PANEL.
//
// WHY A CANVAS
// ------------
// The RGB peripheral scans a framebuffer out continuously at a rate fixed by
// the timings (26 Hz here). The Arduino 2.0.17 IDF exposes only
// esp_lcd_new_rgb_panel: there is no bounce buffer and no
// esp_lcd_rgb_panel_get_frame_buffer (that arrived in IDF 5.0), so pixels can
// only reach the panel through an esp_lcd_panel_draw_bitmap copy. Drawing in
// place is not available. So the UI draws into a PSRAM canvas and the dirty
// part is copied out once per loop iteration.
//
// WHY THE DIRTY REGION IS A Y-RANGE, NOT A RECTANGLE
// --------------------------------------------------
// draw_bitmap needs a CONTIGUOUS buffer for the rectangle it is handed. An
// arbitrary sub-rectangle of a 480-wide canvas is not contiguous, which would
// force a staging copy on every update. A full-width band always is. So the
// dirty region is tracked as a Y-range and blitted at full width: no staging
// buffer, no per-rect copy, and cost stays proportional to dirty height.
//
// Measured on hardware: ~49 us per row, so a 48-row widget update costs about
// 2.4 ms. A full-screen redraw is 23.6 ms of blit on top of 15.2 ms of canvas
// fill, which is the entire 38 ms budget at 26 Hz - hence the whole design.

#include "tft_compat.h"

#if defined(HEXHOUND_RGB_PANEL)

#include "../config.h"
#include "rgb_panel_trgb.h"
#include <esp_heap_caps.h>
#include <math.h>

namespace {
constexpr int CANVAS_W = trgb::PANEL_W;
constexpr int CANVAS_H = trgb::PANEL_H;
}  // namespace

TFT_eSPI::TFT_eSPI()
    : _initialized(false),
      _inTransaction(false),
      _rotation(0),
      _colstart(0),
      _rowstart(0),
      _width(CANVAS_W),
      _height(CANVAS_H),
      _textFg(TFT_WHITE),
      _textBg(TFT_BLACK),
      _textSize(1),
      _cursorX(0),
      _cursorY(0),
      _canvas(nullptr),
      _dirtyTop(CANVAS_H),
      _dirtyBottom(-1) {}

void TFT_eSPI::init() {
    if (_initialized) {
        return;
    }

    _canvas = (uint16_t*)heap_caps_malloc((size_t)CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!_canvas) {
        // 460800 bytes out of 8 MB. If this fails the board is misconfigured
        // (memory_type must be qio_opi), and there is no smaller fallback that
        // would render a 480x480 panel correctly.
        Serial.println("[TRGB] FATAL: canvas alloc failed (need 460800 B PSRAM)");
        return;
    }

    if (!trgb::begin()) {
        Serial.println("[TRGB] FATAL: panel bring-up failed, display is dead");
        heap_caps_free(_canvas);
        _canvas = nullptr;
        return;
    }

    _initialized = true;
    Serial.println("[TRGB] backend ready");

    // Clear to black and push it before the backlight comes up, so the first
    // thing the owner sees is the UI rather than uninitialised PSRAM.
    fillScreen(TFT_BLACK);
    present();
    trgb::backlightOn();

#ifdef HEXHOUND_RGB_SELFTEST
    // Bring-up aid: a brief full-field colour proves the panel, the blit path
    // and the backlight are all working, independently of whether the app's
    // own drawing ever reaches the glass. Without it a black screen is
    // ambiguous between "panel dead" and "nothing drew".
    Serial.println("[TRGB] selftest: RED");
    fillScreen(TFT_RED);   present();  delay(700);
    Serial.println("[TRGB] selftest: GREEN");
    fillScreen(TFT_GREEN); present();  delay(700);
    Serial.println("[TRGB] selftest: BLUE");
    fillScreen(TFT_BLUE);  present();  delay(700);
    Serial.println("[TRGB] selftest: done, back to black");
    fillScreen(TFT_BLACK); present();
#endif
}

void TFT_eSPI::markDirty(int y, int h) {
    if (h <= 0) {
        return;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y >= CANVAS_H || h <= 0) {
        return;
    }
    int bottom = y + h - 1;
    if (bottom >= CANVAS_H) {
        bottom = CANVAS_H - 1;
    }
    if (y < _dirtyTop) {
        _dirtyTop = y;
    }
    if (bottom > _dirtyBottom) {
        _dirtyBottom = bottom;
    }
}

void TFT_eSPI::present() {
    if (!_initialized || !_canvas || _dirtyTop > _dirtyBottom) {
        return;
    }
    const int rows = _dirtyBottom - _dirtyTop + 1;
#ifdef HEXHOUND_RGB_SELFTEST
    // Log the first handful of blits. If these keep arriving while the glass
    // stays black, the app is drawing and the panel is not showing it; if they
    // stop, nothing is drawing and the panel is not the problem.
    static int blitLog = 0;
    if (blitLog < 24) {
        blitLog++;
        Serial.printf("[TRGB] present #%d: rows %d..%d\n", blitLog, _dirtyTop, _dirtyBottom);
    }
#endif
    trgb::blitRows(_dirtyTop, rows, _canvas + (size_t)_dirtyTop * CANVAS_W);
    _dirtyTop    = CANVAS_H;
    _dirtyBottom = -1;
}

void TFT_eSPI::fillScreen(uint16_t color) {
    fillRect(0, 0, CANVAS_W, CANVAS_H, color);
}

void TFT_eSPI::drawPixel(int x, int y, uint16_t color) {
    if (!_canvas || x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H) {
        return;
    }
    _canvas[(size_t)y * CANVAS_W + x] = color;
    markDirty(y, 1);
}

void TFT_eSPI::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (!_canvas || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= CANVAS_W || y >= CANVAS_H) {
        return;
    }
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; ++row) {
        uint16_t* p = _canvas + (size_t)(y + row) * CANVAS_W + x;
        for (int col = 0; col < w; ++col) {
            p[col] = color;
        }
    }
    markDirty(y, h);
}

void TFT_eSPI::setRotation(int r) {
    // Recorded for callers that read it back, but there is nothing to apply.
    // In the product orientation (USB-C on the viewer's right) the panel's scan
    // already lands upright, so the ST7701S MADCTL stays at LilyGo's 0x08 and
    // no software rotation is needed. Rotating here would cost a full-canvas
    // transform per frame for no benefit.
    _rotation = r & 0x03;
}

// An RGB parallel panel has no command channel once esp_lcd owns the bus: the
// ST7701S register interface is bit-banged over the I2C expander during init
// and is not reachable afterwards. These exist to satisfy the shared surface.
void TFT_eSPI::writecommand(uint8_t) {}
void TFT_eSPI::writedata(uint8_t) {}
void TFT_eSPI::invertDisplay(bool) {}

#endif  // HEXHOUND_RGB_PANEL
