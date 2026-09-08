#pragma once

// ── HexHound - LilyGo T-RGB panel driver ─────────────────────────
//
// ST7701S 480x480 on the ESP32-S3's RGB parallel LCD peripheral, with a
// framebuffer in PSRAM. This is the only board in the fleet whose panel is not
// driven over SPI, so none of the TFT_eSPI or vendor-ST7735 machinery applies.
//
// The configuration here is the one CONFIRMED ON GLASS during bring-up:
// V1 init table, V1 data pin map, 8 MHz pixel clock, RGB colour order. The
// alternative revisions are deliberately not carried into the release driver;
// [env:lilygo-t-rgb-bringup] still cycles through all four if another unit
// ever disagrees.
//
// Two things about this board that are easy to get wrong:
//   * The panel's own SPI init lines (CS/MOSI/SCLK/RST), the touch reset and
//     the SD enable are pins on an XL9535 I2C expander, NOT GPIOs. The init
//     table is bit-banged out over I2C, which is why begin() costs ~530 ms.
//   * Expander IO2 is a power enable that must be driven HIGH. On USB the
//     board appears to work without it; on battery it does not come up at all.

#if defined(HEXHOUND_RGB_PANEL)

#include <cstdint>

namespace trgb {

constexpr int PANEL_W = 480;
constexpr int PANEL_H = 480;

// Brings up I2C, the XL9535 expander, the power rail, the touch reset line and
// the RGB panel itself. Returns false if the expander or the panel could not be
// started; the caller should treat that as a dead display rather than retry.
bool begin();

// Pushes `rows` full-width rows starting at `y0` from `src`, which must point
// at the first pixel of that band in a 480-wide RGB565 buffer.
//
// Full width is not a limitation, it is the point: esp_lcd_panel_draw_bitmap
// needs a contiguous buffer, and a sub-rectangle of a 480-wide canvas is not
// contiguous while a full-width band always is. Cost is ~49 us per row.
void blitRows(int y0, int rows, const uint16_t* src);

// The backlight is a pulse-counted 16-step dimmer, not PWM. Held LOW it
// resets; a LOW->HIGH edge selects full brightness and each further LOW/HIGH
// pulse steps it down one notch.
void backlightOn();
void backlightOff();

// 0 until begin() succeeds. Exposed so the touch layer can share the bus
// without probing for the expander a second time.
uint8_t expanderAddress();

// Blanks the panel and puts the S3 into deep sleep. DOES NOT RETURN.
//
// The caller is responsible for everything above the hardware - saving state,
// telling the owner what is about to happen - because this function's last act
// is esp_deep_sleep_start(). Wake is a full boot back through setup(), so
// begin() runs again and rebuilds the panel from scratch.
//
// See the long comment at the definition for why this board cannot copy the
// T-Display S3's "cut the panel rail and gpio_hold_en it" approach, and what it
// does instead.
void enterDeepSleep();

}  // namespace trgb

#endif  // HEXHOUND_RGB_PANEL
