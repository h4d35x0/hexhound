# P1-W1 Behavioural Forms - Integrator Wiring

The pet now has a SECOND identity axis. `PetStage` is age and is bought with XP.
`PetForm` is character and is earned by what the owner actually does. They are
independent: a Legendary Pathfinder and a Legendary Cipher are the same age and
nothing alike, which is the point.

Nothing routes to any of this yet. The module compiles on every firmware and
simulator target and the linker garbage-collects all of it, so the shipped image
is unchanged apart from the dialogue filter (measured below). This file is the
handover.

The files that must be edited to finish the wiring (`src/main.cpp`,
`src/config.h`, `src/ui/ui_menu.*`, `src/pet/pet_core.*`, `src/games/`,
`src/modules/`) are owned by other work, so W1 did not touch any of them.
`PetForm`, `FORM_MIN_EVIDENCE`, `FORM_LEAD_PERCENT`, `PetState::form` and
`PetState::behaviour[]` already existed from P1-W0.

## Files added

| File | Purpose |
|------|---------|
| `src/pet/pet_forms.h` / `.cpp` | Evidence recorders, form selection, title, accent, badge index. |
| `src/ui/form_art.h` | GENERATED. Six 20x20 RGB565 form badges. |
| `scripts/gen_form_badges.py` | Draws the badge PNGs and bakes `form_art.h`. |
| `assets/forms/badge_*.png` | Committed output of that script. |
| `test/test_pet_forms/test_pet_forms.cpp` | 25 tests: floor, lead bar, near-tie, saturation, age isolation, presentation. |

## Files modified

| File | Change |
|------|--------|
| `src/content/content_types.h` | `DialogueLine::form`, plus `DIALOGUE_FORM_UNSET`. |
| `src/content/dialogue_engine.h` / `.cpp` | Third filter in `scoreLine()`; optional `form` argument on `pick()` and `hasLineFor()`. |
| `src/content/content_store.cpp` | `form` parsed from pack JSON, by name or ordinal. |
| `src/content/content_baseline.h` | `form` column on `BaselineDialogue`; 8 form-flavoured lines added (24 -> 32 of a 40 cap). |
| `test/test_content/test_content.cpp` | 5 tests for the form filter and its parsing. |

`scripts/gen_hd_sprites.py` and `src/ui/sprites_hd_*.h` are the STAGE art and
were not touched. Stage and form are independent axes and their art pipelines
stay independent.

## What form is, and is not

* Form is DISPLAY ONLY. A title, an accent colour, a small badge, and a
  different sentence now and then. There is deliberately no stat multiplier, no
  unlock, and no reward anywhere in `pet_forms.h`. The moment a form changes a
  number, the pet stops being a companion and becomes a build to optimise, and
  the correct way to play becomes farming a counter.
* Form is DISCOVERABLE, NOT ANNOUNCED. Do not add a progress bar toward a form,
  a "next form" hint, or a percentage. `formTitle()` returns `""` while the form
  is `FORM_UNSET` precisely so a UI that prints it blind shows nothing rather
  than a goal to chase. The intended experience is someone noticing their pet
  has started talking about distance, not being told they are 62% Pathfinder.
* Evidence NEVER decays. The counters saturate at 0xFFFF and there is no decay
  entry point. An identity is earned across a pet's whole life; letting it rot
  would punish someone for a quiet fortnight.

## Boot wiring

`PetForms` is stateless and needs no `init()`. One call belongs in `setup()`,
after the save has loaded, so a pet whose save predates the form field settles
on the right answer immediately:

```cpp
PetForms::refresh(PetCore::instance().state());
```

That is safe on an old save: `PetState::form` defaults to `FORM_UNSET`, the
behaviour counters default to zero, and `evaluate()` answers `FORM_UNSET` below
the evidence floor. No migration is needed; P1-W0's schema v2 already carries
both fields.

## Recorder wiring (the rules layer)

Six recorders, all saturating, all marking the pet dirty. `weight` lets a caller
report a batch (twenty steps, four collectibles) in one call.

