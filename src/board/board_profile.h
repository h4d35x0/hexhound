#pragma once

#include <stdint.h>

// ── HexHound - Board Profiles ───────────────────────────────────
//
// Centralizes per-board GPIO assignments, display geometry, and LED wiring.
// Add new hardware here so the rest of the firmware stays board-agnostic.

#if defined(HEXHOUND_BOARD_LILYGO_T_DONGLE_C5)

#define HEXHOUND_BOARD_LILYGO_T_DONGLE   1

#define HEXHOUND_BOARD_NAME            "LilyGo T-Dongle C5"
#define HEXHOUND_HAS_SPI_SD            0
#define HEXHOUND_HAS_TOUCH             0
#define HEXHOUND_HAS_BATTERY           0

#define PIN_BUTTON                     28

#define PIN_LED_DATA                   5
#define PIN_LED_CLK                    4
#define HEXHOUND_LED_IS_APA102         1

// The C5 board routes SD and LCD over the same SPI pins. This firmware path
// keeps persistence on SPIFFS so SD cannot contend with the active LCD bus.
#define PIN_SD_CS                      23
#define PIN_SD_MOSI                    2
#define PIN_SD_MISO                    7
#define PIN_SD_SCK                     6

#define PIN_TFT_CS                     10
#define PIN_TFT_SCLK                   6
#define PIN_TFT_MOSI                   2
#define PIN_TFT_DC                     3
#define PIN_TFT_RST                    1
#define PIN_TFT_BL                     0

#define SCREEN_W                       160
#define SCREEN_H                       80

#elif defined(HEXHOUND_BOARD_LILYGO_T_DISPLAY_S3)

#define HEXHOUND_BOARD_NAME            "LilyGo T-Display S3"
#define HEXHOUND_HAS_SPI_SD            0
#define HEXHOUND_HAS_TOUCH             0
#define HEXHOUND_HAS_BATTERY           1

#define PIN_BUTTON                     0
#define PIN_BUTTON_2                   14

#define PIN_LED_DATA                   -1
#define PIN_LED_CLK                    -1

#define PIN_SD_CS                      -1
#define PIN_SD_MOSI                    13
#define PIN_SD_MISO                    12
#define PIN_SD_SCK                     11

#define PIN_TFT_CS                     6
#define PIN_TFT_DC                     7
#define PIN_TFT_RST                    5
#define PIN_TFT_WR                     8
#define PIN_TFT_RD                     9
#define PIN_TFT_D0                     39
#define PIN_TFT_D1                     40
#define PIN_TFT_D2                     41
#define PIN_TFT_D3                     42
#define PIN_TFT_D4                     45
#define PIN_TFT_D5                     46
#define PIN_TFT_D6                     47
#define PIN_TFT_D7                     48
#define PIN_TFT_BL                     38
#define PIN_TFT_POWER                  15

#define PIN_BATTERY_ADC                4
// LilyGo wires this one as two equal resistors, so the cell is twice the pin.
#define BATTERY_ADC_MULT               2

#define SCREEN_W                       320
#define SCREEN_H                       170

#elif defined(HEXHOUND_BOARD_WAVESHARE_147B)

#define HEXHOUND_BOARD_NAME            "Waveshare ESP32-S3-LCD-1.47B"
#define HEXHOUND_HAS_SPI_SD            1
#define HEXHOUND_HAS_TOUCH             0
#define HEXHOUND_HAS_BATTERY           0

#define PIN_BUTTON                     0

#define PIN_LED_DATA                   38
#define PIN_LED_CLK                    -1
#define HEXHOUND_LED_IS_WS2812         1

#define PIN_SD_CS                      21
#define PIN_SD_MOSI                    15
#define PIN_SD_MISO                    16
#define PIN_SD_SCK                     14

#define PIN_TFT_CS                     42
#define PIN_TFT_SCLK                   40
#define PIN_TFT_MOSI                   45
#define PIN_TFT_DC                     41
#define PIN_TFT_RST                    39
#define PIN_TFT_BL                     46

#define SCREEN_W                       320
#define SCREEN_H                       172

#elif defined(HEXHOUND_BOARD_WAVESHARE_TOUCH_147)

#define HEXHOUND_BOARD_NAME            "Waveshare ESP32-S3-Touch-LCD-1.47"
#define HEXHOUND_HAS_SPI_SD            0
#define HEXHOUND_HAS_TOUCH             1
#define HEXHOUND_HAS_BATTERY           1

