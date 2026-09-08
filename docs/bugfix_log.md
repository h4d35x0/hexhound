# HexHound Bugfix Log

Changes made during the code quality audit and test development.

## Code Quality Pass

### TODO Resolution

1. **`src/main.cpp:110`** - `TODO: verify rotation value on actual hardware`
   - Converted to structured comment:
   ```
   // HARDWARE_VERIFY: TFT rotation value - confirm against physical board.
   // Current value: 1 (landscape). Try 3 if display is mirrored.
   // Reference: docs/hardware_checklist.md
   ```

2. **`src/modules/ble_module.h:11`** - `TODO: expand with real OUI prefixes`
   - Converted to structured comment:
   ```
   // HARDWARE_VERIFY: Tracker OUI prefixes - expand with real-world testing.
   // Current set covers Apple FindMy, Samsung SmartTag, and Tile.
   // Reference: docs/hardware_checklist.md
   ```

3. **`src/modules/ble_module.cpp:197`** - `TODO: refine heuristics with real-world testing`
   - Converted to structured comment.

4. **`src/modules/usb_module.cpp:7-10`** - `TODO: TinyUSB HID requires USB_MODE=0`
   - Converted to structured `HARDWARE_VERIFY` comment.

5. **`src/modules/usb_module.cpp:65`** - `TODO: verify HID initialization`
   - Converted to structured comment.

6. **`src/modules/usb_module.cpp:72`** - `TODO: detect actual USB host connection state`
   - Converted to structured comment.

7. **`src/modules/usb_module.cpp:141`** - `TODO: test HID typing speed`
   - Converted to structured comment.

8. **`src/modules/storage_module.cpp:17`** - `TODO: verify SPI pins for SD`
   - Converted to structured comment.

9. **`src/modules/notif_module.cpp:17`** - `TODO: verify LED type on your board revision`
   - Converted to structured comment.

### Include Guards

All `.h` files verified to use `#pragma once`. No issues found.

### Memory Audit

- `src/modules/storage_module.cpp:111` - `readLastJournalEntries()` uses `new String[]` heap allocation. This is called infrequently (only when entering Journal screen), so the pattern is acceptable. The `delete[]` cleanup is correctly placed.
- All sprite data confirmed in `PROGMEM`.
- No string concatenation in hot loops found.
- `ui_patrol_hud.cpp` confirmed to use stack-allocated `char[]` buffers for counter formatting.

### Error Handling

- Storage operations already check `_ready` flag and return false on failure.
- WiFi scan checks for `WIFI_SCAN_RUNNING` error codes.
- JSON parsing in `pet_core.cpp:loadFrom()` already handles `DeserializationError`.
- Config loading in `storage_module.cpp:loadConfig()` already handles parse errors.

### Magic Numbers

- Evolution cutscene phase durations are already defined as `PHASE_*_MS` constants.
- UI layout constants are defined as `#define` at file scope.
- No unaddressed magic numbers found.

## HAL Refactoring

### New Files Created

- `src/hal/hal.h` - Abstract hardware interface
- `src/hal/hal_hardware.cpp` - Real ESP32 implementation
- `src/hal/hal_sim.cpp` - SDL2 simulator implementation
- `src/hal/sim_font.h` - Minimal 6x8 bitmap font for simulator text rendering
- `src/hal/sim_main.cpp` - Simulator entry point (provides `main()`)
- `src/hal/sim_modules.cpp` - Fake WiFi/BLE/USB/Storage/Notif for simulator
- `src/hal/tft_compat.h` - TFT_eSPI compatibility wrapper for simulator builds

### Files Modified

All source files updated with conditional includes to support both hardware and simulator builds:
- `#ifdef SIMULATOR_BUILD` / `#include "hal/tft_compat.h"` / `#else` / `#include <Arduino.h>` etc.

### Build System

- Added `[env:desktop-sim]` to `platformio.ini` for native SDL2 builds
- Added `[env:native]` for unit test execution
- Simulator excludes hardware module implementations, uses `sim_modules.cpp` stubs

## T-Dongle S3 Bring-Up, Persistence, and Display Migration

### Summary

The LilyGo T-Dongle S3 flashed reliably, but the original full firmware display path did not stay visible on real hardware. The board and panel were ultimately verified as healthy by switching to LilyGo's own `esp_lcd` ST7735 driver path, then migrating HexHound's full firmware onto that backend.

### What Went Wrong

