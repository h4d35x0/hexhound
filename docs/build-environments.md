# HexHound Build Environments

Every PlatformIO environment in `platformio.ini`, what it is for, and the
board-specific build notes that go with it.

## Firmware targets

| Environment | Purpose |
|-------------|---------|
| `lilygo-t-dongle-s3-vendor-app` | Recommended full firmware for the LilyGo T-Dongle S3 |
| `lilygo-t-dongle-s3-vendor-baseline` | Vendor-style color-cycle board validation target |
| `t-dongle-s3-hid` | T-Dongle S3 HID build for live USB mission execution |
| `lilygo-t-dongle-c5-vendor-app` | Full firmware for the LilyGo T-Dongle C5 |
| `lilygo-t-display-s3` | Full firmware for the LilyGo T-Display S3 |
| `waveshare-esp32-s3-lcd-147b` | Full firmware for the non-touch Waveshare 1.47-inch board |
| `waveshare-esp32-s3-touch-lcd-147` | Full firmware for the capacitive-touch Waveshare 1.47-inch board |
| `waveshare-esp32-s3-lcd-128` | Full firmware for the round 240x240 Waveshare 1.28-inch board |
| `lilygo-t-rgb` | Full firmware for the round 480x480 LilyGo T-RGB |
| `lilygo-t-rgb-bringup` | Standalone T-RGB panel bring-up: colour bars, flat-field sweep, expander and touch probe |

## Simulator targets

| Environment | Purpose |
|-------------|---------|
| `desktop-sim` / `desktop-sim-linux` | 160x80 T-Dongle panel |
| `desktop-sim-wave` / `desktop-sim-wave-linux` | 320x172 Waveshare panel |
| `desktop-sim-round` / `desktop-sim-round-linux` | 240x240 round panel, circular bezel mask applied |
| `desktop-sim-round480` | 480x480 round panel (T-RGB), same round layout family |
| `native` | Unit test runner (`pio test -e native`) |

The Windows and Linux simulator envs differ only in how SDL2 is located:
`desktop-sim` uses MSYS2 paths, `desktop-sim-linux` uses `pkg-config`. Both
share one source filter and library list (`[sim_common]`).

## USB mode: CDC vs HID

The T-Dongle S3 has two USB modes and they are mutually exclusive:

| Mode | Flag | Serial Monitor | HID Keyboard | Use For |
|------|------|---------------|-------------|---------|
| **CDC** (default) | `ARDUINO_USB_MODE=1` | Yes | No | Development, debugging |
| **OTG/HID** | `ARDUINO_USB_MODE=0` | No | Yes | Mission execution on real hosts |

The recommended build, `lilygo-t-dongle-s3-vendor-app`, keeps CDC enabled and
uses the vendor-proven display path. HID missions therefore require flashing
`t-dongle-s3-hid` separately. That environment has not yet been migrated onto
the vendor display backend; treat the migration as an open task.

Flashing the HID build also removes the serial port, so reflashing back to a
CDC build needs a manual reset into download mode.

## Display backends

The original TFT_eSPI path compiled but black-screened during real app boot on
the tested T-Dongle S3. The T-Dongle targets now use LilyGo's `esp_lcd` ST7735
driver wrapped behind `src/hal/tft_compat.*`. The C5 uses the same wrapper with
C5-specific pins. T-Display S3 and the Waveshare boards use TFT_eSPI with
board-specific pin and panel definitions.

**The T-RGB uses neither path.** Its ST7701S is driven by the ESP32-S3's RGB
*parallel* LCD peripheral with a framebuffer in PSRAM, behind
`src/hal/tft_compat_rgb.cpp` and `src/hal/rgb_panel_trgb.*`. TFT_eSPI cannot
drive it at all. Two consequences worth knowing before touching that code:

- The panel's own SPI init lines are pins on an **XL9535 I2C expander**, not
  GPIOs, and `power_enable` (expander IO2) must be driven HIGH or the board is
  dead on battery. There is no command channel once `esp_lcd` owns the bus, so
  `writecommand`/`writedata`/`invertDisplay` are no-ops.
- `setRotation()` is a **no-op**: it records the value and applies nothing,
  because in the product orientation the panel's scan already lands upright and
  a software rotation would cost a full-canvas transform every frame. Code that
  assumes a rotation took effect will be wrong on this board.

Scan-out is fixed at **26 Hz** and a full redraw is about 39 ms, which is the
entire frame budget, hence the dirty-Y-range design in `tft_compat_rgb.cpp`.

Full failure analysis: [t-dongle-s3-build-report.md](t-dongle-s3-build-report.md).

On the vendor `esp_lcd` path, `fillRect` hands a shared static buffer to an
asynchronous DMA transfer. Any change that increases the rate of `fillRect`
calls is a correctness risk, not just a performance question.

## C5-specific notes

- Onboard SPIFFS is the active persistence backend.
- The C5 exposes SD hardware, but SD and LCD share the same SPI pins, so SD is
  disabled on this target until HexHound has a shared SPI bus implementation.
- APA102 LED output is disabled during bring-up so display, Wi-Fi, BLE, storage
  and button behaviour can be validated independently.
