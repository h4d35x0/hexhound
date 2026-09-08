# P1-W3 - The Den: wiring notes

`SCREEN_DEN` is built and verified but **not routed**. `src/main.cpp`,
`src/config.h` and `src/ui/ui_menu.*` are owned by other workstreams, so this
document is the handover: everything below is a change to a file this
workstream did not touch.

Files added:

| File | What it is |
|------|-----------|
| `src/ui/ui_den.h` | The screen, plus the den's placement RULES as inline free functions |
| `src/ui/ui_den.cpp` | Pixels only: the room, the pet, the slots, the chrome |
| `src/ui/den_art.h` | GENERATED room tiles (wall / floor / horizon) + palette |
| `scripts/gen_den_art.py` | The generator for the above; re-run after editing it |
| `test/test_den/test_den.cpp` | 19 tests: the id-0 invariant, placement, tier limits |

One existing file was changed, and only additively:

| File | Change | Why |
|------|--------|-----|
| `src/ui/ui_inventory.h/.cpp` | Added `itemIconFor(uint8_t)` and `UI_ITEM_ICON_PX` | `src/ui/item_art.h` says "include from ONE .cpp only" and means it: the icon tables are `static const`, so a second translation unit including it bakes a second copy into flash (2,560 bytes on a 160x80, 10,240 on the big and round panels). The den asks `ui_inventory.cpp` for an icon instead of including the header again. |

That change cannot regress any current board: nothing in the firmware
references `ui_inventory.o` yet, so it is not linked. Measured below.

---

## Wiring

### 1. Include and init

`src/main.cpp`, beside the other screen includes and inits:

```cpp
#include "ui/ui_den.h"
...
UIDen::instance().init(tft);
```

### 2. Menu entry

`src/ui/ui_menu.*` needs a `MENU_DEN` entry in `MENU_ORDER`, then in
`src/main.cpp`'s menu long-press switch:

```cpp
case MENU_DEN:
    switchScreen(SCREEN_DEN);
    UIDen::instance().open();
    break;
```

Gating is the menu team's call. A reasonable one: show it from
`STAGE_PACKET_PUP`, which is when the first den cosmetic
(`cos.lamp`) becomes craftable. Do **not** gate it on owning something -
an empty den is a first-class state and seeing the empty room is how a
player learns the den exists to fill.

### 3. Short press (cursor)

In the short-press screen switch:

```cpp
case SCREEN_DEN:
    UIDen::instance().scrollDown();
    break;
```

### 4. Long press (act)

In the long-press screen switch:

```cpp
case SCREEN_DEN: {
    if (UIDen::instance().backSelected()) {
        switchScreen(SCREEN_MENU);
        UIMenu::instance().open();
    } else {
        const DenResult r = UIDen::instance().act();
        if (r != DEN_OK_PLACED && r != DEN_OK_SWAPPED &&
            r != DEN_OK_CLEARED) {
            showAlert(NOTIF_INFO, UIDen::resultText(r), "Den");
        }
    }
    break;
}
```

`act()` redraws itself on success and writes nothing on failure. The toast on
the failure branch is **required, not optional**: owning no den cosmetics is
normal early on, and a long press that does nothing and says nothing is how a
player concludes the button is broken. `UIDen::resultText()` supplies the
sentence for every result, including `DEN_NONE_OWNED` ("craft a den item
first") and `DEN_NONE_FREE` ("everything is out already").

### 5. Nothing else

The den needs no tick, no timer and no event subscription. It writes exactly
one thing, `PetState::denSlots[]`, and sets `dirty` so the existing save path
picks it up. It never calls a stat mutator.

### 6. Simulator force-screen (optional)

`src/main.cpp`'s `HEXHOUND_SIM_FORCE_SCREEN` block has no `den` case. Adding
one is a two-line change and makes the screen capturable by
`scripts/capture_screens.py`:

```cpp
} else if (!strcmp(fs, "den")) {
    switchScreen(SCREEN_DEN); UIDen::instance().open();
}
```

Note `SCREEN_INVENTORY` has no case either, so the two are in the same boat.

---

## Measured cost

Every figure below is from a real link. Unrouted code is stripped, so simply
adding these files changes nothing; the numbers come from building with the
wiring above actually in place, then reverting it. Three points per board:

* **baseline** - the tree as handed over, nothing routed
* **dependency** - a probe linking exactly the inventory surface `ui_den.cpp`
  touches (`itemCount`, `count`, `definition`, `nameFor`, `itemIconFor`) and
  nothing else
* **wired** - the full wiring from section 1-4 above

### T-Dongle S3 (`lilygo-t-dongle-s3-vendor-app`, TIER_CORE, 160x80, 8 px art)

| Build | RAM | Flash |
|-------|-----|-------|
| baseline | 78,964 | 1,168,857 |
| dependency only | 79,580 | 1,173,345 |
| den wired | 79,580 | 1,175,745 |

* **The den itself: +8 bytes RAM, +2,400 bytes flash.**
* The shared inventory dependency: +608 bytes RAM, +4,488 bytes flash.

### Waveshare 1.28 round (`waveshare-esp32-s3-lcd-128`, TIER_RICH, 240x240, 16 px art)

| Build | RAM | Flash |
|-------|-----|-------|
| baseline | 76,816 | 1,502,397 |
| dependency only | 77,424 | 1,514,561 |
| den wired | 77,432 | 1,517,921 |

* **The den itself: +8 bytes RAM, +3,360 bytes flash.**
* The shared inventory dependency: +608 bytes RAM, +12,164 bytes flash.

`xtensa-esp32s3-elf-size -A`, baseline vs wired, round board:

```
.dram0.bss     42784 ->  42792  (+8)       the UIDen singleton
.dram0.data    34032 ->  34640  (+608)     inventory dependency
.flash.rodata 552952 -> 565896  (+12944)   item icons 10240 + den tiles 1536
.flash.text   832483 -> 834455  (+1972)
```

### Reading these numbers honestly

The dependency line is the important one and it is **not** the den's bill. At
the handover commit nothing in the firmware references `Inventory`, `ITEM_DEFS`
or `ITEM_ICONS`, so P1-W2's whole subsystem is linked out. The den is simply
the first screen to be wired, so it is the one that pays the entry fee. Wiring
`SCREEN_INVENTORY` first would move that 608 bytes of RAM onto the inventory
screen's ledger and the den's marginal cost would be unchanged. Whichever lands
first pays it once; the second pays nothing.

The den's own 8 bytes of RAM are the `UIDen` singleton (a `TFT_eSPI*` and an
`int` cursor). There is no frame buffer, no scratch bitmap and no allocation
anywhere in this screen - the room is tiled straight to the panel.