1. The original full app path compiled and ran, but the screen stayed black.
2. Raw display diagnostics proved the panel could show colors, so the hardware was not dead.
3. TFT_eSPI-based and custom software-TFT bring-up paths still failed to keep the screen visible during normal app boot.
4. A major hidden issue was GPIO `38` backlight handling. On the tested board, the backlight behaved correctly only when driven the way LilyGo's own factory code does: active-low via PWM. Resetting GPIO `38` after that setup caused the backlight to turn off again.

### What Went Right

1. Serial bring-up was stable from the start.
2. Raw display tests isolated the problem to the display stack instead of USB flashing or general boot stability.
3. LilyGo's official baseline color-cycle firmware worked immediately once mirrored locally.
4. That baseline gave a known-good reference for panel init, orientation, and backlight control.

### Final Fix

The working deployment path now uses:

- `lilygo-t-dongle-s3-vendor-baseline` for board validation
- `lilygo-t-dongle-s3-vendor-app` for full HexHound firmware
- `src/esp_lcd_st7735.[ch]` for the LilyGo ST7735 driver
- `src/hal/tft_compat.*` as the UI-facing wrapper
- `src/hal/backlight.*` for centralized, correct backlight handling

### Persistence Recovery

Follow-on hardware testing exposed a second real-device issue after the display path was fixed:

- evolution and patrol progress looked correct in RAM
- unplug/replug returned the pet to Egg
- boot logs showed both persistence backends were unavailable on the board:
  - `SPIFFS init FAILED`
  - `SD card init FAILED`
  - `No persistence backend available`

The persistence fixes now include:

- switching the onboard flash backend from `LittleFS` to `SPIFFS`, matching the board's default `default_16MB.csv` partition table
- formatting and mounting SPIFFS automatically on first boot when mount fails
- overwriting state/config saves instead of appending corrupted JSON
- recovering the last valid JSON object from older appended save files when possible
- adding `saveSeq` so boot chooses the newest valid pet save across SPIFFS and SD
- saving immediately after patrols, missions, and evolution events
- returning cleanly to Home after evolution so the new stage is visible immediately

Known-good real-device result:

- `lilygo-t-dongle-s3-vendor-app` boots with `SPIFFS ready`
- pet state saves to SPIFFS even when SD initialization fails
- progress survives unplug/replug on the tested board

### Key Files Added or Reworked

- `src/esp_lcd_st7735.c`
- `src/esp_lcd_st7735.h`
- `src/hal/tft_compat.cpp`
- `src/hal/tft_compat.h`
- `src/hal/backlight.cpp`
- `src/hal/backlight.h`
- `src/lilygo_vendor_baseline.cpp`
- `platformio.ini`

### Known-Good Result

HexHound now boots and displays correctly on the tested LilyGo T-Dongle S3 using the vendor-backed display path.

See `docs/t-dongle-s3-build-report.md` for the full chronology, diagnostics, failed hypotheses, and recovery path.

---

## 2026-08-28: The 480x480 round panel, and what a doubled panel breaks

Adding the LilyGo T-RGB reused the existing round layout family rather than
forking it. Generalising it surfaced one class of defect over and over: a
constant hand-tuned for the 240 panel that nobody scaled.

### Five classes of 240-tuned constant, found in this order

Finding one does **not** find the next. Sweeping for one pattern misses the rest.

1. Bare `y` literals passed to `uiround::` helpers.
2. Relative offsets inside those calls (`ty + 10`, `BODY_Y - 8`, `CY + 40`).
3. Line pitches walked in loops (`y += 10`) and locally computed block heights.
4. Raw `fillRect` / `drawFastHLine` / `setTextSize` with literals.
5. A screen with **no round path at all**. The missions screen fell through to
   the rectangular layout and drew under the bezel. It went unnoticed because
   the only round board at the time had no native USB, so the menu item was
   hidden there.

### The worst one was not a layout bug

`drawSprite()` takes its `w`/`h` as **source** dimensions and indexes
`sprite[row * w + col]`. Two evolution hero blits passed `HD_HERO_PX`, which
after the baked-size/drawn-size split is twice the baked size on the 480 panel,
so the cutscene read 320x320 out of a 25,600-element array: **153,600 bytes past
the end**. Correct by accident at 240, where the two sizes are equal. On
hardware that reads unmapped flash and panics, which presented as "the evolution
sequence never played, and afterwards the pet had already evolved".

Every hero blit now routes through one `drawHero()`.

### The patrol screens were missed entirely

