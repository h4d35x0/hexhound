# P1-W2 Inventory, Crafting and Cosmetics - Integrator Wiring

Items, recipes, an inventory engine and the `SCREEN_INVENTORY` list screen now
exist. They compile on every target, but nothing routes to them yet, so the
linker garbage-collects almost all of it. This file is the handover.

The files that must be edited to finish the wiring (`src/main.cpp`,
`src/ui/ui_menu.*`, `src/config.h`) are owned by other work, so P1-W2 did not
touch them. Neither did it touch `src/pet/pet_core.*`, `src/games/`,
`src/content/content_store.*`, `src/ui/sprites_hd_*.h` or
`scripts/gen_hd_sprites.py`.

## Files added

| File | Purpose |
|------|---------|
| `src/content/item_defs.h` | Item and recipe tables as DATA, plus the types. |
| `src/pet/pet_inventory.h` / `.cpp` | The engine: counts, stack rules, recipe validation, crafting, pack loading. |
| `src/ui/ui_inventory.h` / `.cpp` | The `SCREEN_INVENTORY` list screen, all three panel families. |
| `src/ui/item_art.h` | GENERATED. RGB565 item icons. |
| `scripts/gen_item_icons.py` | Draws those icons with Pillow and bakes the header. |
| `test/test_inventory/test_inventory.cpp` | 30 tests. Seventh suite; the six that existed are untouched. |

No existing file was modified. `build_src_filter` is `+<*>` on every firmware
and simulator environment, so the new translation units are picked up with no
`platformio.ini` change.

## The item ids the den team needs

**The numeric id is the index into `PetState::items[]` and it is persisted.**
Append only. Never insert, never reorder, never delete a row in `ITEM_DEFS`: a
save written by an older firmware indexes that array positionally, so renumbering
turns a player's Signal Scarf into a Cracked Lens.

`ITEM_TYPE_COUNT` is 24 and 20 are used, so four ids are spare for a later
append with no schema bump.

### Materials (ids 0-7)

| id | string id | name | accent |
|----|-----------|------|--------|
| 0 | `mat.scrap` | SCRAP | `0x8410` |
| 1 | `mat.signal` | SIGNAL FRAG | `0x07FF` |
| 2 | `mat.circuit` | CIRCUIT BIT | `0x07E0` |
| 3 | `mat.shard` | DATA SHARD | `0x881F` |
| 4 | `mat.glitch` | GLITCH FRAG | `0xF81E` |
| 5 | `mat.copper` | COPPER WIRE | `0xFB60` |
| 6 | `mat.lens` | CRACKED LENS | `0xAEDF` |
| 7 | `mat.ferrite` | FERRITE BEAD | `0x5AEB` |

### Cosmetics (ids 8-19)

| id | string id | name | slot | stage gate |
|----|-----------|------|------|-----------|
| 8 | `cos.antenna` | ANTENNA HAT | `COSMETIC_WORN` | Packet Pup |
| 9 | `cos.scarf` | SIGNAL SCARF | `COSMETIC_WORN` | Packet Pup |
| 10 | `cos.goggles` | RECON GOGGLES | `COSMETIC_WORN` | Beacon Beast |
| 11 | `cos.collar` | LED COLLAR | `COSMETIC_WORN` | Beacon Beast |
| 12 | `cos.lamp` | DESK LAMP | `COSMETIC_DEN` | Packet Pup |
| 13 | `cos.rug` | STATIC RUG | `COSMETIC_DEN` | Packet Pup |
| 14 | `cos.poster` | FARADAY POSTER | `COSMETIC_DEN` | Packet Pup |
| 15 | `cos.fern` | SERVER FERN | `COSMETIC_DEN` | Beacon Beast |
| 16 | `cos.lantern` | BEACON LAMP | `COSMETIC_DEN` | Beacon Beast |
| 17 | `cos.confetti` | PACKET FLURRY | `COSMETIC_FLOURISH` | Gremlin |
| 18 | `cos.sparks` | SOLDER SPARKS | `COSMETIC_FLOURISH` | Gremlin |
| 19 | `cos.aurora` | NOISE AURORA | `COSMETIC_FLOURISH` | Sentinel |

Ids 20-23 are unused and must stay that way until something appends to
`ITEM_DEFS`.

**For the den specifically:** `PetState::denSlots[]` holds item ids, `0` meaning
empty. Item id 0 is `mat.scrap`, a real item, so the den cannot store scrap in a
slot and must treat `0` as empty exactly as the schema comment says. Filter
placeable items with `definition(id)->slot == COSMETIC_DEN`; there are five
today. Do not place a `COSMETIC_WORN` or `COSMETIC_FLOURISH` item in a den slot,
and do not read `ItemDef` for anything other than presentation - it carries no
number an engine could treat as a modifier, deliberately.