### Art budget

`src/ui/den_art.h` compiles exactly one tile set, chosen by panel family, the
same rule `item_art.h` uses:

| Panel family | Tile size | Flash |
|---|---|---|
| 160x80 | 8 px | 384 bytes |
| 320x172 and 240x240 round | 16 px | 1,536 bytes |

Three seamless tiles, not a backdrop image. A full-screen RGB565 backdrop would
have been 25,600 bytes on the T-Dongle and 115,200 on the round panel, and
would need redrawing for every new panel. The tiler in `ui_den.cpp` is
phase-locked to absolute screen coordinates and clips exactly, so the same
three tiles fill a room of any size on any panel.

---

## Verification performed

* `pio test -e native` - 9 suites, all green (8 pre-existing plus `test_den`,
  19 tests). No pre-existing test was modified.
* `pio run` - `lilygo-t-dongle-s3-vendor-app` and `waveshare-esp32-s3-lcd-128`,
  both clean.
* `pio run` - `desktop-sim`, `desktop-sim-wave`, `desktop-sim-round`, all clean.
* Rendered headlessly and inspected on all three panels: empty den, partly
  furnished, fully furnished, cursor on an empty slot, cursor on a filled slot,
  and the owns-nothing case. Both tiers were rendered - the 8-slot TIER_RICH
  layout was forced with `PLATFORMIO_BUILD_FLAGS="-DBOARD_HAS_PSRAM=1"` rather
  than by editing `platformio.ini`, since no simulator env declares PSRAM.

## Layout, per panel

| Panel | Slots | Arrangement |
|---|---|---|
| 160x80 | 3 (core) | pet at the back left against the horizon, one row of slots standing on the floor, no labels (a 41 px cell holds six glyphs) |
| 320x172 | 3 core / 8 rich | same, plus a second row on a wall shelf when the board shows more than four slots; names under each object |
| 240x240 round | 3 core / 8 rich | pet in the middle of the room, belongings in a ring around it. The ring is an **ellipse** (rx 68, ry 52), not a circle: squashing it vertically bounds the extremes so no slot lands on the title or the footer for **any** slot count, rather than tuning a start angle per count |

## Things a future change should not break

1. **`denSlots[i] == 0` means empty, and item id 0 is `mat.scrap`.** That pun is
   safe only while no `COSMETIC_DEN` item has id 0.
   `test_every_den_item_has_a_nonzero_id` and `denZeroMeansEmptyIsSafe()` pin
   it. A `static_assert` would be better but `ITEM_DEFS` is not `constexpr`
   (its rows hold `const char*`), so its members are unreadable in a constant
   expression.
2. **Nothing writes a slot without `denIsPlaceable()`**, which requires
   `ITEM_COSMETIC` + `COSMETIC_DEN`. A corrupt or future item id in the array
   is drawn as empty, never trusted.
3. **The full `denSlots[]` array persists on every board.** `TIER_CORE` hides
   the extra slots; it must never clear them, or a pet moved from a rich board
   to a core one and back loses half its room. Hiding is not clearing.
4. **The tier is read via `Caps::tier()`, never an `#if`.** The slot count is a
   function *of* a tier (`denVisibleSlots(BoardTier)`) so both answers are
   testable in one native build.

## Known follow-ups (not done, deliberately)

* `fitText()` now exists in three screens (`ui_inventory.cpp`, `ui_quests.cpp`,
  `ui_den.cpp`) as a file-local copy. Hoisting it into `ui_utils.h` is the right
  fix, but it means editing two screens this workstream does not own during a
  parallel workstream. Worth doing once P1 lands.
* The den has no `HEXHOUND_SIM_FORCE_SCREEN` case (section 6), so
  `scripts/capture_screens.py` cannot pick it up and no screenshot ships in
  `docs/screenshots/`. Same gap as `SCREEN_INVENTORY`.
