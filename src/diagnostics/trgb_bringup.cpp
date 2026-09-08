// ── HexHound - LilyGo T-RGB bring-up diagnostic ──────────────────
//
// STANDALONE. This file shares nothing with the HexHound application: it has
// its own setup()/loop(), and its env compiles this translation unit and no
// other.
//
// A standalone ENV is not by itself enough to keep the other seven boards safe,
// which is worth stating because the first version of this file got it wrong.
// Their build_src_filter is `+<*>`, so they happily compiled this file too and
// failed to link on a duplicate setup()/loop(). The HEXHOUND_TRGB_BRINGUP guard
// below is what actually makes the addition additive.
//
// Build:   pio run -e lilygo-t-rgb-bringup
// Flash:   pio run -e lilygo-t-rgb-bringup -t upload --upload-port COM22
// Monitor: pio device monitor -b 115200 --port COM22
//
// WHAT THIS ANSWERS
// -----------------
// The 2.1" panel ships as two electrically different revisions. They disagree
// about the RGB data pin map, the pixel clock (8 vs 10 MHz) and the porch
// timings, so guessing wrong yields a dark or scrambled screen with no
// diagnostic. This sketch cycles through the candidate configurations and
// draws a pattern designed to make a wrong guess obvious rather than merely
// ugly. Whichever mode renders cleanly IS the board's identity.
//
// WHY THERE IS NO LIBRARY HERE
// ----------------------------
// LilyGo's own driver calls extension.transfer9() / extension.beginSPI() on
// SensorLib's ExtensionIOXL9555, and pins SensorLib to 0.2.3 with an #error
// guarding against anything newer. Neither method exists in the v0.2.3 source
// tree, nor in any later tag through v0.4.1. Rather than gamble on a registry
// build differing from the git tag, the ~60 lines of XL9535 driver this needs
// are written out below. The part is PCA9535-compatible and trivial.
//
// PIN FACTS, all read from LilyGo's utilities.h / LilyGo_RGBPanel.cpp:
//   * Panel SPI init lines are NOT GPIOs. CS/MOSI/SCLK/RST live on an XL9535
//     I2C expander, so the ST7701S init table is bit-banged over I2C.
//   * Expander IO2 is power_enable and MUST be driven HIGH; on battery the
//     board does not come up at all without it.
//   * The backlight on GPIO46 is a pulse-counted 16-step dimmer, not PWM.
//     Held LOW it resets; a LOW->HIGH edge selects full brightness, and each
//     subsequent LOW/HIGH pulse steps it down one notch.

// Guarded so this translation unit is empty for every other env. Without it
// the seven existing boards, whose build_src_filter is `+<*>`, pick this file
// up and fail to link with a duplicate setup()/loop(). Same approach as
// src/diagnostics/step_capture.*: no release image carries any of this.
#if defined(HEXHOUND_TRGB_BRINGUP)

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

// ── Board wiring ───────────────────────────────────────────────────────────

static constexpr int PANEL_W = 480;
static constexpr int PANEL_H = 480;

static constexpr int PIN_BL    = 46;
static constexpr int PIN_HSYNC = 47;
static constexpr int PIN_VSYNC = 41;
static constexpr int PIN_DE    = 45;
static constexpr int PIN_PCLK  = 42;

static constexpr int PIN_I2C_SDA = 8;
static constexpr int PIN_I2C_SCL = 48;

// Expander pin numbers (port 0 bit positions), not GPIOs.
static constexpr uint8_t XIO_TP_RESET     = 1;
static constexpr uint8_t XIO_POWER_ENABLE = 2;
static constexpr uint8_t XIO_LCD_CS       = 3;
static constexpr uint8_t XIO_LCD_MOSI     = 4;
static constexpr uint8_t XIO_LCD_SCLK     = 5;
static constexpr uint8_t XIO_LCD_RST      = 6;
static constexpr uint8_t XIO_SDMMC_CS     = 7;

// The panel is physically RGB666; the S3 drives the low 16 lines as RGB565.
// These maps are transcribed from LilyGo_RGBPanel.cpp and differ per revision.
// Index 0 is the LSB of blue, index 15 the MSB of red.
static const int kDataMapV1[16] = {
    7, 6, 5, 3, 2,            // DATA13..DATA17
    14, 13, 12, 11, 10, 9,    // DATA6..DATA11
    21, 18, 17, 16, 15        // DATA1..DATA5
};

static const int kDataMapV2[16] = {
    43, 7, 6, 5, 3,           // DATA12..DATA16
    14, 13, 12, 11, 10, 9,    // DATA6..DATA11
    44, 21, 18, 17, 16        // DATA0..DATA4
};