### Recipe ids

`craft.antenna`, `craft.scarf`, `craft.lamp`, `craft.rug`, `craft.poster`,
`craft.goggles`, `craft.collar`, `craft.fern`, `craft.lantern`,
`craft.confetti`, `craft.sparks`, `craft.aurora`.

Recipe ids are strings, not indices, and the table IS pack-replaceable, so never
persist a recipe index. `Inventory::recipeIndexById()` resolves one on demand.

## Boot wiring

`Inventory::begin()` loads the baseline recipe table and then an optional
`/content/recipes.json` pack. It must run before anything asks for a recipe, and
after `PetCore::instance().init()` because a recipe's stage gate reads the pet's
stage. Beside the other content engines in `setup()`:

```cpp
ContentStore::instance().begin();
Inventory::instance().begin();          // add this
QuestEngine::instance().begin(PetCore::instance().questDaySeed());
```

The screen is a singleton and needs the TFT pointer before anything can draw,
exactly like `UIMissions` and `UIQuests`:

```cpp
UIInventory::instance().init(tft);
```

If `Inventory::begin()` never runs, the screen still draws: it shows `BACK`, the
note row and no recipes. That is a content problem, not a crash.

## Menu entry

`ui_menu.*` is not owned by this workstream, so `MenuItem` has no `MENU_KIT`
entry yet. The menu owner adds one, gives it a label, puts it in `MENU_ORDER`,
and `main.cpp` switches on it:

```cpp
case MENU_KIT:
    switchScreen(SCREEN_INVENTORY);
    UIInventory::instance().open();
    break;
```

**Do not gate the entry on owning something.** A new pet owns nothing, and the
empty screen is where a player learns what the materials are for. Hiding it
until the inventory is non-empty means it appears out of nowhere later, and the
first thing you would ever have seen is the thing you no longer need.

## Button handling

Same one-button convention as `SCREEN_MISSIONS` and `SCREEN_QUESTS`: short press
moves the cursor, long press acts on the highlighted row, row 0 is `BACK`.

```cpp
// short press
UIInventory::instance().scrollDown();

// long press
if (UIInventory::instance().backSelected()) {
    switchScreen(SCREEN_MENU);
    UIMenu::instance().open();
} else {
    switch (UIInventory::instance().craftSelected()) {
        case CRAFT_OK:
            // Crafted. The screen has already redrawn itself.
            // A results flourish here is optional and is P1-W3's call.
            break;
        case CRAFT_NO_SUCH_RECIPE:
            // The cursor is on a material or the note row. Nothing to do.
            break;
        default:
            // CRAFT_SHORT / CRAFT_LOCKED / CRAFT_ALREADY_OWNED. The footer is
            // already saying which, so a toast is optional.
            break;
    }
}
```

`craftSelected()` spends nothing and draws nothing on failure, so the caller
never has to re-derive the rules. `actionHint()` returns the same reason the
footer is showing, and `selectedKind()` exposes the row kind if a caller wants to
branch before pressing.

## Awarding materials

Nothing in the tree awards a material yet. That is roam's and patrol's job
(P1-W4 / P1-W5). The engine's half of the contract:

```cpp
uint16_t landed = Inventory::instance().add(itemId, amount);
```

`add()` returns how many actually landed, saturating at the item's stack max. It
returns less than `amount` when the stack is full; report that rather than
pretending all of them fit. `ITEM_STACK_MAX` is 999 for a material. **A cosmetic
caps at 1**, because owning nine Antenna Hats is not a thing anybody wants, and
that is why `add()` and not a raw `items[id] += n` is the only supported path.

Both `add()` and `remove()` set `PetState::dirty`, so the existing debounced save
picks the change up with no extra call.

## Items are cosmetic. That is a rule, not a default.

`ItemDef` has no stat field, no XP field, no multiplier and no duration, and
`pet_inventory.cpp` calls no stat mutator. An item may unlock an idle animation,
something worn on the pet, or a den decoration - all selected by `slot` - and
nothing else.

Two things guard it. A `static_assert` against `ItemDefShapeProbe` fails the
build if `ItemDef` grows a field, and
`test_inventory.cpp::test_crafting_never_touches_a_stat` snapshots all seven
stats plus mastery XP across a craft and asserts they are unchanged. If you are
about to widen either, read the rule at the top of `src/content/item_defs.h`
first: the reason is that the moment an item pays a stat, the pet becomes a build
to optimise instead of a companion.