They were simply absent from the phase's verified list, though both were
reachable through `HEXHOUND_SIM_FORCE_SCREEN` the whole time. The results screen
used a raw `step = 22` row pitch while a size-2 row is 32 px tall at 480, so
every row was drawn 10 px into the one above it. The live HUD's whole round
branch was 240 literals and used only the top half of the glass.

### Every GAME OVER screen drew its text on top of itself

`games::centerText()` forwards to `uiround::centerText()`, which multiplies a
240-relative size by `TEXT_SCALE`, so a "size 2" string is 32 px tall at 480,
while each of the four games computed its pitch as `8 * size + 4` = 20 px. The
six raw `setTextSize()` sites in `src/games/` had the same mistake inverted, and
were drawing at half size. `game_layout.h` now exposes `realTextSize()`,
`lineHeight()` and `linePitch()`, all identities on a rectangular panel.

The games were the last unverified surface at 480 for a simple reason: there was
no `HEXHOUND_SIM_FORCE_SCREEN=game` hook, so they could not be rendered. Adding
one found the bug immediately.

### Verification lessons

- **A similarity score is a triage signal, not a verdict.** Two screens scoring
  ~96% against the doubled 240 render were labelled "art related" and shipped
  broken. Both were text overlap. Render every screen and *look* at it.
- **A photograph of a round board is not evidence.** Two photos showed text
  running at ~45 degrees and read as a rotation bug; no display path can produce
  45 degrees, and the board was simply lying at an angle. Rendering the screen
  in the simulator showed the real defect in one shot.
- **"The linker drops it" is not "the image is byte-identical."** Unreferenced
  methods were garbage-collected and the image still moved 16-24 bytes through
  literal-pool and segment padding. Guard with `#if`.

### The guard rule, paid for three times in one week

Code added *outside* a `#if HEXHOUND_PANEL_ROUND` / `#if HEXHOUND_HAS_TOUCH`
guard, in a file that compiles on every board, ships everywhere:

- a private helper method referenced from five sites grew all three rectangular
  boards by **16 bytes each**;
- a lambda restructure added for bezel clamping grew them by another **16**, and
  was reverted;
- both rendered perfectly. The **byte comparison** caught them, not the render.

When hunting which edit moved an image, **bisect by reverting**. The obvious
suspect, a large art change, was measured and found innocent twice.

### The invariant, stated precisely

Two builds of *different* source with *identical* loadable content still differ
in exactly 65 bytes: `0xb0..0xcf` (`app_elf_sha256`, a hash of the whole ELF
including DWARF line numbers) and the final 33 bytes (image checksum + SHA256).
So the check is: identical **size**, and the only differing runs are those two.
A same-source rebuild is byte-identical, so the comparison is meaningful.


## 2026-08-29: A radar that drew under its own sprite, and a room with more places than the game has furniture

Three things reported from the T-RGB in one message, and two of them had already
been "fixed" once. Both of those had the same shape: the fix was correct and the
verification was blind.

### The pet's black box came back because update() repaints it every frame

The HD stage art is only chroma-keyed where the source was very nearly fully
transparent (`ALPHA_CUTOFF = 24` in `scripts/gen_hd_sprites.py`); every softer
edge pixel is composited over black and is therefore OPAQUE. On the home screen,
whose background is black, that is exactly right. On the patrol radar, where the
hound rides at the centre of the ring, it means the sprite carries a dark square
margin that punches out anything drawn underneath it.

The first fix carved a clear hub out of the crosshairs in `drawRadarFrame()`.
That function runs ONCE, from `begin()`. `update()` then drew its own
full-length `drawFastHLine` / `drawFastVLine` pair through the centre, and ran
every sweep beam from the centre, on all thirty frames a second after it. So the
board showed the defect continuously and the first frame did not.

Both paths now go through `UIPatrolHUD::drawCrosshairs()`, and `drawSweep()`
starts every beam at `hubRadius()` instead of at the centre. `hubRadius()` is
`petDrawWidth() / 2 + 2` on a round panel and **0** on a rectangular one, where
the pet sits in a corner and the centre is free: at 0 the two half-spokes meet
in the middle and the sweep runs from the centre, so those five boards paint
exactly the pixels they painted before.

### Why the simulator said it was fine

`hud.update()` is called from `updatePatrol()`, and `updatePatrol()` only runs
while a real scan is in progress. `HEXHOUND_SIM_FORCE_SCREEN=hud` parks the HUD
on screen with no patrol behind it, so **every capture ever taken of this screen
was the static frame `begin()` paints** and not one pixel of what `update()`
does. The screen was verified with a tool that could not see the defect.