// Alternate ordering LilyGo selects for BGR panels.
static const int kDataMapBGR[16] = {
    7, 6, 5, 3, 2,            // DATA13..DATA17
    44, 21, 18, 17, 16, 15,   // DATA0..DATA5
    13, 12, 11, 10, 9         // DATA7..DATA11
};

// ── ST7701S init tables ────────────────────────────────────────────────────
// databytes: low 5 bits = payload length, bit 7 = delay 100 ms afterwards,
// 0xFF terminates. Transcribed verbatim from LilyGo's RGBPanelInit.h.

struct st7701_cmd_t {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;
};

static const st7701_cmd_t kInitV1[] = {
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 0x05},
    {0xC0, {0x3b, 0x00}, 0x02},
    {0xC1, {0x0b, 0x02}, 0x02},
    {0xC2, {0x07, 0x02}, 0x02},
    {0xCC, {0x10}, 0x01},
    {0xCD, {0x08}, 0x01},
    {0xb0, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}, 0x10},
    {0xb1, {0x00, 0x11, 0x16, 0x0e, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2a, 0x31, 0x18}, 0x10},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 0x05},
    {0xb0, {0x6d}, 0x01},
    {0xb1, {0x37}, 0x01},
    {0xb2, {0x81}, 0x01},
    {0xb3, {0x80}, 0x01},
    {0xb5, {0x43}, 0x01},
    {0xb7, {0x85}, 0x01},
    {0xb8, {0x20}, 0x01},
    {0xc1, {0x78}, 0x01},
    {0xc2, {0x78}, 0x01},
    {0xc3, {0x8c}, 0x01},
    {0xd0, {0x88}, 0x01},
    {0xe0, {0x00, 0x00, 0x02}, 0x03},
    {0xe1, {0x03, 0xa0, 0x00, 0x00, 0x04, 0xa0, 0x00, 0x00, 0x00, 0x20, 0x20}, 0x0b},
    {0xe2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x0d},
    {0xe3, {0x00, 0x00, 0x11, 0x00}, 0x04},
    {0xe4, {0x22, 0x00}, 0x02},
    {0xe5, {0x05, 0xec, 0xa0, 0xa0, 0x07, 0xee, 0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x10},
    {0xe6, {0x00, 0x00, 0x11, 0x00}, 0x04},
    {0xe7, {0x22, 0x00}, 0x02},
    {0xe8, {0x06, 0xed, 0xa0, 0xa0, 0x08, 0xef, 0xa0, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0x10},
    {0xeb, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}, 0x07},
    {0xed, {0xff, 0xff, 0xff, 0xba, 0x0a, 0xbf, 0x45, 0xff, 0xff, 0x54, 0xfb, 0xa0, 0xab, 0xff, 0xff, 0xff}, 0x10},
    {0xef, {0x10, 0x0d, 0x04, 0x08, 0x3f, 0x1f}, 0x06},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 0x05},
    {0xef, {0x08}, 0x01},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 0x05},
    {0x36, {0x08}, 0x01},
    {0x3a, {0x66}, 0x01},
    {0x11, {0x00}, 0x80},
    {0x29, {0x00}, 0x80},
    {0, {0}, 0xff}
};

