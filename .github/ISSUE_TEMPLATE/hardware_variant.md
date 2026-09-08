---
name: Hardware Variant Report
about: Report a different board revision or alternative hardware
title: "[HARDWARE] "
labels: hardware
assignees: ''
---

## Board Information

- **Board name:**
- **Board revision / version:**
- **Where purchased:**
- **ESP32-S3 module variant** (if known):

## Pin Differences

Compare your board's pinout against `src/diagnostics/pin_checker.h`.

| Signal | Expected GPIO | Actual GPIO | Notes |
|--------|--------------|-------------|-------|
| TFT_MOSI | 3 | | |
| TFT_SCLK | 5 | | |
| TFT_CS | 4 | | |
| TFT_DC | 2 | | |
| TFT_RST | 1 | | |
| TFT_BL | 38 | | |
| LED_DATA | 40 | | |
| LED_CLK | 39 | | |
| SD_CS | 10 | | |
| SD_MOSI | 11 | | |
| SD_SCK | 12 | | |
| SD_MISO | 13 | | |
| BUTTON | 0 | | |

## LED Type

- [ ] APA102 (2-wire: DATA + CLK) — same as reference
- [ ] WS2812 (1-wire: DATA only)
- [ ] Other:

## Display Driver

- [ ] ST7735S — same as reference
- [ ] ST7789
- [ ] Other:

## Changes Required

What did you need to change to get HexHound running on this board?

```
List modified files, changed pin values, build flag changes, etc.
```

## Working Features

Check all features that work on your board:

- [ ] Display initializes and shows boot splash
- [ ] SD card read/write
- [ ] WiFi scanning
- [ ] BLE scanning
- [ ] RGB LED (correct colors)
- [ ] Button (short + long press)
- [ ] USB HID (in OTG mode)

## Hardware Validation Output

```
Paste hw_validator results here
```

## Photos

If possible, include a photo of your board (both sides) to help identify the revision.

## Additional Notes

Any other observations about compatibility, performance differences, etc.