Sim builds now drive `update()` for a forced HUD (`main.cpp`, section 8d), and
the forced HUD seeds three fake networks so the capture shows a working radar
rather than an empty ring.

### The hound was also just small

`petDrawWidth()` was `HD_HOME_SRC_PX / 2` - an 88 px pet inside a 216 px ring on
a 480 panel. It is `(HD_HOME_SRC_PX * 3) / 4` now: 132 px at 480 and 66 at 240,
both integral, about 61% of the ring on both round panels. The ring, the dots and
the counters did not move.

### The den had 8 places and the game has 5 pieces of furniture

`DEN_SLOT_COUNT` is 8. `ITEM_DEFS` contains exactly five `COSMETIC_DEN` items,
and two of those gate at Beacon Beast - so a Packet Pup that had crafted
everything it could reach still faced five empty brackets it had no way to fill.
That is not an empty state; it reads as missing content, and it was reported
from the board twice.

`denVisibleSlots(BoardTier)` is now `denSlotCap(BoardTier)` - a ceiling - and
`denRoomSlots(BoardTier, const PetState&)` decides what the room actually shows:
never fewer than `DEN_SLOTS_CORE` so an empty den is still a room, never fewer
than `owned + 1` so there is always at least one free place for the next thing
(EXACTLY one only when the room is packed from slot 0; a save furnished on the
old fixed-8 ring can hold an item at a high index, which pins the room wider
and leaves two or more gaps - reported from a board on 2026-08-30 as five slots
with three filled, and correct: shrinking past it would hide furniture),
never fewer than `denHighestOccupied()` so a save carried back from a richer
board cannot have an item hidden by the room shrinking around it, and never more
than the tier's cap. The cursor is clamped after every act, because the room can
now shrink under it.

### "3 of 8 out" was still on screen, in the branch that was not changed

The slot-selected branch of `UIDen::footerLeft()` had already been changed from
a slot count to an item count. The BACK branch had the same string and was
missed - and BACK is the position the screen opens on, so it is the one that got
photographed and read, correctly, as "three of my five things are out, where are
the other two". Both branches count ITEMS now. Against slots the number was
unfixable: the room has more places than the game has furniture, so it would
report a shortfall that no amount of play could close.

### A finished cosmetic now says where it goes

The Kit reported a bare `HAVE` for every crafted cosmetic, so five made things
read as five things for the den - two of which were `ANTENNA HAT` and
`SIGNAL SCARF`, which the den can never hold. Nothing on either screen had ever
said so. `rowDetail()` now prints `HAVE:DEN`, `HAVE:WORN` or `HAVE:FX`.

### Two simulator gaps closed, both of which had produced wrong answers

- **The 480 sim reported `tier=core` while the board it simulates reports
  `tier=rich`.** `BOARD_HAS_PSRAM` comes from the PlatformIO board definition on
  hardware and a native build never sees it, so `Caps::tier()` answered CORE and
  every tiered screen rendered at the wrong size. The den was the visible case:
  the sim drew 3 places and the board drew 8, so a den screenshot from the sim
  was a picture of a smaller room than the one being reported from the glass.
  `desktop-sim-round480` sets `-DBOARD_HAS_PSRAM=1`.
- **`HEXHOUND_SIM_OWN` and `HEXHOUND_SIM_STAGE`.** The Kit and the Den could
  only ever be captured EMPTY and at the EGG's stage, so the rows that report
  what you own and where it goes were being reasoned about from source rather
  than looked at. `HEXHOUND_SIM_OWN=cos.lamp,cos.rug,...` grants cosmetics and
  stands the den ones in the room; `HEXHOUND_SIM_STAGE=1..5` sets the stage
  before any screen is drawn.

### One test lesson, again

Both new den tests compiled, ran under a green `pio test -e native`, and were
never executed: this suite registers cases explicitly with `RUN_TEST()` in
`main()`, and adding a `TEST()` block does not add it to the run. The suite
reported 19 passed with them present and 21 after they were registered. **Read
the case count, not the PASSED line.**

## 2026-08-30: The pet can wear what it makes, and the radar stops erasing itself

### The black box was drawWornOn's neighbour all along: a fillRect

Reported a second time as "there is still a black background behind them
blocking the radar", after the hub fix above. The hub fix was correct about the
symptom and wrong about the cause, and the cause was measurable the whole time.

Measured, on the baked 176 px Packet Pup portrait actually compiled into this
board (`src/ui/sprites_hd_round480.h`):

    30976 pixels, 25658 of them the 0xF81F chroma key   (82.8%)
    all four corners chroma
    126 opaque pixels below luminance 8, hugging the silhouette