static const st7701_cmd_t kInitV2[] = {
    {0x3A, {0x50}, 0x01},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 0x05},
    {0xC0, {0x3b, 0x00}, 0x02},
    {0xC1, {0x06, 0x05}, 0x02},
    {0xC2, {0x37, 0x02}, 0x02},
    {0xC6, {0x21}, 0x01},
    {0xC3, {0x02}, 0x01},
    {0xCC, {0x30}, 0x01},
    {0xb0, {0xc0, 0x54, 0x5c, 0x0d, 0x51, 0x06, 0x09, 0x08, 0x07, 0x24, 0x03, 0x11, 0x0f, 0xac, 0xb5, 0x7f}, 0x10},
    {0xb1, {0xc0, 0x54, 0x5c, 0x0e, 0x11, 0x07, 0x0a, 0x09, 0x08, 0x24, 0x04, 0x51, 0x10, 0xad, 0x75, 0x7f}, 0x10},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 0x05},
    {0xb0, {0x7d}, 0x01},
    {0xb1, {0x3b}, 0x01},
    {0xb2, {0x07}, 0x01},
    {0xb3, {0x80}, 0x01},
    {0xb5, {0x45}, 0x01},
    {0xb7, {0x87}, 0x01},
    {0xb8, {0x33}, 0x01},
    {0xB9, {0x10}, 0x01},
    {0xBB, {0x03}, 0x01},
    {0xC0, {0x03}, 0x01},
    {0xc1, {0x70}, 0x01},
    {0xc2, {0x70}, 0x01},
    {0xd0, {0x88}, 0x01},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 0x05},
    {0xe0, {0x00, 0x18, 0x00, 0x00, 0x00, 0x20}, 0x06},
    {0xe1, {0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x22, 0x22}, 0x0b},
    {0xe2, {0x10, 0x10, 0x20, 0x20, 0xe7, 0x00, 0x00, 0x00, 0xe6, 0x00, 0x00, 0x00, 0x00}, 0x0d},
    {0xe3, {0x00, 0x00, 0x11, 0x11}, 0x04},
    {0xe4, {0x44, 0x44}, 0x02},
    {0xe5, {0x03, 0xE0, 0x00, 0xF5, 0x05, 0xe2, 0x00, 0xf5, 0x07, 0xe4, 0x00, 0xf5, 0x09, 0xe6, 0x00, 0xf5}, 0x10},
    {0xe6, {0x00, 0x00, 0x11, 0x11}, 0x04},
    {0xe7, {0x44, 0x44}, 0x02},
    {0xe8, {0x02, 0xDF, 0x00, 0xf5, 0x04, 0xe1, 0x00, 0xf5, 0x06, 0xe3, 0x00, 0xf5, 0x08, 0xe5, 0x00, 0xf5}, 0x10},
    {0xeb, {0x00, 0x02, 0xe4, 0xe4, 0x88, 0x00, 0x10}, 0x07},
    {0xEC, {0x3D, 0x02, 0x00}, 0x03},
    {0xed, {0x20, 0x76, 0x54, 0x98, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xab, 0x89, 0x45, 0x67, 0x02}, 0x10},
    {0xef, {0x00, 0x00, 0x04, 0x00, 0x3f, 0x1f}, 0x06},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 0x05},
    {0xE8, {0x00, 0x0E}, 0x02},
    {0xE8, {0x00, 0x0C}, 0x02},
    {0xE8, {0x00, 0x00}, 0x02},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 0x05},
    {0x36, {0x08}, 0x01},
    {0x11, {0x00}, 0x80},
    {0x29, {0x00}, 0x80},
    {0x20, {0x00}, 0x01},
    {0, {0}, 0xff}
};

// ── XL9535 I2C GPIO expander (PCA9535-compatible) ──────────────────────────
// Only port 0 is used on this board; every expander pin T-RGB needs is 1..7.

namespace xl9535 {

static constexpr uint8_t REG_OUTPUT0 = 0x02;
static constexpr uint8_t REG_CONFIG0 = 0x06;

static uint8_t s_addr   = 0;
static uint8_t s_output = 0xFF;   // shadow of the output port
static uint8_t s_config = 0xFF;   // 1 = input (power-on default)

static bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// Probes 0x20..0x27 and latches the first responder.
static bool begin() {
    for (uint8_t a = 0x20; a <= 0x27; ++a) {
        if (present(a)) {
            s_addr = a;
            return true;
        }
    }
    return false;
}

static uint8_t address() { return s_addr; }

static void pinModeOutput(uint8_t pin) {
    s_config &= ~(uint8_t)(1u << pin);
    writeReg(REG_CONFIG0, s_config);
}

// Stages a level in the shadow without touching the bus.
static inline void stage(uint8_t pin, bool level) {
    if (level) {
        s_output |= (uint8_t)(1u << pin);
    } else {
        s_output &= ~(uint8_t)(1u << pin);
    }
}

static inline void flush() { writeReg(REG_OUTPUT0, s_output); }

static void digitalWriteX(uint8_t pin, bool level) {
    stage(pin, level);
    flush();
}

// 9-bit SPI word, MSB first. Bit 8 is the data/command flag: 0 = command.
// Staging MOSI and the falling SCLK edge together keeps this to two bus
// transactions per bit instead of three.
static void transfer9(uint16_t word) {
    digitalWriteX(XIO_LCD_CS, false);
    for (int i = 8; i >= 0; --i) {
        stage(XIO_LCD_MOSI, (word >> i) & 1u);
        stage(XIO_LCD_SCLK, false);
        flush();
        digitalWriteX(XIO_LCD_SCLK, true);
    }
    digitalWriteX(XIO_LCD_CS, true);
}

}  // namespace xl9535

static void st7701WriteCommand(uint8_t cmd) {
    xl9535::transfer9(cmd);
}

static void st7701WriteData(const uint8_t *data, int len) {
    for (int i = 0; i < len; ++i) {
        xl9535::transfer9((uint16_t)data[i] | 0x100u);
    }
}