// Battery, one button, no power switch: this board ran until the cell was flat.
// Same capability and same seam as the round 1.28, but NOT the same
// implementation. This board's panel reset is GPIO40, OUTSIDE the S3's RTC
// range, while the 1.28's is GPIO12 inside it, so the two release their pad
// holds through different registers. src/power/soft_sleep_tft.h carries both
// and the pin table that separates them.
#define HEXHOUND_HAS_SOFT_SLEEP        1

#define PIN_BUTTON                     0

#define PIN_LED_DATA                   -1
#define PIN_LED_CLK                    -1

#define PIN_SD_CS                      -1
#define PIN_SD_MOSI                    -1
#define PIN_SD_MISO                    -1
#define PIN_SD_SCK                     -1

#define PIN_TFT_CS                     21
#define PIN_TFT_SCLK                   38
#define PIN_TFT_MOSI                   39
#define PIN_TFT_DC                     45
#define PIN_TFT_RST                    40
#define PIN_TFT_BL                     46

#define PIN_TOUCH_SDA                  42
#define PIN_TOUCH_SCL                  41
#define PIN_TOUCH_RST                  47
#define PIN_TOUCH_INT                  48

#define PIN_BATTERY_ADC                12
// Waveshare's documented divider for this board. Stated explicitly rather than
// inherited: see the BATTERY_ADC_MULT note in the defaults section.
#define BATTERY_ADC_MULT               3

#define SCREEN_W                       320
#define SCREEN_H                       172

#elif defined(HEXHOUND_BOARD_WAVESHARE_LCD_128)

// Waveshare ESP32-S3-LCD-1.28: ESP32-S3R2 (16 MB flash, 2 MB QUAD PSRAM),
// GC9A01A 240x240 round IPS on 4-wire SPI, QMI8658 6-axis IMU on I2C.
//
// This is the NON-touch variant. Its sibling ESP32-S3-Touch-LCD-1.28 shares the
// GC9A01 but wires LCD RST to GPIO14 and BL to GPIO2 and adds a CST816S touch
// panel - do not copy pins between the two.
//
// The USB-C port is a CH343P USB-to-UART bridge, not a native USB device port,
// so there is no USB HID capability here and Serial runs on UART0 (43/44).

#define HEXHOUND_BOARD_NAME            "Waveshare ESP32-S3-LCD-1.28"
#define HEXHOUND_HAS_SPI_SD            0
#define HEXHOUND_HAS_TOUCH             0
#define HEXHOUND_HAS_BATTERY           1
#define HEXHOUND_HAS_IMU               1
#define HEXHOUND_HAS_USB_HID           0

// Battery, one button, no power switch: this board ran until the cell was flat.
// There is no touch here, so the entry gesture is a CONFIG row committed with a
// BUTTON hold - see UIConfig::onLongPress(), which handleButton() reaches on
// every board, touch or not. Implementation and wake source are in
// src/power/soft_sleep_tft.h; its panel reset is GPIO12, an RTC pad, which is
// where it parts company with the Touch 1.47.
#define HEXHOUND_HAS_SOFT_SLEEP        1

// Round panel: the glass is a circle inscribed in the 240x240 framebuffer, so
// every corner pixel is behind the bezel. UI code selects the round layout
// family on this macro (see src/ui/ui_round.h).
#define HEXHOUND_PANEL_ROUND           1

#define PIN_BUTTON                     0

#define PIN_LED_DATA                   -1
#define PIN_LED_CLK                    -1

#define PIN_SD_CS                      -1
#define PIN_SD_MOSI                    -1
#define PIN_SD_MISO                    -1
#define PIN_SD_SCK                     -1

#define PIN_TFT_CS                     9
#define PIN_TFT_SCLK                   10
#define PIN_TFT_MOSI                   11
#define PIN_TFT_DC                     8
#define PIN_TFT_RST                    12
#define PIN_TFT_BL                     40

#define PIN_IMU_SDA                    6
#define PIN_IMU_SCL                    7
#define PIN_IMU_INT1                   47
#define PIN_IMU_INT2                   48

#define PIN_BATTERY_ADC                1
// Waveshare's documented divider for this board. Stated explicitly rather than
// inherited: see the BATTERY_ADC_MULT note in the defaults section.
#define BATTERY_ADC_MULT               3

#define SCREEN_W                       240
#define SCREEN_H                       240

#elif defined(HEXHOUND_BOARD_LILYGO_T_RGB)