**The sprite paints no box.** `drawSprite()` skips every chroma pixel. What drew
the box is one line in `drawPetCorner()`: a `fillRect` of `COL_BG` over the
pet's whole bounding square, AFTER the crosshair and the sweep had been drawn
through it. A hard-edged black rectangle stamped over the radar, every frame.

Carving a hub out of the lines only stopped them entering the rectangle, so the
box became a void instead - the same thing to look at. `hubRadius()` is 0 now,
the lines run to the centre again, and the fillRect is gone on round panels:
`update()` already erases the whole ring interior with one `fillCircle` before
drawing anything, and the pet's box sits comfortably inside it (132 px box,
corners 93 px from centre, interior cleared to 107).

**The lesson is the same one three entries above.** A theory about art that was
never checked against the art. `ALPHA_CUTOFF = 24` was read out of the generator
and reasoned from; the generated header was not opened until the second report.

### Worn cosmetics: the feature the item table has been describing since Phase 1

`COSMETIC_WORN` was authored with four items and four recipes, two of them
reachable at Packet Pup, and nothing ever read it. Before this change
`COSMETIC_WORN` appeared in exactly ONE place in the whole firmware: a label on
the Kit screen added the day before. Crafting an ANTENNA HAT produced a row in a
list. Asked as "how can you add or remove items from the den or off the pet",
and the honest answer was that you could not put them on either.

What was added:

- **`WearAnchor`** in `item_defs.h`: `WEAR_HEAD` / `WEAR_NECK`. Two anchors, not
  one worn slot, because the content was already authored that way - hat and
  goggles are both head, scarf and collar are both neck - and one slot would
  have made a Beacon Beast choose between its goggles and its collar.
- **`PetState::wornSlots[WEAR_SLOT_COUNT]`**, schema **v3 -> v4**. Width lives
  in `config.h` because `pet_core.h` cannot include `item_defs.h`; a
  `static_assert` in `item_defs.h` refuses to let the two drift, since an
  anchor added without widening the array writes past a persisted struct.
- **`src/pet/pet_wear.h`**: free inline rules, same shape as the den's, so they
  are testable with no display. The cycle is the den's cycle - bare, each owned
  item that fits, bare - because that is the verb the den already taught.
- **`src/ui/ui_worn.*`**: one entry point, `drawWornOn(tft, stage, x, y, px)`,
  called by every screen that draws the pet. `px` is the size the PET was drawn
  at; the anchor table is in thousandths of that, so it is unchanged for a 24 px
  pet on a T-Dongle and a 320 px hero shot on the T-RGB.
- **`src/ui/ui_closet.*`** and `SCREEN_CLOSET`, next to the DEN in the menu.
- Generated art: `scripts/gen_worn_art.py` (72 KB at 480, 18 KB at 240/big,
  4.5 KB compact) and `scripts/gen_wear_anchors.py`.

It is drawn on the home screen, in the den, on the patrol radar, and through
every phase of the evolution cutscene including the growing reveal and the
landing bounce. `drawHero()` takes the stage now so no call site can forget it:
a hat that vanished for the length of the cutscene would read as having been
lost in the transformation.

### Four heuristics, four wrong anchor tables

The per-stage anchor points were going to be measured from the silhouettes.
Every automatic rule tried placed at least one anchor visibly wrong:

1. "the shoulder line is the first row at least 55% as wide as the widest" put
   the gremlin's neck twenty-five rows INSIDE its head - its ears are as wide
   as its shoulders.
2. "the neck is the narrowest row above the torso" returned the single-pixel
   top of every skull.
3. "the neck is the narrowest row between the head's widest row and the
   torso's" found the shoulders for the head and put necks at 72% of height.
4. "the first local minimum in the upper 40%" works on three stages: the Beacon
   Beast has a notch between its crest and its head that it picks instead, and
   the gremlin has no minimum at all.

Five hand-drawn characters with different anatomy - an ovoid with no head, a
quadruped, two bipeds, and something with ears wider than its body. **Thirty
numbers read off the art beat a rule that has to generalise over all of them.**
The script now holds an authored table and VERIFIES it: every anchor must land
inside the figure's silhouette and every box must fit the canvas, so a typo
fails the script. `--preview` renders the overlays onto all five stages, which
is the check that actually found each of the four failures.

Each wrong table looked perfectly reasonable as a column of numbers.

### A green suite that was hiding a failure, and one that was hiding a skip