// ── Modes ──────────────────────────────────────────────────────────────────

struct Mode {
    const char       *name;
    const st7701_cmd_t *init;
    const int        *dataMap;
    uint32_t          pclkHz;
    uint16_t          hSyncPulse, hBackPorch, hFrontPorch;
    uint16_t          vSyncPulse, vBackPorch, vFrontPorch;
    bool              swapColorOrder;   // issues 0x36 = 0x00 after init
};

static const Mode kModes[] = {
    {"V1 / RGB", kInitV1, kDataMapV1,  8000000UL, 1, 30, 50, 1, 30, 20, false},
    {"V1 / BGR", kInitV1, kDataMapBGR, 8000000UL, 1, 30, 50, 1, 30, 20, true },
    {"V2 / RGB", kInitV2, kDataMapV2, 10000000UL, 2, 34, 20, 2, 20, 50, false},
    // V2+BGR is an extrapolation: LilyGo's driver has no defined pin map for
    // this combination, so it keeps the V2 map and only swaps via 0x36.
    {"V2 / BGR", kInitV2, kDataMapV2, 10000000UL, 2, 34, 20, 2, 20, 50, true },
};
static constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// Survives ESP.restart(), which is how the mode is advanced. Re-initialising
// the RGB peripheral in place is fragile; rebooting into the next mode is not.
static constexpr uint32_t kRtcMagic = 0x54524742UL;   // 'TRGB'
RTC_NOINIT_ATTR static uint32_t g_rtcMagic;
RTC_NOINIT_ATTR static uint8_t  g_mode;
RTC_NOINIT_ATTR static uint8_t  g_attempts;

static esp_lcd_panel_handle_t g_panel = nullptr;
static uint16_t              *g_canvas = nullptr;

// ── Tiny drawing primitives, straight into the PSRAM canvas ────────────────

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    for (int row = 0; row < h; ++row) {
        uint16_t *p = g_canvas + (size_t)(y + row) * PANEL_W + x;
        for (int col = 0; col < w; ++col) p[col] = c;
    }
}