- The C5 pulls its platform from pioarduino, which publishes under the same
  package names as the stock platform. `scripts/build_flashes.py` gives it an
  isolated PlatformIO core directory so it cannot break the ESP32-S3 builds.
  Building it by hand needs `PLATFORMIO_CORE_DIR` set; see the README.
- **The platform is pinned to the `55.03.311` release zip, not a branch**
  (2026-08-03). It used to be `platform-espressif32.git#develop`, which made the
  C5 the only target in the repo tracking a moving ref, and it was the only one
  of seven that broke while the rest rebuilt unchanged. The release pin also
  pins the Arduino core: `#develop` resolved `framework-arduinoespressif32` to
  `arduino-esp32/archive/master.zip`, whereas `55.03.311` uses the 3.3.11
  release tarballs. Bump the pin deliberately; do not put a branch back.
- Do not add a script that "repairs" `toolchain-riscv32-esp`. pioarduino's
  registry entry for it is a 1643-byte idf_tools manifest, and
  `tool-esp_install` materializes the real toolchain into `<core>/tools/` and
  re-registers the package. A repair script that probes for `.exe` names worked
  on Windows, failed on every Linux CI run, and was the entire C5 build failure
  until it was deleted. See `tasks/lessons.md`.

## Regenerating pet art

HexHound has no PNG decoder and no guaranteed SD card, so every image is baked
into a flash-resident RGB565 array at build time. **RGB565 is 2 bytes per pixel,
so image size drives flash hard - bake small things small.**

Each art domain gets its own generator and its own output header, so two people
working in parallel never edit the same generated file:

| Generator | Output | Contents |
|---|---|---|
| `scripts/gen_hd_sprites.py` | `src/ui/sprites_hd_*.h` | HD pet stills, one header per panel family |
| `scripts/gen_hd_idle.py` | `src/ui/sprites_hd_idle_round480.h` | HD idle frames for the 480 panel, derived from the same masters |
| `scripts/gen_item_icons.py` | `src/ui/item_art.h` | Inventory / den item icons |
| `scripts/gen_den_art.py` | `src/ui/den_art.h` | Den room tiles |
| `scripts/gen_form_badges.py` | `src/ui/form_art.h` | Form badges |

A build carries only the sizes its panel uses:

| Header | Home / hero size | Panels |
|---|---|---|
| `sprites_hd_small.h` | 24 / 48 | 160x80 |
| `sprites_hd_big.h` | 64 / 112 | 320x172 |
| `sprites_hd_round.h` | 88 / 160 | 240x240 round |
| `sprites_hd_round480.h` | 176 / 320 | 480x480 round (T-RGB) |

**`HD_*_SRC_PX` is the size art is BAKED at; `HD_*_PX` is the size it is DRAWN
at.** `drawHDArt()` bridges the two, and `animator.cpp` static_asserts that the
compiled header matches, so a board wired to the wrong header fails to compile.
`drawSprite()`'s w/h are SOURCE dimensions, always - passing a drawn size once
read 153 KB past the end of an array and hard-faulted the board.

Pixel sprites (`src/ui/sprites.h`) carry the animated states: idle, scan,
happy, hungry, alert, hatch, type, laugh - 36 frames at 16x16, or 20x20 for
Sentinel, totalling about 20 KB.

**On the 480 panel the IDLE animation is now HD too.** `gen_hd_idle.py` derives
two 176x176 frames per stage from the same 400 px masters - a breath that scales
about each stage's own alpha bounding box, so the feet stay planted, plus a neon
pulse applied to RGB only so it provably cannot change the silhouette. That is
620 KB for ten frames. The other seven animations still fall back to pixel art,
which on this panel means a 16 px sprite blown up 8x.

Closing the rest of that gap is an art job, not a space one: 36 frames at
176x176 would be 2.13 MB against roughly 3.4 MB still free, but the poses have
to be generated. For scale, the whole remaining pixel-art inventory is about
37 KB while the HD stills alone are 1.27 MB.

Item icons and den tiles are baked at the size the panel DRAWS them, which is
32 px on the 480 - no scaling at draw time. A larger bake is a hypothesis worth
measuring first: the den tiles were byte-identical to a 2x enlargement at 32 px
because every feature sat on a coarse unit grid, so they needed a finer shading
pass to be worth the 6 KB rather than a plain re-run.

## Tests

```bash
pio test -e native
```

Fourteen suites: `test_content`, `test_content_pack`, `test_den`,
`test_event_bus`, `test_hexpass`, `test_inventory`, `test_ota`, `test_pet_core`,
`test_pet_forms`, `test_pet_memory`, `test_pet_rules`, `test_roam`,
`test_storage`, `test_touch_nav`.

They use this project's own `TEST(name)` and `ASSERT_EQ(actual, expected)` from
`test/test_stubs.h`, **not** Unity's macros.

**On Windows, `pio test -e native` swallows the compiler's stderr** and reports
a bare `ERRORED` with no diagnostic, because a fresh `cc1plus` cannot find its
runtime DLLs. Only the suite whose source actually changed recompiles, so a
stale cache hides it. Export the toolchain path **in the same command**:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH" && pio test -e native
```

## Building release images

```bash
python scripts/build_flashes.py <version>        # merged .bin per board
python scripts/stage_web_flasher.py <version>    # stage those into web/
```

`firmware/`, `web/firmware/`, `web/manifests/` and `web/boards.json` are
generated build output and are gitignored.