// LilyGo T-RGB: ESP32-S3R8 (16 MB flash, 8 MB OCTAL PSRAM), ST7701S 480x480
// round panel, FT3267 capacitive touch. Sold as "2.1 inch half circle",
// marking H583, but the active area is a complete circle with nothing clipped
// by a chord, so it is treated as a full round panel.
//
// This board does NOT drive its panel over SPI. The ST7701S runs on the S3's
// RGB parallel LCD peripheral with a framebuffer in PSRAM, and the panel's own
// SPI init lines (CS/MOSI/SCLK/RST) are pins on an XL9535 I2C expander rather
// than GPIOs. There are therefore no PIN_TFT_* SPI pins to declare; see
// src/hal/rgb_panel_trgb.h for the real wiring.
//
// Unlike the Waveshare 1.28 round board, the USB-C port here is the S3's
// native USB, so USB HID missions are available.

#define HEXHOUND_BOARD_NAME            "LilyGo T-RGB"
#define HEXHOUND_HAS_SPI_SD            0
#define HEXHOUND_HAS_TOUCH             1
#define HEXHOUND_HAS_BATTERY           1
#define HEXHOUND_HAS_IMU               0
#define HEXHOUND_HAS_USB_HID           1

// This board has a battery, no power switch and no second button, so until now
// there was no way to stop it short of letting the cell go flat. See the soft
// sleep block at the bottom of this file for why it is a capability macro of
// its own rather than a reuse of HEXHOUND_HAS_BUTTON_B.
#define HEXHOUND_HAS_SOFT_SLEEP        1

#define HEXHOUND_PANEL_ROUND           1

#define PIN_BUTTON                     0

#define PIN_LED_DATA                   -1
#define PIN_LED_CLK                    -1

// SD is SDMMC (SCK 39 / CMD 40 / DAT 38), not SPI, and is out of scope until
// a phase adds an SDMMC path. Declared absent rather than mis-declared as SPI.
#define PIN_SD_CS                      -1
#define PIN_SD_MOSI                    -1
#define PIN_SD_MISO                    -1
#define PIN_SD_SCK                     -1

// Panel control lives on the RGB peripheral and the I2C expander, not on a
// 4-wire SPI bus. -1 keeps the shared declarations well-formed.
#define PIN_TFT_CS                     -1
#define PIN_TFT_SCLK                   -1
#define PIN_TFT_MOSI                   -1
#define PIN_TFT_DC                     -1
#define PIN_TFT_RST                    -1
// Real GPIO, but NOT a plain on/off backlight: it is a pulse-counted 16-step
// dimmer. Held LOW it resets; a LOW->HIGH edge selects full brightness and
// each further LOW/HIGH pulse steps it down one notch.
#define PIN_TFT_BL                     46
#define HEXHOUND_BL_PULSE_DIMMER       1

// FT3267 capacitive touch, sharing the panel's I2C bus at 0x38.
// PIN_TOUCH_RST is -1 on purpose: the controller's reset line is expander pin
// IO1, not a GPIO, and it is released inside trgb::begin() so its ~300 ms
// settle overlaps the panel init instead of adding to boot time.
#define PIN_TOUCH_SDA                  8
#define PIN_TOUCH_SCL                  48
#define PIN_TOUCH_INT                  1
#define PIN_TOUCH_RST                  -1

// GPIO4 (ADC1_CH3). LilyGo read it with analogRead() and double the result,
// so the hardware divider is /2. Stated explicitly: a board with a battery
// must declare its divider or fail to compile.
#define PIN_BATTERY_ADC                4
#define BATTERY_ADC_MULT               2

#define SCREEN_W                       480
#define SCREEN_H                       480

#else

#define HEXHOUND_BOARD_TDONGLE_S3      1
#define HEXHOUND_BOARD_LILYGO_T_DONGLE 1
#define HEXHOUND_BOARD_NAME            "LilyGo T-Dongle S3"
#define HEXHOUND_HAS_SPI_SD            1
#define HEXHOUND_HAS_TOUCH             0
#define HEXHOUND_HAS_BATTERY           0

#define PIN_BUTTON                     0

#define PIN_LED_DATA                   40
#define PIN_LED_CLK                    39
#define HEXHOUND_LED_IS_APA102         1

#define PIN_SD_CS                      10
#define PIN_SD_MOSI                    11
#define PIN_SD_MISO                    13
#define PIN_SD_SCK                     12

#define PIN_TFT_CS                     4
#define PIN_TFT_SCLK                   5
#define PIN_TFT_MOSI                   3
#define PIN_TFT_DC                     2
#define PIN_TFT_RST                    1
#define PIN_TFT_BL                     38

#define SCREEN_W                       160
#define SCREEN_H                       80

#endif

// ── Feature defaults ──────────────────────────────────────────────────────
// Defaulted here so adding a capability macro never requires touching the
// board branches that predate it, and so `#if HEXHOUND_*` is always
// well-formed regardless of which board is selected.