static void drawCircleOutline(int cx, int cy, int r, uint16_t c) {
    // Midpoint circle, plotting all eight octants.
    int x = r, y = 0, err = 1 - r;
    auto plot = [&](int px, int py) {
        if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) {
            g_canvas[(size_t)py * PANEL_W + px] = c;
        }
    };
    while (x >= y) {
        plot(cx + x, cy + y); plot(cx + y, cy + x);
        plot(cx - y, cy + x); plot(cx - x, cy + y);
        plot(cx - x, cy - y); plot(cx - y, cy - x);
        plot(cx + y, cy - x); plot(cx + x, cy - y);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

// A pattern chosen so a WRONG configuration is unmistakable rather than merely
// unattractive: the colour bars expose a swapped R/B order by reordering
// themselves, and the per-channel ramps turn banded or noisy the moment a data
// pin is misassigned.
static void drawTestPattern(int mode) {
    const uint16_t black = rgb565(0, 0, 0);
    fillRect(0, 0, PANEL_W, PANEL_H, black);

    // Panel extent and a safe-area guide, useful later for the half-circle mask.
    drawCircleOutline(240, 240, 239, rgb565(255, 255, 255));
    drawCircleOutline(240, 240, 200, rgb565(60, 60, 60));
    drawCircleOutline(240, 240, 160, rgb565(60, 60, 60));

    // Eight colour bars. Order left to right: white, yellow, cyan, green,
    // magenta, red, blue, black.
    const uint16_t bars[8] = {
        rgb565(255, 255, 255), rgb565(255, 255, 0), rgb565(0, 255, 255),
        rgb565(0, 255, 0),     rgb565(255, 0, 255), rgb565(255, 0, 0),
        rgb565(0, 0, 255),     rgb565(30, 30, 30)
    };
    for (int i = 0; i < 8; ++i) {
        fillRect(60 + i * 45, 100, 45, 80, bars[i]);
    }

    // Per-channel ramps across 360 px.
    for (int x = 0; x < 360; ++x) {
        int v = (x * 255) / 359;
        fillRect(60 + x, 190, 1, 40, rgb565((uint8_t)v, 0, 0));
        fillRect(60 + x, 235, 1, 40, rgb565(0, (uint8_t)v, 0));
        fillRect(60 + x, 280, 1, 40, rgb565(0, 0, (uint8_t)v));
    }

    // Crosshair through the centre.
    fillRect(0, 239, PANEL_W, 2, rgb565(120, 120, 120));
    fillRect(239, 0, 2, PANEL_H, rgb565(120, 120, 120));

    // Corner markers, each a different colour so rotation and mirroring show up.
    fillRect(8, 8, 20, 20, rgb565(255, 0, 0));
    fillRect(452, 8, 20, 20, rgb565(0, 255, 0));
    fillRect(8, 452, 20, 20, rgb565(0, 0, 255));
    fillRect(452, 452, 20, 20, rgb565(255, 255, 255));

    // Edge midpoint markers.
    fillRect(232, 4, 16, 16, rgb565(255, 255, 0));
    fillRect(232, 460, 16, 16, rgb565(0, 255, 255));
    fillRect(4, 232, 16, 16, rgb565(255, 0, 255));
    fillRect(460, 232, 16, 16, rgb565(255, 140, 0));

    // Mode indicator: (mode + 1) filled dots, low and centred so they land
    // inside the visible area on a half circle.
    for (int i = 0; i <= mode; ++i) {
        fillRect(200 + i * 24, 380, 16, 16, rgb565(255, 255, 255));
    }
}

// A flat field is the honest test for banding. The composite pattern cannot
// distinguish a real data-bit fault from camera moiré, because bright flat
// areas alias against the LCD grid in a photograph either way; looking at an
// unbroken field of one colour with the naked eye can.
static void drawFlatField(uint16_t c) {
    fillRect(0, 0, PANEL_W, PANEL_H, c);
}

// Makes the panel's true "up" unambiguous, which the symmetric test pattern
// cannot: a round panel photographed at any angle looks equally plausible.
static void drawOrientation() {
    fillRect(0, 0, PANEL_W, PANEL_H, rgb565(0, 0, 0));

    // Thick band along the canvas TOP edge only, and a narrower one down the
    // canvas LEFT edge. Two different edges, two different colours, no symmetry.
    fillRect(0, 0, PANEL_W, 28, rgb565(255, 255, 255));
    fillRect(0, 0, 14, PANEL_H, rgb565(255, 0, 0));

    // Solid triangle pointing towards canvas-up, centred.
    const int apexY = 120, baseY = 330, cx = 240;
    for (int y = apexY; y <= baseY; ++y) {
        int halfWidth = ((y - apexY) * 150) / (baseY - apexY);
        fillRect(cx - halfWidth, y, halfWidth * 2 + 1, 1, rgb565(0, 255, 0));
    }

    // One dot at canvas top-centre, two side by side at canvas bottom-centre.
    fillRect(232, 40, 16, 16, rgb565(255, 255, 0));
    fillRect(212, 420, 16, 16, rgb565(0, 200, 255));
    fillRect(252, 420, 16, 16, rgb565(0, 200, 255));

    drawCircleOutline(240, 240, 239, rgb565(120, 120, 120));
}

// ── Bring-up steps ─────────────────────────────────────────────────────────

static void scanI2C() {
    Serial.println("[I2C] scanning 0x03..0x77 on SDA=8 SCL=48");
    int found = 0;
    for (uint8_t a = 0x03; a <= 0x77; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C]   device at 0x%02X%s\n", a,
                          (a >= 0x20 && a <= 0x27) ? "  <- XL9535 expander"
                          : (a == 0x38)            ? "  <- FT3267 touch"
                          : (a == 0x15)            ? "  <- CST820 touch"
                          : (a == 0x5D || a == 0x14) ? "  <- GT911 touch"
                                                     : "");
            found++;
        }
    }
    Serial.printf("[I2C] %d device(s)\n", found);
}

// FocalTech parts require a REPEATED START between the register address and
// the read. Ending the write with a STOP makes them return 0x00 for every
// register, which looks exactly like a dead controller and is not one.
static bool readTouchReg(uint8_t reg, uint8_t &out) {
    Wire.beginTransmission(0x38);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // false = repeated start
    if (Wire.requestFrom((uint8_t)0x38, (uint8_t)1) != 1) return false;
    out = Wire.read();
    return true;
}

static void identifyTouch() {
    uint8_t chipId = 0, vendorId = 0, firmware = 0;
    if (!readTouchReg(0xA3, chipId)) {
        Serial.println("[TOUCH] nothing answering at 0x38 (expected FT3267)");
        return;
    }
    readTouchReg(0xA8, vendorId);
    readTouchReg(0xA6, firmware);

    Serial.printf("[TOUCH] 0x38 chip_id=0x%02X vendor_id=0x%02X fw=0x%02X%s\n",
                  chipId, vendorId, firmware,
                  (chipId == 0x33 || chipId == 0xE7 || chipId == 0x64)
                      ? "  (FocalTech FT3267 family)" : "");
}