## Content packs

`/content/recipes.json` replaces the recipe table. Same shape and same
guarantees as the dialogue and quest packs:

```json
{ "recipes": [
  { "id": "craft.antenna", "output": "cos.antenna", "stage": 2,
    "inputs": [ { "item": "mat.copper", "qty": 3 },
                { "item": "mat.scrap",  "qty": 2 } ] }
] }
```

A bare `[ ... ]` is accepted too. `stage` is a `PetStage` number (1-5); anything
outside that means no gate. Cap is `RECIPE_MAX_DEFS` (16) rows and
`RECIPE_MAX_PACK_BYTES` (4096) bytes; a bigger file is refused whole.

**The item table is NOT pack-replaceable**, because item ids are indices into a
persisted array and a pack that renumbered them would silently rewrite every
player's inventory. Only recipes are overridable, and they name items by string
id, which is what lets an unknown item be rejected instead of resolved to slot 0.

A recipe is rejected, individually, when it has no id, an id at or over
`CONTENT_MAX_ID_LEN`, an unknown output, an output that is a material, zero
inputs, more than three inputs, an unknown input, a quantity of 0 or above
`ITEM_STACK_MAX`, an input equal to the output, or the same input twice. If a
pack yields zero valid recipes the baseline table is left completely untouched.
The baseline goes through exactly the same gate, and a test asserts all twelve
shipped recipes survive it.

## Art

`src/ui/item_art.h` is generated. Do not hand-edit it. Re-run:

```
python scripts/gen_item_icons.py
```

The generator draws each icon procedurally with Pillow at 128 px and downscales
with LANCZOS, which is what gives the icons the same soft neon edge as the HD
pet art. It reads the accent colours out of `src/content/item_defs.h` rather than
keeping a second copy, so a colour cannot drift between the table and the icon;
if that parse ever stops matching the table the script exits instead of baking
the wrong palette.

This is a separate pipeline from the pet art. `scripts/gen_hd_sprites.py` and
`src/ui/sprites_hd_*.h` were not touched and must not be folded into this.

Two sizes are baked and exactly one is compiled, chosen by panel family:

| Panel | Icon | Flash |
|-------|------|-------|
| 160x80 | 8x8 | 2,560 bytes |
| 320x172 and 240x240 round | 16x16 | 10,240 bytes |

## Panel families

All three are handled and were verified by rendering into the SDL simulator on
each, in both the populated and the empty state.

| Family | Layout |
|--------|--------|
| 160x80 | Size-1 rows, 13 px pitch, 4 visible, 8 px icons. Compact header and footer. |
| 320x172 (`SCREEN_H > 100`) | Size-2 rows, 22 px pitch, 5 visible, 16 px icons, via `uiBigHeader` / `uiBigFooter`. |
| 240x240 (`HEXHOUND_PANEL_ROUND`) | Centred rows with chord-clipped selection capsules, via `uiround::*`. Each row is two size-1 lines: the icon and the name centred as one block, then the detail. |

Every position derives from `SCREEN_W` / `SCREEN_H`, and the round path derives
its horizontal extents from `uiround::chordHalfW()` per row, so nothing is drawn
under the bezel.

Item names are clipped with a visible `..` when they do not fit, same rule as the
quest list. The names in `ITEM_DEFS` are all 14 characters or fewer and fit
untruncated on every panel; the truncation exists for a content pack, not for the
baseline.

## Row model

The list is flat because the button is flat, but the rows are not all the same
thing:

```
row 0             BACK
materials         one row per material the pet actually holds,
                  or exactly ONE note row when it holds none
recipes           EVERY recipe, ordered craftable, then short,
                  then already owned, then locked by stage
```

The ordering is computed by `open()` and then frozen while the screen is open. A
list that resorted itself after every craft would move the row you were about to
press.

Recipes are never hidden, including locked ones. A locked recipe is the only way
a player learns that a Beacon Lamp exists to work towards.

The right-hand column is `x12` for a material, and for a recipe: `MAKE` (green,
craftable), `1/2` (amber, that many of its ingredients are ready), `HAVE` (grey,
already owned), or a three-letter stage tag (`PUP` / `BST` / `GRM` / `SNT`, dark
grey, locked).

## Empty is a first-class state

A new pet owns nothing, so this is the state most people see first, and it is
built rather than tolerated:

* The materials section collapses to one honest note row (`No parts yet` on the
  160x80, `No parts yet - go roaming` where there is room). It does not vanish,
  so the list geometry is identical whether the pet is carrying everything or
  nothing.
