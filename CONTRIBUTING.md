# Contributing to HexHound

Thanks for your interest in contributing to HexHound.

## Development Environment

### Prerequisites

- [VS Code](https://code.visualstudio.com/) with [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- SDL2 development libraries (for simulator)
  - Ubuntu/Debian: `sudo apt install libsdl2-dev`
  - macOS: `brew install sdl2`
  - Windows: download from [libsdl.org](https://www.libsdl.org/)
- Python 3.8+ with Pillow (for screenshot capture)

### Building

```bash
# Firmware (requires ESP32-S3 toolchain)
pio run -e lilygo-t-dongle-s3

# Desktop simulator (requires SDL2)
pio run -e desktop-sim

# Run the simulator
.pio/build/desktop-sim/program
```

### Running the Simulator

The SDL2 simulator renders the selected panel, scaled up. Which panel you get
is a **build environment**, not a runtime flag, because the layout family is a
compile-time decision: `desktop-sim` (160x80), `desktop-sim-wave` (320x172),
`desktop-sim-round` (240x240), `desktop-sim-round480` (480x480). Controls:

| Key | Action |
|-----|--------|
| SPACE | Short button press |
| ENTER | Long button press |
| S | Save screenshot |
| Q | Quit |

Fake scan data is injected automatically - no radio hardware needed.

### Running Tests

```bash
export PATH="/c/msys64/mingw64/bin:$PATH" && pio test -e native
```

Fourteen suites, using this project's own `TEST(name)` and
`ASSERT_EQ(actual, expected)` from `test/test_stubs.h` - **not** Unity's macros.

**On Windows, export the toolchain path in the same command as shown above.**
Otherwise `pio test` swallows the compiler's stderr and reports a bare
`ERRORED` with no diagnostic. Only the suite whose source changed recompiles, so
a stale cache can hide this for a long time.

## Rules that are specific to this project

These are not style preferences. Each one was learned by shipping the bug.

### Adding or changing a board must not move another board's firmware

This is the project's central invariant. Verify it, do not assume it:

1. Build the board at your base commit and keep the `.bin`.
2. Build it again with your change.
3. The sizes must be **identical**, and the only differing bytes must be
   `0xb0..0xcf` (`app_elf_sha256`) and the final 33 bytes (image checksum +
   SHA256). Those two always differ, because the ELF hash covers DWARF line
   numbers, so about 65 differing bytes is a clean result. Anything else is a
   real change to executable content.

**Guard with `#if`, do not trust `--gc-sections`.** An unreferenced method *is*
dropped by the linker, and dropping it still moved images by 16-24 bytes of
literal-pool and segment padding. Three separate times in one week, code added
outside a `#if HEXHOUND_PANEL_ROUND` / `#if HEXHOUND_HAS_TOUCH` guard grew every
other board: a helper method referenced from five sites cost 16 bytes on three
boards, and a lambda restructure cost another 16. If a file compiles on every
board - `ui_home.cpp`, `animator.*`, `ui_patrol_hud.cpp`, `game_layout.h` - then
anything you add outside a guard ships everywhere.

When you do have to find which edit moved an image, **bisect by reverting, do
not reason about it**. The obvious suspect has been wrong.

### The round layout is one family at two sizes

`src/ui/ui_round.*` serves both the 240x240 and the 480x480 panels. Every
hand-tuned constant goes through `uiround::scaled(x)` = `x * DIAM / 240`, which
reproduces the 240 value exactly by construction and is pinned by
`static_assert`.

**Text sizes passed to `uiround::` helpers are 240-RELATIVE** - the helper
multiplies by `TEXT_SCALE` internally, so pass `2`, never `ts(2)`. A raw
`_tft->setTextSize(n)` bypasses the helpers entirely and must become
`uiround::ts(n)`.

The classes of constant that hide from a search, in the order they were found -
finding one does not find the next:

1. bare `y` literals passed to `uiround::` helpers
2. relative offsets in those calls (`ty + 10`, `BODY_Y - 8`)
3. line pitches walked in loops (`y += 10`) and locally computed block heights
4. raw `fillRect` / `drawFastHLine` / `setTextSize` with literals
5. a screen with no round path at all

### Verify a layout by rendering it and looking at it

A similarity score against the 240 render doubled is a good triage signal and a
terrible verdict: two screens scoring ~96% were called "art related" and shipped
broken, and both were text overlap. Use the score to decide what to look at
first, then **look**.

Any screen can be captured headlessly:

```bash
HEXHOUND_SIM_FORCE_SCREEN=<name> HEXHOUND_SIM_SHOT_DIR=<dir>   HEXHOUND_SIM_SHOT_MS=6500 HEXHOUND_SIM_QUIT_MS=7200   ./.pio/build/desktop-sim-round480/program.exe
```

Boot takes about 4.3 seconds, so a shot earlier than that captures the splash
instead of your screen. Screens: `menu stats journal config missions quests
games inventory report den closet update roam roamreport brief patrol alert hud
game`.
Home is the default - omit the variable. Animated screens need several shots at
increasing times; identical frames mean nothing is animating.

**Set the state you are trying to look at, or you are looking at a fresh egg
that owns nothing.** Three more variables exist for exactly that, and each one
was added after a screen got reasoned about from source because the sim could
not be put into the state being reported:

| Variable | What it does |
|---|---|
| `HEXHOUND_SIM_STAGE=1..5` | Pet stage before any screen is drawn. Stage gates recipes, slots and half the HUD, so at the default EGG the Kit renders every cosmetic as locked. |
| `HEXHOUND_SIM_OWN=cos.lamp,cos.rug,...` | Grants cosmetics, stands the `COSMETIC_DEN` ones in the room and puts the `COSMETIC_WORN` ones ON the pet. Goes through `wearSet()`, so it cannot fabricate a state the closet could not reach. Unknown ids are skipped with a log line, never guessed. |
| `HEXHOUND_SIM_HUD_STAGE=1..5` | The HUD's own stage override; predates the general one and still works. |

Two traps this section is paying for:

- **A forced screen is not a running screen.** `HEXHOUND_SIM_FORCE_SCREEN=hud`
  had no patrol behind it, and `hud.update()` is only called by
  `updatePatrol()` - so every capture of that screen was the static frame
  `begin()` paints, and a defect that only `update()` could draw survived a
  screenshot that looked correct. Sim builds now drive `update()` for a forced
  HUD. If you add a screen whose animation is driven from somewhere else, drive
  it here too or the capture is worthless.
- **The 480 sim used to report `tier=core`.** `BOARD_HAS_PSRAM` comes from the
  PlatformIO board definition on hardware and a native build never sees it, so
  every tiered screen rendered at the wrong size - the den drew 3 places where
  the board draws 8. `desktop-sim-round480` sets it explicitly now. Check the
  `[BOOT] Board:` line in the log says the tier you expect.

### Sprite dimensions are SOURCE dimensions

`drawSprite()`'s `w`/`h` describe the array you are reading, never the size you
want on screen. Passing a drawn size instead once read **153 KB past the end of
an array**, which on hardware reads unmapped flash and panics. `drawHDArt(tft,
art, x, y, srcPx, dstPx)` exists to bridge baked size and drawn size;
`Animator::draw()` takes a *scale* and uses its own source dimensions.

### Line endings are LF

Git for Windows ships `core.autocrlf=true` in its **system** config, where it
does not show up in `git config --global --list`. If `git diff --stat` is far
larger than the change you actually made, that is a line-ending flip, not your
edit - check with `git diff --ignore-cr-at-eol`. Never "fix" a mass line-ending
diff by committing it.

### Commit messages

Conventional prefixes: `feat: fix: chore: docs: refactor: test:`. Write full
sentences that name the cause, not a bullet list of files - see `git log`.
**No `Co-Authored-By` trailers and no "generated with" footers, ever.**

## Code Style

- 2-space indentation
- Descriptive variable and function names
- Comment non-obvious logic; don't over-comment obvious code
- Keep functions focused and short
- Use `PROGMEM` for all sprite data and large constant arrays
- Prefer stack/static allocation over heap in hot paths
- Use `char[]` buffers instead of `String` concatenation in loops

## Pull Request Process

1. Fork the repo and create a feature branch from `main`
2. Make your changes, ensuring both firmware and simulator compile
3. Run the test suite - all tests must pass
4. Update documentation if your change affects user-facing behavior
5. Submit a PR with a clear description of what and why

## Hardware Variant Reporting

If you get HexHound running on a different board:

1. Open a GitHub Issue using the hardware variant template
2. Include: board name, pin differences, required code changes
3. Include hw_validator output and serial logs
4. Note any display driver or LED type differences

## Sprite Contributions

New pet sprites are welcome. Requirements:

- 16x16px (stages 1-4) or 20x20px (stage 5)
- RGB565 `uint16_t` PROGMEM arrays
- Magenta (`0xF81F`) for transparency
- Follow the palette defined in `src/ui/sprites.h`
- Include at least 2 idle frames and 1 scan frame

Art in this project is **generated, not hand-drawn**: each domain has its own
Pillow generator and its own output header, so two people working in parallel
never edit the same generated file. See the table in
[docs/build-environments.md](docs/build-environments.md#regenerating-pet-art).
A regeneration with default arguments must reproduce the committed output
byte-for-byte; if adding an option perturbs existing assets, fix the generator.

RGB565 is 2 bytes per pixel, so size drives flash hard. The five HD stage
stills alone are 1.27 MB on the 480 panel, while the entire remaining pixel-art
inventory is about 37 KB. Bake small things small.

## Licensing of contributions

HexHound is MIT licensed; see `LICENSE`. **Contributions are accepted under the
same licence**, so opening a pull request means you are offering your work under
MIT and confirming you have the right to do so.

That sentence exists because its absence is the thing that bites later. Without
it there is no agreed term covering inbound work, and a project with no
identified copyright holder and no inbound licence cannot relicense, dual
licence, or grant an exception without tracking down every contributor who ever
touched the file. One line now is cheaper than that conversation later.

No CLA, no sign-off ceremony, no separate document to read.

## Questions?

Open a GitHub Issue.