Two separate things, one day apart, both about reading the wrong line:

- `test_wear`'s twenty cases had to be registered by hand with `RUN_TEST()`.
  See the entry above; the same trap, checked for this time.
- Bumping `PET_SCHEMA_VERSION` to 4 broke `test_hexpass`, which pins the version
  deliberately. The **suite summary still showed 15 PASSED** because the broken
  one ERRORED and dropped out of the list entirely rather than reporting FAILED.
  A suite vanishing looks exactly like a suite that was never there.
  **Count the suites, not the passes.** The pin was right and did its job: it
  forced the question of what a v2 save does with a field it has never heard of,
  and both migration tests now assert the pet arrives bare.

## 2026-09-02: A keyboard that could not tell whether it had let go

One fix, in `src/modules/usb_module.cpp`. It is the only item on the board with
a safety consequence, and it is the second stuck-modifier incident on this
project.

### What happened on 2026-09-01

A T-RGB running `[env:lilygo-t-rgb-hid-demo]` opened LinkedIn and Microsoft 365
repeatedly while mission 6 (Map Link) was being filmed, and left the owner's
keyboard and mouse unusable until he closed the laptop lid. The lid worked
because a sleep/wake cycle re-enumerates USB, which clears a held modifier.

### The evidence that a modifier was held

The URL mission 6 types is
`https://maps.google.com/?q=3284+Northside+Parkway+NW,+Atlanta,+GA`. With GUI
held, the four digits in `3284` are `Win`+3, `Win`+2, `Win`+8 and `Win`+4, and
each of those launches a pinned taskbar app. That is a direct match for the
symptom, and it also explains the dead keyboard and the dead mouse with one
cause rather than two.

### The defect, which is NOT the same as the mechanism

`USBHIDKeyboard::sendReport()` is declared `void` and discards the `bool`
returned by `USBHID::SendReport()`. So every `press()`, `release()` and
`releaseAll()` in the Arduino keyboard API is fire-and-forget, and the whole
safety story of this module rested on a call that returns nothing.

`pressCombo()` ended with two `releaseAll()` calls and a comment saying "a
second release costs nothing and covers a dropped first report". It covers
nothing. `releaseAll()` zeroes the library's local `_keyReport` and calls
`sendReport()`. If the host was unready, if the report mutex timed out, or if
the completion callback never arrived, BOTH calls returned normally having
transmitted nothing - and the device then believed the keyboard was clear.
Nothing afterwards re-asserted it, because the local state already said clear.

**The mechanism was NOT reproduced and this entry does not claim one.** Reading
the HID stack, the simplest story - "a release report was dropped, so GUI stayed
held for the rest of the typing run" - does not fully close, because every
later report carries the complete modifier byte and a delivered keystroke
report with `modifier=0` should clear a stale GUI on the host. What is certain
is that the old code could not have detected the failure whatever caused it,
and could not have recovered from it.

### The fix

Built so it does not depend on which mechanism it was.

- `clearAllKeys()` sends an explicit all-zero report through a second `USBHID`
  handle whose return value is CHECKED, and requires TWO confirmed
  transmissions spaced 20 ms apart. The report is idempotent, so a host that
  receives both is in the same state as a host that receives one; two spaced
  sends is what makes a single lost report survivable.
- `pressCombo()` returns `bool`. It clears BEFORE building the combo, because
  `press()` ORs into whatever `_keyReport` already holds, and returns the
  verified clear after.
- **If the release cannot be confirmed, nothing gets typed.** `executeOpenURL()`
  aborts before the URL, and clears again immediately before the typing run
  rather than trusting the clear from 500 ms earlier. This failure path is the
  point: a fix that only worked when reports were delivered would fix nothing.
- `executeMission()` refuses to start on an unconfirmed state, and will not
  publish `EVENT_MISSION_COMPLETE` or award XP unless the closing clear is
  confirmed. That closing clear also covers the plain-typing missions, because
  `print()` sets SHIFT for every capital letter.
- `update()` retries an unconfirmed clear once a second, gated on a
  non-blocking `ready()` check so an absent host cannot stall the UI loop. A
  host that was merely busy or suspended gets its keyboard back on its own.
  Nobody should have to rediscover "close the laptop lid".

### What this cannot do

If `SendReport()` succeeds but the host still holds the modifier, the device
cannot tell. Nothing on the device side can. The recovery retry in `update()`
is the mitigation for that case, not a detection of it.

### The six non-HID images do not move