* Every recipe is still listed with its stage gate, so the screen reads as "here
  is what you are working towards".
* The footer reads `nothing carried` rather than `0/0`.
* `actionHint()` on the note row returns `go roam`.

`Inventory::isEmpty()`, `distinctMaterials()` and `ownedCosmetics()` exist so the
screen and the engine agree on what "empty" means instead of each deriving it.

## Cost

Measured on `lilygo-t-dongle-s3-vendor-app`, the no-PSRAM design target, with a
clean build each time.

As the tree stands nothing references any of this, so the linker drops almost
all of it and RAM is **unchanged at 78,916 bytes**, with flash at 1,167,337
against a 1,165,941 baseline - 1,396 bytes more purely from retained sections in
the two new object files. That is NOT the cost of this work and must not be
quoted as it.

The real cost was measured with a link probe: a temporary block in `setup()`
behind a `static volatile bool` the compiler cannot fold, calling into every
public entry point so nothing can be garbage-collected. Three builds, each
differing from the last by exactly one thing:

| Build | RAM | Flash |
|-------|-----|-------|
| Baseline, P1-W2 files absent | 78,916 | 1,165,941 |
| Probe reaching `Inventory` only | 79,524 | 1,172,821 |
| Probe reaching `Inventory` + `UIInventory` | 79,556 | 1,177,905 |

* **The engine costs 608 bytes of RAM and 6,880 bytes of flash.** The RAM is
  entirely the recipe table. `xtensa-esp32s3-elf-nm` confirms it independently:
  the `Inventory` singleton is 610 bytes (`0x262`). There is no other allocation
  and no heap use on any path; the item counts themselves live in `PetState`,
  which already existed. The recipe table lands in `.data` rather than `.bss`
  because `Recipe` default-initialises its item ids to `ITEM_ID_NONE` rather than
  0, which costs a second 610 bytes of flash for the initialiser and is worth it:
  item id 0 is `mat.scrap`, so a zero-initialised unused slot would describe a
  valid, free, zero-ingredient recipe for scrap if a bounds check ever slipped.
* **The screen costs 32 bytes of RAM and 5,084 bytes of flash.** The 32 bytes are
  exactly the singleton (`nm` agrees: 0x20), which is the TFT pointer, the cursor
  and scroll, and the 16-byte recipe ordering. The screen owns no framebuffer and
  no row cache; the largest allocation on the draw path is a 40-byte stack
  scratch for one truncated row. Of its flash, **2,560 bytes are the 8x8 icon
  set**; on a 320x172 or round board that line item is 10,240 instead.
* **Total once wired: RAM 78,916 -> 79,556 (+640, 24.1% -> 24.3%), flash
  1,165,941 -> 1,177,905 (+11,964).**

The probe was deleted after measurement; it is not in the tree, and `git status`
shows `src/main.cpp` unmodified.

## Verified

| Environment | Result |
|-------------|--------|
| `lilygo-t-dongle-s3-vendor-app` | SUCCESS, RAM 78,916 / flash 1,167,337 |
| `waveshare-esp32-s3-lcd-128` (round) | SUCCESS, RAM 76,784 / flash 1,500,921 |
| `desktop-sim` (160x80) | SUCCESS |
| `desktop-sim-wave` (320x172) | SUCCESS |
| `desktop-sim-round` (240x240) | SUCCESS |
| `pio test -e native` | 7 suites PASSED (the 6 that existed, plus `test_inventory`: 30 tests, 0 failed) |

CI runs `pio test -e native`, which discovers suites from `test/`, so the new
suite needs no workflow change.

Rendering was checked on all three panel families by driving the screen through
the simulator's headless capture path, with the same temporary probe adding an
`inventory` case to `HEXHOUND_SIM_FORCE_SCREEN`: a populated inventory, the
empty state on a fresh egg (note row plus locked recipes), a state where two
recipes are craftable (both sorted to the top, both showing a green `MAKE`), and
the scrolled window on the 160x80 where the list does not fit in four rows.

## Known follow-up

`readRecipePack()` in `pet_inventory.cpp` is a near-copy of `readPack()` in
`content_store.cpp`. That reader is file-static and `content_store.cpp` is owned
by another workstream, so duplicating about thirty lines of bounded file IO was
the lesser evil against editing a file being changed in parallel. Consolidating
both onto one shared helper is worth doing once P1 lands; it is not urgent,
because both copies enforce the same size cap and both refuse an oversized pack
whole.