static bool startPanel(const Mode &m) {
    // Reset the panel, then clock the init table out through the expander.
    xl9535::digitalWriteX(XIO_LCD_RST, false);
    delay(20);
    xl9535::digitalWriteX(XIO_LCD_RST, true);
    delay(10);

    Wire.setClock(1000000UL);
    const uint32_t t0 = millis();

    for (int i = 0; m.init[i].databytes != 0xFF; ++i) {
        st7701WriteCommand(m.init[i].cmd);
        st7701WriteData(m.init[i].data, m.init[i].databytes & 0x1F);
        if (m.init[i].databytes & 0x80) delay(100);
    }
    if (m.swapColorOrder) {
        const uint8_t zero = 0x00;
        st7701WriteCommand(0x36);
        st7701WriteData(&zero, 1);
    }

    const uint32_t initMs = millis() - t0;
    // Touch cannot be addressed above 400 kHz, so drop back before it is used.
    Wire.setClock(400000UL);
    Serial.printf("[PANEL] init table clocked out in %u ms\n", (unsigned)initMs);

    // Field-by-field assignment rather than a designated initialiser: the
    // struct layout differs between Arduino core 2.x and 3.x.
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz           = m.pclkHz;
    cfg.timings.h_res             = PANEL_W;
    cfg.timings.v_res             = PANEL_H;
    cfg.timings.hsync_pulse_width = m.hSyncPulse;
    cfg.timings.hsync_back_porch  = m.hBackPorch;
    cfg.timings.hsync_front_porch = m.hFrontPorch;
    cfg.timings.vsync_pulse_width = m.vSyncPulse;
    cfg.timings.vsync_back_porch  = m.vBackPorch;
    cfg.timings.vsync_front_porch = m.vFrontPorch;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.data_width        = 16;
    cfg.psram_trans_align = 64;
    cfg.hsync_gpio_num    = PIN_HSYNC;
    cfg.vsync_gpio_num    = PIN_VSYNC;
    cfg.de_gpio_num       = PIN_DE;
    cfg.pclk_gpio_num     = PIN_PCLK;
    cfg.disp_gpio_num     = GPIO_NUM_NC;
    memcpy(cfg.data_gpio_nums, m.dataMap, sizeof(cfg.data_gpio_nums));
    cfg.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
    if (err != ESP_OK) {
        Serial.printf("[PANEL] esp_lcd_new_rgb_panel failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = esp_lcd_panel_init(g_panel);
    if (err != ESP_OK) {
        Serial.printf("[PANEL] esp_lcd_panel_init failed: %s\n", esp_err_to_name(err));
        return false;
    }
    Serial.println("[PANEL] RGB panel started");
    return true;
}

static void backlightOn() {
    // Reset the pulse-counted dimmer, then take the rising edge to full scale.
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, LOW);
    delay(4);
    digitalWrite(PIN_BL, HIGH);
    delayMicroseconds(30);
}

// Measures the numbers the 480x480 UI has to be designed around. This IDF
// exposes only esp_lcd_new_rgb_panel: there is no bounce buffer, and no
// get_frame_buffer, so every pixel reaches the panel through a PSRAM-to-PSRAM
// copy inside draw_bitmap. The ratio between a full-screen blit and a small
// one is what decides whether the UI can afford full redraws or has to track
// dirty rectangles.
static void runBenchmark() {
    Serial.println("\n[BENCH] frame budget, PSRAM canvas -> panel framebuffer");

    const uint32_t panelHz =
        kModes[g_mode].pclkHz /
        ((uint32_t)(PANEL_W + kModes[g_mode].hSyncPulse + kModes[g_mode].hBackPorch +
                    kModes[g_mode].hFrontPorch) *
         (uint32_t)(PANEL_H + kModes[g_mode].vSyncPulse + kModes[g_mode].vBackPorch +
                    kModes[g_mode].vFrontPorch));
    Serial.printf("[BENCH] panel scan-out is fixed at %u Hz by the timings\n", (unsigned)panelHz);

    struct { const char *label; int w, h, iters; } cases[] = {
        {"full screen 480x480", 480, 480, 10},
        {"half screen 480x240", 480, 240, 10},
        {"pet sprite  160x160", 160, 160, 40},
        {"stat tile    96x 48",  96,  48, 80},
    };

    for (auto &c : cases) {
        const uint32_t t0 = micros();
        for (int i = 0; i < c.iters; ++i) {
            esp_lcd_panel_draw_bitmap(g_panel, 0, 0, c.w, c.h, g_canvas);
        }
        const uint32_t us = (micros() - t0) / c.iters;
        const uint32_t px = (uint32_t)c.w * c.h;
        Serial.printf("[BENCH]   %s  %6u us  (%u px, %u.%02u MB/s, %u/s)\n",
                      c.label, (unsigned)us, (unsigned)px,
                      (unsigned)((px * 2ULL) / (us ? us : 1)),
                      (unsigned)(((px * 200ULL) / (us ? us : 1)) % 100),
                      (unsigned)(us ? 1000000UL / us : 0));
    }

    // Pure canvas write, no blit: what the drawing code itself costs.
    const uint32_t t1 = micros();
    for (int i = 0; i < 10; ++i) fillRect(0, 0, PANEL_W, PANEL_H, 0x1234);
    Serial.printf("[BENCH]   canvas fill 480x480  %6u us (draw cost, no blit)\n",
                  (unsigned)((micros() - t1) / 10));
}

// ── Entry points ───────────────────────────────────────────────────────────

static void printMenu();

void setup() {
    Serial.begin(115200);
    // Bounded wait. An unbounded flush here is what once turned a boot into a
    // 443-second wait on the USB host, and LilyGo warn about the same trap.
    const uint32_t deadline = millis() + 5000;
    while (!Serial && millis() < deadline) delay(10);
    delay(200);

    if (g_rtcMagic != kRtcMagic) {
        g_rtcMagic = kRtcMagic;
        g_mode     = 0;
        g_attempts = 0;
    }
    if (g_mode >= kModeCount) g_mode = 0;

    // If a mode has been entered repeatedly without ever reaching the end of
    // setup(), it is hanging or panicking. Step past it instead of looping.
    if (g_attempts >= 2) {
        Serial.printf("\n[MODE] '%s' failed to complete %u times, skipping\n",
                      kModes[g_mode].name, (unsigned)g_attempts);
        g_mode     = (uint8_t)((g_mode + 1) % kModeCount);
        g_attempts = 0;
    }
    g_attempts++;

    Serial.println("\n=====================================================");
    Serial.println(" HexHound - LilyGo T-RGB bring-up");
    Serial.println("=====================================================");
    Serial.printf("Chip      : %s rev %d, %d core(s) @ %u MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned)getCpuFrequencyMhz());
    Serial.printf("Flash     : %u bytes\n", (unsigned)ESP.getFlashChipSize());
    Serial.printf("PSRAM     : %u bytes total, %u free\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("Heap      : %u free\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("MODE %d/%d : %s  (pclk %u Hz)\n",
                  g_mode + 1, kModeCount, kModes[g_mode].name,
                  (unsigned)kModes[g_mode].pclkHz);
    Serial.println("-----------------------------------------------------");

    if (ESP.getPsramSize() == 0) {
        Serial.println("[FATAL] no PSRAM. The 480x480 framebuffer cannot be allocated.");
        Serial.println("        Check board_build.arduino.memory_type = qio_opi.");
        return;
    }

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000UL);
    scanI2C();

    if (!xl9535::begin()) {
        Serial.println("[FATAL] no XL9535 expander on 0x20..0x27. Nothing else can proceed.");
        return;
    }
    Serial.printf("[XL9535] found at 0x%02X\n", xl9535::address());

    for (uint8_t pin : {XIO_POWER_ENABLE, XIO_LCD_CS, XIO_LCD_MOSI,
                        XIO_LCD_SCLK, XIO_LCD_RST, XIO_TP_RESET, XIO_SDMMC_CS}) {
        xl9535::pinModeOutput(pin);
    }
    // Idle high, and critically: power_enable HIGH or the board is dead on battery.
    xl9535::digitalWriteX(XIO_SDMMC_CS, true);
    xl9535::digitalWriteX(XIO_LCD_CS, true);
    xl9535::digitalWriteX(XIO_POWER_ENABLE, true);
    Serial.println("[XL9535] power_enable (IO2) driven HIGH");

    // Release the touch controller from reset before identifying it. The INT
    // line (GPIO1, distinct from the expander's IO1 reset despite both being
    // "1" in LilyGo's headers) is sampled by FocalTech parts during reset, so
    // leave it floating as an input. These parts also need roughly 300 ms
    // after reset release before their registers read back anything but zero.
    pinMode(1, INPUT);
    xl9535::digitalWriteX(XIO_TP_RESET, false);
    delay(30);
    xl9535::digitalWriteX(XIO_TP_RESET, true);
    delay(320);
    identifyTouch();

    g_canvas = (uint16_t *)heap_caps_malloc((size_t)PANEL_W * PANEL_H * 2, MALLOC_CAP_SPIRAM);
    if (!g_canvas) {
        Serial.println("[FATAL] could not allocate the 460800-byte canvas in PSRAM.");
        return;
    }

    if (!startPanel(kModes[g_mode])) {
        Serial.println("[PANEL] start failed; press any key to try the next mode.");
        return;
    }

    drawTestPattern(g_mode);
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, PANEL_W, PANEL_H, g_canvas);
    backlightOn();

    // Boot sweep. Flat fields are the only honest test for a channel fault or
    // for banding, and requiring a keypress to reach them meant they went
    // untested. Showing them unprompted costs 7 seconds and needs no terminal.
    struct { const char *label; uint16_t colour; uint16_t ms; } sweep[] = {
        {"WHITE  - must be NEUTRAL. A blue or cyan cast means red is attenuated.", rgb565(255, 255, 255), 2500},
        {"RED    - must be full, saturated red, as bright as the green below.",    rgb565(255, 0, 0),     1500},
        {"GREEN  - reference for brightness.",                                     rgb565(0, 255, 0),     1500},
        {"BLUE   - reference for brightness.",                                     rgb565(0, 0, 255),     1500},
    };
    Serial.println("\n[SWEEP] flat fields, watch the panel:");
    for (auto &s : sweep) {
        Serial.printf("[SWEEP]   %s\n", s.label);
        drawFlatField(s.colour);
        esp_lcd_panel_draw_bitmap(g_panel, 0, 0, PANEL_W, PANEL_H, g_canvas);
        delay(s.ms);
    }
    drawTestPattern(g_mode);
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, PANEL_W, PANEL_H, g_canvas);
    Serial.println("[SWEEP] done, back to the composite pattern.");

    runBenchmark();

    // Reaching here means the mode did not hang, so clear the retry counter.
    g_attempts = 0;

    Serial.println("-----------------------------------------------------");
    Serial.printf("Showing MODE %d/%d: %s\n", g_mode + 1, kModeCount, kModes[g_mode].name);
    Serial.println("Expected, if this mode is correct:");
    Serial.println("  * 8 colour bars, left to right:");
    Serial.println("    white, yellow, cyan, green, magenta, red, blue, black");
    Serial.println("  * three SMOOTH ramps below them: red, green, blue");
    Serial.println("  * corners: red TL, green TR, blue BL, white BR");
    Serial.printf("  * %d white dot(s) low centre = this mode number\n", g_mode + 1);
    Serial.println("If red and blue are swapped, the colour order is wrong.");
    Serial.println("If the ramps are banded or noisy, the data pin map is wrong.");
    printMenu();
}

