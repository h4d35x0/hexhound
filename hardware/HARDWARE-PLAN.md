# HexHound Hardware Plan - the cyber-Tamagotchi

Direction set 2026-07-21. Read `HARDWARE-PLAYBOOK.md` in this folder first - it holds the
reusable process/lessons; this file is HexHound-specific.

## The product

HexHound becomes a **cyber-Tamagotchi**: a small, charming, keychain pet that lives inside a round
color "portal" and reacts to the RF world around it (passive WiFi/BLE scan, count-never-attack). It
should read as a *toy*, not a hacking tool - that is both its charm and its OPSEC. The pet is the
star; the recon is the ambient soul underneath.

## Prototype boards being ordered (2026-07-21)

Two round S3 boards, deliberately different size classes / display buses:
- **LILYGO T-RGB 2.1" (getting the HALF-CIRCLE [H583] variant), ~$30.** ESP32-S3R8, 8MB PSRAM, 16MB
  flash, WiFi b/g/n + BT5. Display = **2.1" round IPS, 480x480, 18-bit color, ST7701S**. On-board LiPo
  support + microSD + Grove I2C. Touch: **FT3267** (half circle) / CST820 (full circle), I2C cap.
  - **CRITICAL: the ST7701S here is an RGB-PARALLEL panel, NOT an SPI display.** SPI for init only,
    then 16-bit RGB pixel bus through the S3 LCD peripheral into a PSRAM framebuffer. It does NOT
    drive with TFT_eSPI. Use **Arduino_GFX (moononournation) / LovyanGFX / esp_lcd RGB** - start from
    LILYGO's own T-RGB library + examples for the display adapter.
  - RGB bus eats ~22 GPIO, so on the dev board drive inputs off Grove I2C (touch + accel), the boot
    button, and touch itself. A future custom PCB must budget pins around the RGB bus.
  - Confirm the half-circle's exact active area + FT3267 wiring from LILYGO's repo when it arrives.
  - **Role for HexHound: the premium SHOWPIECE.** 480x480 makes the HD mascot look like a product; the
    half-circle is a distinctive form. But 2.1" (~53mm) is handheld/desk size, not keychain.
- **Waveshare round ESP32-S3 1.28" (GC9A01, 240x240, SPI).** Keychain-sized (~32mm). Drives with
  TFT_eSPI/LovyanGFX (SPI) = lower-friction port. **Role for HexHound: the KEYCHAIN form validation.**

Plan: prototype the LOOK on the T-RGB (480x480 does the HD art justice), validate the KEYCHAIN size on
the 1.28". Decide the production display after seeing both in hand.

## Display + art (decided)

- **A round color IPS, 480x480 (T-RGB) or 240x240 (GC9A01) depending on final size.** A glowing
  creature inside a little round window is the most Tamagotchi/pet-like display possible; the round
  frame IS the aesthetic. Higher res (480x480) is better for the gradient HD art.
