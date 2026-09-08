#pragma once

// ── HexHound - T-Dongle S3 Pin Reference ─────────────────────────
// Consolidates all pin assignments and hardware verification notes.
// Reference: LilyGo T-Dongle S3 v1.1 schematic
//
// ESP32-S3 Module: ESP32-S3-FN4R2 (4MB Flash, 2MB PSRAM)
// USB: Native USB-C (GPIO19=D-, GPIO20=D+)
// ────────────────────────────────────────────────────────────────────────────

// ── Display: ST7735S 0.96" TFT (80×160) ────────────────────────────────────
// Connected via SPI (configured in TFT_eSPI User_Setup.h)
//
// Signal      GPIO    Verified    Notes
// ──────────  ──────  ──────────  ──────────────────────────────────
// TFT_MOSI    3       YES         SPI data out
// TFT_SCLK    5       YES         SPI clock
// TFT_CS      4       YES         Chip select (active low)
// TFT_DC      2       YES         Data/command select
// TFT_RST     1       YES         Hardware reset
// TFT_BL      38      VERIFY Backlight enable (active high)
//
// TFT_eSPI User_Setup.h should define:
//   #define ST7735_DRIVER
//   #define TFT_WIDTH  80
//   #define TFT_HEIGHT 160
//   #define TFT_MOSI   3
//   #define TFT_SCLK   5
//   #define TFT_CS     4
//   #define TFT_DC     2
//   #define TFT_RST    1
//   #define SPI_FREQUENCY 27000000

// ── RGB LED: APA102 (1 LED) ─────────────────────────────────────────────────
// Signal      GPIO    Verified    Notes
// ──────────  ──────  ──────────  ──────────────────────────────────
// LED_DATA    40      YES         APA102 data (MOSI)
// LED_CLK     39      YES         APA102 clock (SCK)
//
// FastLED config: CRGB leds[1]; FastLED.addLeds<APA102, 40, 39>(leds, 1);

// ── SD Card: MicroSD slot (SPI) ─────────────────────────────────────────────
// Signal      GPIO    Verified    Notes
// ──────────  ──────  ──────────  ──────────────────────────────────
// SD_CS       10      VERIFY Chip select
// SD_MOSI     11      VERIFY SPI data out
// SD_SCK      12      VERIFY SPI clock
// SD_MISO     13      VERIFY SPI data in
//
// NOTE: SD card uses a separate SPI bus from the TFT.
// Initialize with: SD.begin(PIN_SD_CS, SPI1) or software SPI.
// Some boards use SDMMC instead of SPI - verify on hardware.

// ── Button ──────────────────────────────────────────────────────────────────
// Signal      GPIO    Verified    Notes
// ──────────  ──────  ──────────  ──────────────────────────────────
// BUTTON      0       YES         Active low, internal pull-up
//                                  Also serves as BOOT button
//                                  INPUT_PULLUP, digitalRead == LOW when pressed

// ── USB ─────────────────────────────────────────────────────────────────────
// Signal      GPIO    Verified    Notes
// ──────────  ──────  ──────────  ──────────────────────────────────
// USB_D-      19      N/A         Native USB, managed by hardware
// USB_D+      20      N/A         Native USB, managed by hardware
//
// USB modes:
//   ARDUINO_USB_MODE=1 -> CDC (Serial over USB, default)
//   ARDUINO_USB_MODE=0 -> OTG (HID keyboard possible)
// To use USBHIDKeyboard, build with USB_MODE=0 and TinyUSB.

// ── WiFi / BLE ──────────────────────────────────────────────────────────────
// No dedicated GPIO - uses ESP32-S3 internal radio with onboard antenna.
// WiFi: 2.4GHz 802.11 b/g/n
// BLE: Bluetooth 5.0 LE (NimBLE stack)

// ── Unused / Available GPIOs ────────────────────────────────────────────────
// The T-Dongle S3 exposes limited GPIOs. Most are used by the
// display, SD, LED, and USB. Check schematic for any exposed pads.
// GPIO 6-9 may be available (verify on your board revision).
// GPIO 14-18 used for PSRAM/Flash - DO NOT USE.
// GPIO 43 (TX), 44 (RX) - UART0, used by Serial if CDC not active.

// ── Pin Definition Summary (for config.h) ───────────────────────────────────
// These should match config.h exactly:
//
// #define PIN_BUTTON        0
// #define PIN_LED_DATA      40
// #define PIN_LED_CLK       39
// #define PIN_SD_CS         10
// #define PIN_SD_MOSI       11
// #define PIN_SD_MISO       13
// #define PIN_SD_SCK        12
// #define PIN_TFT_BL        38