```cpp
PetForms::recordWalk(n);     // roam steps, roam points, patrol in a new place
PetForms::recordGuard(n);    // threat triage, trusted-device watch, defensive quests
PetForms::recordCollect(n);  // items gained, journal entries, first-time captures
PetForms::recordPuzzle(n);   // challenge answered, educational content completed
PetForms::recordPlay(n);     // minigame round finished, mission run
PetForms::recordSocial(n);   // encounter with another device or pet, co-op
```

Suggested call sites, all in `src/pet/pet_rules.cpp` unless noted:

| Event | Call |
|-------|------|
| `onWifiScanDone` / patrol completed | `recordWalk(1)` |
| New unique SSID or BLE address logged | `recordCollect(1)` |
| `onWifiOpenNetwork`, `onWifiDuplicate`, `onBleTrackerAlert` | `recordGuard(1)` |
| `onMissionComplete` | `recordPlay(1)` |
| Minigame round finished (beside `PetMemory::recordGameScore`) | `recordPlay(1)` |
| Roam step batch (P1 roam owner) | `recordWalk(steps)` |
| Item collected (P1 inventory owner) | `recordCollect(1)` |
| Encounter resolved (P1 encounter owner) | `recordSocial(1)` |

Keep the weights in the same order of magnitude across recorders. The selection
rule compares raw counters, so a recorder that fires per step against one that
fires per patrol would decide the form by accident of units rather than by what
the owner actually did.

## Dialogue wiring

`DialogueEngine::pick()` and `hasLineFor()` gained a trailing `form` argument
that defaults to `DIALOGUE_FORM_UNSET`. Every existing call site therefore
compiles unchanged and keeps its exact previous behaviour: form-tagged lines are
simply never eligible for it. To turn the flavour on, pass the pet's form:

```cpp
const PetState& pet = PetCore::instance().state();
const char* line = DialogueEngine::instance().pick(
    DLG_IDLE, pet.traits[0], pet.traits[1], pet.stage, pet.form);
```

Ranking is trait > form > stage. The trait is the pet's voice, the form is what
it has become, the stage is only how old it is.

Content packs tag a line with `"form": "pathfinder"` (or the ordinal). An
absent, misspelled or explicitly `"unset"` form all mean "suits any pet", never
"only a pet with no identity", so a typo cannot silently hide a line.

## UI wiring

```cpp
uint16_t accent = PetForms::formAccent(pet.form);            // RGB565, always drawable
const char* title = PetForms::formTitle(pet.stage, pet.form); // "" while unset
uint8_t idx = PetForms::formIndex(pet.form);                  // 0xFF while unset
```

`formTitle()` returns a pointer into a static 32-byte buffer, valid until the
next call. Same convention as `DialogueEngine::pick()`: draw it or copy it.

When `title[0] == '\0'`, draw the stage name the UI already had. Do not
substitute a placeholder.

The badge:

```cpp
#include "form_art.h"   // ONE .cpp only, same rule as sprites.h
...
if (idx < FORM_BADGE_COUNT) {
    drawSprite(tft, form_badge_by_index[idx], x, y,
               FORM_BADGE_SIZE, FORM_BADGE_SIZE);
}
```

Transparent pixels are the 0xF81F chroma key `drawSprite()` already skips, so
the badge composites over whatever is on screen with no mask. There is no badge
for `FORM_UNSET` on purpose.

`form_art.h` is included NOWHERE today. Including it costs 4824 bytes of flash
(see below) and every extra translation unit that includes it pays that again,
so pick one file.

## The selection rule, and why it is sticky

`evaluate(behaviour, current)`:

* Leader below `FORM_MIN_EVIDENCE` (25) -> `FORM_UNSET`.
* Leader not `FORM_LEAD_PERCENT` (140%) ahead of the runner-up -> keep
  `current`, which is `FORM_UNSET` until a form has actually been earned.
* Otherwise -> the leader.

Keeping `current` rather than reverting is load-bearing. A purely stateless
"leader wins, else unset" rule churns at the boundary: counters of 100 and 71
name a form, the runner-up ticks to 72 and the pet is suddenly nobody, and one
more tick names it again. Requiring a challenger to clear the same 140% bar
against the incumbent makes the round trip cost a 1.96x swing, so a near-tie
sits still. `test_near_tie_does_not_flip_flop_as_counters_tick` walks that exact
sequence.