Everything above is inside `#ifdef USB_MODE_HID`, the CDC stubs are
unreferenced and get collected, and the two new pieces of state are file-scope
statics rather than members - adding fields to `USBModule` would have grown its
singleton in `.bss` and relocated everything after it in every image.

Measured against a cold T-Display S3 baseline taken at HEAD before any edit:
**64 differing bytes, of which 31 are the ELF hash in the app descriptor and 33
are the trailing image SHA-256 plus checksum. Zero in the middle.** Both HID
images (`lilygo-t-rgb-hid`, `t-dongle-s3-hid`) build.

### Still unproven

None of this has run on glass. `docs/hardware_checklist.md` section 10 is the
bench step, and it is explicit about which half of the claim a bench pass can
actually judge.

## 2026-09-02 (second pass): the interface that registers too late

Four parallel lanes plus integration, chasing why the T-RGB was cycling on USB
with a HID image. The headline result is a defect found BEFORE it shipped, in
the diagnostic tool built to find a different one.

### The trap: a console build with a dead keyboard

`[env:lilygo-t-rgb-hid-diag]` is a composite CDC+HID image - a keyboard AND a
serial port - so that a crash in a HID image stops being invisible. Flipping
`ARDUINO_USB_CDC_ON_BOOT` to 1 is all it takes to build, and it would have
enumerated a working console attached to a keyboard that never types.

`USB.h:23` defines `ARDUINO_USB_ON_BOOT` as the OR of the three ON_BOOT macros,
so that one flag makes it true. `cores/esp32/main.cpp` `app_main()` then calls
`Serial.begin()` and `USB.begin()` under
`#if ARDUINO_USB_ON_BOOT && !ARDUINO_USB_MODE` - before `initArduino()`, before
`loopTask` exists, and long before `USBModule::init()` at STEP 13 of `setup()`.
The HID interface is registered by the `USBHID` constructor, and
`esp32-hal-tinyusb.c:661-664` refuses once TinyUSB is up:

    log_e("TinyUSB has already started! Interface HID not enabled")

**The only report of that failure is a `log_e`, and it goes to the IDF console
on UART0** (`sdkconfig` pins `CONFIG_ESP_CONSOLE_UART_DEFAULT`), so it does not
reach the CDC console you would be watching. Raising `CORE_DEBUG_LEVEL` does not
help for the same reason. A silent HID failure inside the image whose entire
purpose is that failures stop being silent.

Two lanes found this independently, from opposite directions - one auditing the
USB configuration, one building the env - which is the reason it was caught.

**The fix** is that the keyboard and its report-status handle are file-scope
objects rather than `new`ed in `init()`. Global constructors run in
`do_global_ctors()` before `app_main()`, so the interface is registered before
anything can start TinyUSB. This works in BOTH USB configurations rather than
only the one that ships today. Declaration order is load-bearing: the keyboard
is declared first so its own `USBHID` member is the one that calls
`addDevice()`.

Verified in the linked binary rather than by reasoning:

    _GLOBAL__sub_I__ZN9USBModule9_missionsE:
        call8 <USBHIDKeyboard::USBHIDKeyboard()>
        call8 <USBHID::USBHID()>

### The boot-time all-clear

`init()` now arms a verified all-clear, gated on `ready()`. This closes the one
hole the 2026-09-02 modifier fix cannot cover from inside a mission: if the chip
resets while a modifier is down, no code on the device can send the release, and
the host keeps believing GUI is held.

`ready()` false is the normal case on every power-up (enumeration takes hundreds
of milliseconds), so nothing is printed and the retry is simply armed. A
separate `_bootClearPending` flag exists so that `update()`'s success line does
not say "Key state recovered" on every ordinary boot - the same cry-wolf defect
relocated one function along, which is what the first draft would have shipped.

**The recovery machinery already existed and was never armed.** At the previous
commit `_keysUnconfirmed` starts `false`, so `update()`'s retry loop could not
run on a fresh boot at all.

### What the reset loop is NOT

Ruled out by measurement, not by argument:

- **Not the demo-only flags.** `HEXHOUND_DEMO_STAGE=4` is `STAGE_GREMLIN`, and
  `PetStage` starts at 1, so every stage-indexed table takes index 3 of 5. All
  bounds enumerated; all guarded by a clamp, a `switch` default or a
  `static_assert`. The demo image measures 8 bytes LESS static RAM and ~8 KB
  less flash than the working HID image.
- **Not memory.** The HID image adds 12368 bytes of static DRAM (TinyUSB's
  buffers) against ~236 KB free heap. The only dynamic display allocation is a
  compile-time-constant PSRAM canvas, and it is null-checked.