#ifndef HEXHOUND_HAS_IMU
#define HEXHOUND_HAS_IMU               0
#endif

// Every board except the round Waveshare exposes a native USB device port and
// can therefore run the USB HID missions subsystem.
#ifndef HEXHOUND_HAS_USB_HID
#define HEXHOUND_HAS_USB_HID           1
#endif

// Rectangular panel unless a board opts into the round layout family.
#ifndef HEXHOUND_PANEL_ROUND
#define HEXHOUND_PANEL_ROUND           0
#endif

// ── Battery ADC divider ───────────────────────────────────────────────────
// The battery pin sits behind a resistor divider and the ratio is a property of
// the BOARD, not of the firmware. It used to be a hardcoded `* 3` in
// battery_module.cpp carrying the comment "Waveshare's example multiplies the
// ADC reading by 3", which was true for the Waveshares and simply wrong
// everywhere else. On the T-Display S3 (LilyGo, two equal resistors, so x2) it
// over-reported by 50%, and because percent() then clamps, the device reported a
// FULL battery at every state of charge:
//
//     pct = (6807 - 3400) * 100 / (4200 - 3400) = 425  ->  clamped to 100
//
// So the gauge could never warn about a flat cell, and roam's
// ROAM_BATTERY_FLOOR_PCT guard could never fire either.
//
// THERE IS DELIBERATELY NO DEFAULT FOR A BOARD THAT HAS A BATTERY. The original
// bug was not a wrong number typed once, it was a board INHERITING another
// board's divider silently, and a default would leave that trap armed for the
// next board added. Declaring it is now mandatory, and getting it wrong is a
// compile error rather than a gauge that reads 100% forever.
#if HEXHOUND_HAS_BATTERY && !defined(BATTERY_ADC_MULT)
#error "This board sets HEXHOUND_HAS_BATTERY but no BATTERY_ADC_MULT. The ADC \
divider ratio is board-specific and must not be inherited: a LilyGo T-Display S3 \
is x2 while the Waveshares are x3, and guessing produced a gauge that reported a \
full battery at every state of charge. Measure or look it up, then state it in \
this board's block."
#endif

// Boards with no battery never read the pin, so the value is irrelevant to them;
// 1 is chosen so that if one ever does start reading it, the number is obviously
// unscaled rather than plausibly wrong.
#ifndef BATTERY_ADC_MULT
#define BATTERY_ADC_MULT               1
#endif

// ── Button B ──────────────────────────────────────────────────────────────
// Derived rather than declared per board: a board has a Back button exactly
// when it defines a usable PIN_BUTTON_2. Today only the T-Display S3 does.
//
// Everything else stays a one-button device, where short = next/scroll and long
// = select/open, and none of the two-button code is compiled at all. That split
// is deliberate: the six single-button boards are shipping and must not change
// behaviour because a seventh grew a button.
#if defined(PIN_BUTTON_2) && (PIN_BUTTON_2 >= 0)
#define HEXHOUND_HAS_BUTTON_B          1
#else
#define HEXHOUND_HAS_BUTTON_B          0
#endif

// ── Soft sleep ────────────────────────────────────────────────────────────
// A board can be stopped by the owner, from the UI, without a power switch.
//
// This is DELIBERATELY NOT derived from HEXHOUND_HAS_BUTTON_B, even though the
// only soft power off in the tree today lives behind that macro. Two separate
// things were tangled together there:
//
//   * the GESTURE (hold both buttons), which needs a second button, and
//   * the CAPABILITY (this board can be told to stop), which does not.
//
// Deriving one from the other is why three battery-powered boards - the T-RGB,
// the Waveshare 1.28 round and the Waveshare Touch 1.47 - have no way to stop
// at all: they each have a battery and a single button, so the whole sleep path
// compiled out with the gesture that happened not to fit them.
//
// Declared per board rather than derived, because "can this board be stopped
// safely" is a question about its POWER hardware, not about its buttons. Every
// board that sets this must also have a wake source that is an RTC-capable pad
// (GPIO0..GPIO21 on the ESP32-S3) and a way to make the panel visibly dark, or
// sleep is indistinguishable from a crash. src/power/soft_sleep.h is the seam
// where a board's implementation is bound, and it is a hard #error there if a
// board sets this without providing one.
//
// Defaulted to 0 here so no existing board branch had to be edited, and so the
// six shipping boards compile not one instruction of this.
#ifndef HEXHOUND_HAS_SOFT_SLEEP
#define HEXHOUND_HAS_SOFT_SLEEP        0
#endif