The floor branch answers `FORM_UNSET` rather than keeping `current`, unlike the
lead branch. Because evidence never decays, a pet that once cleared the floor
cannot fall back under it during normal play, so the only way to reach that
branch holding a form is a save whose counters were lost. In that case the
evidence really is gone, and carrying the old label forward would be the
firmware asserting something it can no longer support.

## Regenerating the art

```
pip install Pillow
python scripts/gen_form_badges.py
```

Writes `assets/forms/badge_*.png` and `src/ui/form_art.h`. RGB565 is two bytes
per pixel with no compression, so each badge costs `2 * edge^2` bytes and there
are six: 20px is 4800 bytes, 64px would be 49152. Raising `BADGE_SIZE` is
quadratic, not free. The six accent colours in that script must stay in step
with `FORM_ACCENT` in `src/pet/pet_forms.cpp`, or the badge and the title beside
it will disagree.

## Measured cost, T-Dongle S3 (`lilygo-t-dongle-s3-vendor-app`)

| Build | RAM | Flash |
|-------|-----|-------|
| Baseline (fa5d9e6, before W1) | 78916 | 1165941 |
| W1 as committed, nothing routed to it | 78964 (+48) | 1166901 (+960) |
| W1 with a link probe forcing every symbol in | 79004 | 1172725 |

The middle row is what ships today. The +48 RAM and +960 flash are the dialogue
filter alone: one extra byte on `DialogueLine` across the 40-line table, plus
the 8 new baseline lines and the extra branch in `scoreLine()`. That cost is
paid whether or not anything is wired.

The bottom row is the real number to plan against. Subtracting the probe's own
190 bytes of code and 4 bytes of BSS, wiring W1 in full will cost about
**+6600 bytes of flash and +84 bytes of RAM against the pre-W1 baseline**.

Per-symbol attribution from `xtensa-esp32s3-elf-nm -S` on the probe image:

| Item | Bytes | Where |
|------|-------|-------|
| Six 20x20 badges | 4800 | flash (.rodata) |
| `form_badge_by_index` | 24 | flash |
| `PetForms` code (13 functions) | 532 | flash |
| `FORM_NAMES` / `STAGE_ADJECTIVE` / `FORM_ACCENT` tables | 56 | flash |
| Name and adjective string literals | ~101 | flash (.rodata.str, pooled) |
| `s_title` title buffer | 32 | RAM (.bss) |

The badge art is 89% of the flash cost. If a board ever needs W1 without the
badges, include `pet_forms.h` and not `form_art.h`: that is 588 bytes of flash
and 32 bytes of RAM for the title and accent alone.

To re-measure after wiring, a probe is not needed - the symbols will be live.
To re-measure before it, drop a `src/*.cpp` guarded by your own `-D` flag that
touches every entry point from an `__attribute__((constructor))` function
(`.init_array` is KEEP'd, so it survives `--gc-sections`), build with
`PLATFORMIO_BUILD_FLAGS=-D<your flag>`, and delete the file afterwards.

## Verification run

| Check | Result |
|-------|--------|
| `pio run -e lilygo-t-dongle-s3-vendor-app` | SUCCESS, RAM 78964, Flash 1166901 |
| `pio run -e waveshare-esp32-s3-lcd-147b` | SUCCESS, RAM 76156, Flash 1297273 |
| `pio run -e desktop-sim` (MinGW + SDL2) | SUCCESS |
| `pio test -e native` | 7 suites PASSED (6 pre-existing + `test_pet_forms`) |
| `test_pet_forms` | 25 passed, 0 failed |
| `test_content` | 33 passed, 0 failed (28 pre-existing, all still green) |

`checkEvolution()`, the `STAGE_*` enum and the XP thresholds were not touched,
so existing saves stay readable and the evolution cutscene is unaffected.
`test_form_never_moves_stage_or_xp` and `test_stage_and_form_are_independent_axes`
hold that line.
