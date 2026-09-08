# Bill of Materials

## Core Components

This BOM uses the LilyGo T-Dongle S3 as the reference build because it is the most fully validated hardware path today. Supported Waveshare alternatives are listed below.

| Component | Part | Source | Est. Cost |
|-----------|------|--------|-----------|
| Main board | LilyGo T-Dongle S3 (ESP32-S3) | [AliExpress](https://www.aliexpress.com/item/1005004459426498.html) / Amazon | ~$15 |
| MicroSD card | Optional. Any FAT32 formatted, 8GB+ | Any electronics retailer | ~$5 |

**Total estimated build cost: ~$15 base / ~$20 with SD card**

## Alternate Supported Boards

| Board | Notes |
|------|-------|
| Waveshare ESP32-S3-LCD-1.47B | Larger ST7789 display, battery sensing, no touch |
| Waveshare ESP32-S3-Touch-LCD-1.47 | Larger ST7789 display, capacitive touch, battery sensing |
| Waveshare ESP32-S3-LCD-1.28 | 240x240 round GC9A01A, QMI8658 IMU input, battery sensing. USB-C is a CH343P UART bridge, so no USB HID |
| LilyGo T-RGB (2.1in round) | 480x480 round ST7701S on the RGB *parallel* bus with a PSRAM framebuffer, FT3267 capacitive touch, battery sensing, native USB. Requires an ESP32-S3 with PSRAM; the validated unit is 8 MB octal PSRAM + 16 MB flash |
| LilyGo T-Display S3 | 320x170 ST7789, two buttons (the only board with a real Back), battery sensing |
| LilyGo T-Dongle C5 | ESP32-C5. Needs its own PlatformIO core directory; SD shares SPI with the LCD and is disabled |

## Optional Accessories

| Component | Part | Source | Est. Cost |
|-----------|------|--------|-----------|
| USB-A extension cable | Short (10-15cm) M-to-F | Any | ~$2 |
| Keychain lanyard | Small loop or carabiner clip | Any | ~$1 |
| Protective case | 3D printed enclosure (STL TBD) | Self-print or service | ~$3 |

## Sourcing Notes

### LilyGo T-Dongle S3

- **AliExpress** - cheapest option, ships from China. Expect 2-4 week delivery. Search for "LilyGo T-Dongle S3" or "LILYGO T-Dongle-S3 ESP32-S3."
- **Amazon** - faster delivery (1-3 days with Prime), higher price (~$18-22). Search for "LILYGO T-Dongle S3." Available from third-party sellers.
- **LilyGo official store** - [lilygo.cc](https://www.lilygo.cc/)

### Board Revision Notes

- Verify the **LED type** on arrival. Most T-Dongle S3 boards use an APA102 (2-wire SPI: DATA + CLK). Some earlier revisions may use WS2812 (1-wire). If the LED doesn't respond, check `pin_checker.h` for alternate wiring.
- Verify the **display driver**. Standard is ST7735S. Build flags in `platformio.ini` are configured for this driver.
- The T-Dongle S3 has a **USB-A male connector** built in. No cable needed for direct connection, but an extension cable helps when the dongle blocks adjacent ports.

### MicroSD Card

- **FAT32 format required.** Cards larger than 32GB may need to be formatted manually (Windows formats 64GB+ as exFAT by default).
- A basic 8GB card is plenty - HexHound stores text logs and small JSON files.
- On the current T-Dongle S3 vendor-app path, core pet persistence works from onboard SPIFFS even if no SD card is installed.
- If SD initialization fails, try a different brand. Some off-brand cards have SPI compatibility issues.

## Tools Required

- Computer with USB-A port (or USB-C to USB-A adapter)
- VS Code with PlatformIO extension installed
- MicroSD card reader (if not built into your computer)

## No Soldering Required

The T-Dongle S3 is a complete, assembled board. No soldering, no wiring, no breadboard. Flash the firmware and go. An SD card is optional for removable logs and SD-path validation.

The supported Waveshare boards are also complete assembled boards. They are good options when you want a larger screen, capacitive touch, or battery-backed use.