- **Not `Serial` blocking.** In a CDC-off HID build `Serial` is plain UART0;
  `operator bool()` is "is the driver installed", true immediately, so
  `main.cpp:370`'s wait exits on its first evaluation.
- **Not the descriptor's power request.** TinyUSB advertises bMaxPower 500 mA
  self-powered; 500 mA is the maximum a device may request, and a host that
  refuses it fails enumeration rather than causing a reset.
- **Not a hang.** `loopTaskWDTEnabled = false`, CPU1's idle task is not watched,
  and the RTC WDT is disabled at startup. **A hang on this firmware produces a
  hang, not a reset.** So if the board really is resetting, the cause is narrow:
  panic, brownout, deep sleep, or a deliberate restart.

### The evidence is on the board, and it always was

`main.cpp:1078` journals `BOOT | RESET REASON | <reason> <mV> x<mult>` on every
boot, `appendJournal()` is a pure append with no rotation, and
`HEXHOUND_NO_PERSIST` guards only `savePetState()`. So the reset reasons survive
reflashing - SPIFFS at `0xc90000` is untouched by an app write, and
`partitions.bin` is identical across all three T-RGB envs.

Measured across the 2026-09-01 backup, 84 recorded boots: **59 `UNKNOWN`, 22
`POWERON`, 1 `TASK_WDT`, 2 split across SPIFFS pages. Zero `BROWNOUT`, zero
`PANIC`, and zero `SYSTEM|SLEEP` entries of any kind.**

That backup PREDATES the incident - it contains no `MISSION` entries, and
missions demonstrably fired during it. The boots that matter are on the device.

Two things this does establish: this board has never browned out, and soft sleep
has never once been triggered on it, which independently confirms that the sleep
work remains unproven on glass.

### A caution about the inference that started all this

"Repeated connect/disconnect means the chip was resetting" is an INFERENCE, not
an observation. A host failing enumeration and re-driving the port produces the
identical symptom with nothing resetting at all. On Windows that appears as
"Unknown USB Device (Device Descriptor Request Failed)". The reset-reason line
settles it in one boot, which is what the diagnostic env is for.

## 2026-09-08: the confirm-gate suite had never linked, and CI had been red for days

### What was wrong

`pio test -e native` failed on every run from the moment `test_mission_confirm`
landed. All four build jobs passed; only "Run Tests" failed, and inside it 19 of
the 20 suites passed. The twentieth ERRORED with undefined references to
`UIMissions::instance/init/drawBriefing/longPressShouldExecute` and
`USBModule::instance/getMission`.

`[env:native]` sets `test_build_src = no` and `build_src_filter = -<*>`, so the
test runner compiles no project sources whatsoever. Every suite pulls in the
implementations it needs by `#include`-ing the `.cpp` itself. This suite
included only `ui_missions.h` and `usb_module.h`, so it could not link.

### Why it mattered more than a red badge

The suite is the test coverage for the GUI-keys confirm gate, the control in
front of the two missions that press the host's GUI modifier. Item H closes
with "Covered by `test/test_mission_confirm/`, 29 cases, mutation verified."
That sentence was true on one developer machine and false everywhere else: the
suite could not link under `pio test`, so on the shared runner it covered
nothing at all.

The gate's own two mechanisms were never affected and both still hold: the gate
itself in `longPressShouldExecute()`, and the compile-time backstop of
`executeMission()` taking a `GuiKeyConsent` with no default. The consent
backstop is what would actually have caught a deleted gate, and it is a
compile error, not a test. What was missing was the evidence that the gate's
own state machine still behaves: which hold arms, which runs, and every way an
arm is supposed to expire.

### The fix

The four sources it needs are included below the fake HAL singletons rather
than beside the headers at the top of the file. Placement is load-bearing:
under `-DUNIT_TEST`, `event_bus.cpp` includes no Arduino and no simulator
header at all, on the assumption that the suite already defined `Serial`, and
that only becomes true partway down the file.

### The thing that hid it

`scripts/run_native_suites.sh` carried a special case giving this one suite
different flags and passing its four sources as separate translation units. It
therefore ran green locally while CI could not link it at all. That special
case is removed and every suite now builds exactly the way CI builds it.

A local runner that is allowed to differ from CI is not a check, it is a second
opinion that always agrees with you.

### Verified

20 suites, 439 cases, 0 failed, through the runner with `[env:native]`'s flags.
`test_mission_confirm` on its own: 29 passed, 0 failed.