static void printMenu() {
    Serial.println();
    Serial.println("KEYS:  p pattern   o orientation   n next mode");
    Serial.println("       w white   r red   g green   b blue   k black");
}

void loop() {
    if (!Serial.available()) {
        delay(50);
        return;
    }
    const int key = Serial.read();
    while (Serial.available()) Serial.read();

    if (key == 'n') {
        g_mode     = (uint8_t)((g_mode + 1) % kModeCount);
        g_attempts = 0;
        Serial.printf("\n>>> switching to MODE %d/%d: %s -- rebooting\n",
                      g_mode + 1, kModeCount, kModes[g_mode].name);
        Serial.flush();
        delay(50);
        ESP.restart();
    }

    if (!g_panel || !g_canvas) {
        Serial.println("[!] panel not started; only 'n' (next mode) is available.");
        return;
    }

    switch (key) {
        case 'p': drawTestPattern(g_mode);              Serial.println("-> composite pattern"); break;
        case 'o': drawOrientation();                    Serial.println("-> orientation: white band = canvas TOP, red band = canvas LEFT, green arrow points UP, 1 dot top / 2 dots bottom"); break;
        case 'w': drawFlatField(rgb565(255, 255, 255)); Serial.println("-> flat WHITE (0xFFFF): should be uniform, neutral, no tint and no bands"); break;
        case 'r': drawFlatField(rgb565(255, 0, 0));     Serial.println("-> flat RED (0xF800)");   break;
        case 'g': drawFlatField(rgb565(0, 255, 0));     Serial.println("-> flat GREEN (0x07E0)"); break;
        case 'b': drawFlatField(rgb565(0, 0, 255));     Serial.println("-> flat BLUE (0x001F)");  break;
        case 'k': drawFlatField(rgb565(0, 0, 0));       Serial.println("-> flat BLACK (0x0000)"); break;
        default:  printMenu();                          return;
    }
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, PANEL_W, PANEL_H, g_canvas);
}

#endif  // HEXHOUND_TRGB_BRINGUP