- **Use the HD art set, NOT the pixel sprites.** Source: the sprite generator in a sibling firmware
  project - `hd_egg / hd_pup / hd_beast / hd_gremlin / hd_sentinel` (240x240) plus the
  200x200 `asset_*` transparent versions. They are a cohesive premium mascot line (the circuit-crack
  egg, cyan-eyed pup, antenna'd beast, green-fanged gremlin, blue-visor sentinel). Owner confirmed the
  art can be freely up/downscaled, so the display resolution does not constrain the art.
  - The egg's glowing blue circuit-trace crack ties the mascot to the PCB/hardware theme - lean into it.
  - Pixel sprites read "hobby"; the HD art reads "product." That difference is the whole pitch.
- **Firmware art change:** HexHound today renders RGB565 pixel sprites (`src/ui/sprites.h`). Switch to
  blitting full bitmaps: convert the HD PNGs to RGB565 (or a compressed on-flash format), store in
  flash/LittleFS, blit per stage. This is SIMPLER than a pixel-sprite engine, not harder. Keep
  animation cheap: bob / blink / glow-pulse overlays rather than full extra frames (5 static 240x240
  RGB565 frames is ~575 KB - trivial on a 4-16 MB S3 module; full-frame animation would balloon it).

## Antenna (the clever bit)

- Default: a good **internal antenna** is enough for a pet - it reacts to the RF *vibe* nearby and
  does not need survey range.
- If you want more reach, **disguise an external antenna as EARS or a TAIL** so it becomes part of the
  mascot instead of blowing the toy disguise. A pet with floppy ears that literally "hears" better is
  a feature, not a compromise. Route it to a u.FL on the board so the ear/tail antenna is an enclosure
  choice, not a board respin.

## MCU

- **ESP32-S3.** HexHound's firmware is S3-native (validated on a LilyGo T-Dongle S3). S3 gives PSRAM,
  USB-C flashing, and headroom. The Tamagotchi look comes from the round screen + egg shell, not the
  chip, and the chip is swamped by screen+battery in the size budget - so there is no size win in
  dropping to a C3, only a firmware port cost. Stay S3.

## Prototype-first path (per playbook meta-lesson 1)

Owner is ordering a **round ESP32-S3 240x240 dev board** (e.g. Waveshare ESP32-S3-LCD-1.28 round, or a
LILYGO round module), arriving ~1-2 weeks. Plan of record:
1. On the off-the-shelf round board: port HexHound's UI to 240x240 round, swap in the HD art, wire up
   passive WiFi/BLE scan + LIS3DH gestures. Prove the pet is charming and the scanning works, in hand.
2. Print an egg/pebble shell with a keychain clip around the dev board.
3. ONLY THEN design a custom PCB (S3 + GC9A01 + LiPo + charger + LIS3DH + button + haptic + u.FL for
   the ear/tail antenna) to shrink it into a finished, sellable object.

## Port status (updated 2026-07-30)

The **Waveshare ESP32-S3-LCD-1.28 (non-touch)** arrived and is now a first-class
firmware target: `pio run -e waveshare-esp32-s3-lcd-128`. Round support was added
*additively* - a third layout family selected by `HEXHOUND_PANEL_ROUND` - so all
six pre-existing boards build and behave exactly as before.

Done:
- [x] Board profile, board JSON (ESP32-S3R2: 2 MB **quad** PSRAM, so `qio_qspi`),
      and PlatformIO envs (firmware + demo + ble-test + two simulator envs).
- [x] Re-authored the UI for the 240x240 round frame: `src/ui/ui_round.{h,cpp}`
      provides chord-aware primitives (every horizontal extent derives from the
      chord at its row, not from SCREEN_W) plus rim arc gauges. Home, menu,
      stats, patrol results, patrol HUD, evolve, journal, alert and config all
      have round layouts.
- [x] HD art at round scale: 88px home portrait / 160px hero
      (`sprites_hd_round.h`, 326 KB of flash).
- [x] Simulator renders the circular bezel mask, so the desktop sim cannot show
      layout the real glass hides.
- [x] IMU input: QMI8658 driver (`src/modules/imu_module.*`) publishing
      shake/tilt onto the event bus, mapped onto the existing button actions.
      This is the answer to "the only button is BOOT and there is no touch panel"
      on this variant.
- [x] USB HID missions gated off via `HEXHOUND_HAS_USB_HID` - the USB-C port is
      a CH343P UART bridge, not a native USB device port, so HID cannot work.

**HARDWARE VALIDATION PASSED 2026-07-30**, first flash, over COM25. Boot in
3131 ms with `SETUP COMPLETE - ALL OK`: panel lit, `tft->init()` clean (no
StoreProhibited), SPIFFS up, SD correctly reported absent, WiFi + BLE both up,
and `[IMU] QMI8658 ready at 0x6B` on the first try. So RST=12, BL=40,
`qio_qspi` and `ARDUINO_USB_CDC_ON_BOOT=0` are all confirmed on real hardware,
not just on paper.

`USE_HSPI_PORT` was NOT required here, though the PTR project found it mandatory
on the same board. The difference is the Arduino core: PTR is on 3.x, HexHound
is on espressif32@6.12.0 (core 2.x) and carries its own `REG_SPI_BASE` patch
(`scripts/patch_tft_espi.py`), which fixes the same crash. Do not "fix" one
project by copying the other's flag.

Not done / still open:
- [ ] Confirm `TFT_INVERSION_ON` / `TFT_RGB_ORDER` for this GC9A01 lot (colours
      looked right on first boot; revisit if a later panel lot differs).
- [ ] Replace the RGB565 pixel-sprite path with HD-bitmap blits everywhere
      (the HD set is in use for the resting pet and hero shots; the motion
      frames are still pixel sprites).
- [ ] Cap + age the contact store for dense-RF memory safety.
- [ ] Apply the coex fix (`WIFI_PS_MIN_MODEM`) and a time-shared WiFi/BLE scan.

Note the two 1.28-inch Waveshare boards are NOT interchangeable: the non-touch
**ESP32-S3-LCD-1.28** wires LCD RST=12 / BL=40 and has no touch controller; the
**ESP32-S3-Touch-LCD-1.28** wires RST=14 / BL=2 and adds a CST816S. The "is it 12
or 14?" confusion in the upstream TFT_eSPI threads is two different products.

## Firmware port checklist (keep the brain, rebuild the body)

Reusable as-is (the ~2-3k lines of HAL-isolated logic): `pet/pet_core`, `pet/pet_rules`,
`events/event_bus`, `modules/storage_module`, and the passive-scan LOGIC. Its RX-only ethic already
matches the platform invariant - zero rework there.

To do:
- [ ] Re-author the UI for a 240x240 SQUARE/round frame (current targets are 80x160 / 320x170 landscape).
- [ ] Replace the RGB565 pixel-sprite path with HD-bitmap blits (convert `hexhound_out` PNGs).
- [ ] Add a GC9A01 display adapter (round) under the HAL.
- [ ] Add a LIS3DH adapter + shake/tilt gesture inputs (HexHound has never used an accel - new, but
      perfect for a pet: shake to play, tilt to look around).
- [ ] DROP the USB-HID "missions" subsystem (`usb_module`, `ui_missions`). It needs full USB-OTG HID
      and there is no host to type into on a keychain pet. Cut it.
- [ ] Apply the coex fix (`WIFI_PS_MIN_MODEM`) and a time-shared WiFi/BLE scan.
- [ ] Cap + age the contact store for dense-RF memory safety.

## Wow factor / positioning

The recon-pet genre (Pwnagotchi, Bjorn) is a real cult hit. HexHound's edge: it is a *premium,
color, evolving* pet in a charming toy shell that happens to be a passive RF-awareness companion -
and it is ethically clean (count, never attack), which sets it apart from the attacker-pets. The HD
mascot line is the differentiator that stops it reading as "another Pwnagotchi."
