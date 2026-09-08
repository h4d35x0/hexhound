# P1-W4 Roam Mode - Integrator Wiring

Roam Mode and its two screens exist and build on every target, but nothing
routes to them yet, so the linker garbage-collects the whole feature. This file
is the handover.

The files that must be edited to finish the wiring (`src/main.cpp`,
`src/ui/ui_menu.*`, `src/config.h`) are owned by other work, so W4 did not touch
them. Neither did it touch `src/pet/*`, `src/modules/imu_module.*` or
`src/games/*`.

## Files added

| File | Purpose |
|------|---------|
| `src/modules/roam_module.h` | Tuning block, `StepDetector`, `roam::` churn maths, `RoamReport`, `RoamModule`. |
| `src/modules/roam_module.cpp` | Session state machine, radio sampling, battery bound, materials, commit to `PetState`. |
| `src/ui/ui_roam.h` / `.cpp` | `SCREEN_ROAM` (live) and `SCREEN_ROAM_REPORT`, all three panel families. |
| `test/test_roam/test_roam.cpp` | 18 tests: step detector against four synthetic streams, exploration maths, the separation rule. |

No existing file was modified. `build_src_filter` is `+<*>` on every firmware
and simulator environment, so the new translation units are picked up with no
`platformio.ini` change.

## The central rule: two earners, never one

This is the design constraint the whole workstream is built around, and it is
worth restating here because the wiring is where it would get broken.

| | Exploration points | Steps |
|---|---|---|
| Field | `PetState::roamPoints` | `PetState::stepCount` |
| Boards | **every** board | boards where `Caps::canMeasureMotion()` is true, i.e. the Waveshare 1.28 round board and nothing else today |
| Source | how much the radio environment CHANGED between samples, plus how much of it the pet had never seen | a peak detector over accelerometer magnitude |
| Nature | a derived heuristic | a physical measurement |

**Exploration points must never be labelled steps, distance, metres, paces, or
ground covered** - not on a screen, not in a journal entry, not in a quest
description, not in a dialogue line. A bus ride past forty access points earns
more of them than a mile walked down an empty lane. The number is useful; the
claim would be false.

**`stepCount` must never be estimated.** On a board with no IMU it stays exactly
0 forever. `RoamModule::pollSteps()` is the only caller of the detector anywhere
in the firmware and its body is behind `Caps::canMeasureMotion()`, so on the
T-Dongle S3 the function does not survive to the ELF at all (verified with
`nm`: no `RoamModule::pollSteps`, no `roamIsqrt`, no detector code). There is no
path that could put a number there.

A screen deciding whether to show a step row must gate on
`RoamModule::stepsMeasured()` (a `constexpr` capability answer), **not** on
`steps() > 0`. A board with an IMU that has not moved yet should say `0`; a
board that will never know should say nothing at all. `RoamReport` carries
`stepsMeasured` for the same reason, so a report can be rendered away from the
module and still tell the two cases apart.

## Boot wiring

Both the module and the screen are singletons. Add beside the other `init()`
calls in `setup()`:

```cpp
RoamModule::instance().init();
UIRoam::instance().init(tft);
```

`RoamModule::update()` **must run after `IMUModule::update()`** in `loop()`. It
reads the sample the IMU module already fetched rather than touching I2C
itself, so roaming adds no bus traffic, but it does mean the ordering matters.
On boards with no IMU the ordering is irrelevant because the read is compiled
out.

## Menu entry and screen routing

```cpp
case MENU_ROAM:
    if (RoamModule::instance().begin(pet.stage >= STAGE_BEACON_BEAST)) {
        switchScreen(SCREEN_ROAM);
        UIRoam::instance().open();
    } else {
        // Refused: the board can measure charge and there is not enough of it.
        // report().endReason is ROAM_END_REFUSED. Show a toast; do not retry.
    }
    break;
```

The `begin()` argument is whether to fold BLE into the sampling rotation. It is
the caller's decision for the same reason patrol's is: BLE is stage-gated
content and this module does not reach into the stage rules to second-guess it.
The expression above matches how patrol gates its BLE scan today.

### While `SCREEN_ROAM` is up

```cpp
// every loop
RoamModule::instance().update();
UIRoam::instance().draw();          // cheap when nothing changed

// the expedition can end itself
if (!RoamModule::instance().active()) {
    UIRoam::instance().drawReport(RoamModule::instance().report());
    switchScreen(SCREEN_ROAM_REPORT);
}

// long press = stop it here
const RoamReport& r = RoamModule::instance().end(ROAM_END_USER);
UIRoam::instance().drawReport(r);
switchScreen(SCREEN_ROAM_REPORT);
```

`draw()` is safe to call every frame. It repaints only fields whose value
actually changed, deadbands elapsed time to whole seconds and progress to whole
percent, and prints every numeric field at a fixed width so a shrinking number
cannot leave a stale digit. That is not polish: the battery-noise incident in
this repo was a screen repainting continuously because a noisy analogue reading
moved by one unit, and the live roam screen shows a battery-derived progress bar.

`end()` is idempotent-ish: calling it on an idle module returns the last report
and changes nothing, so a double-fire on the button is harmless.

### `SCREEN_ROAM_REPORT`

Static. Draw once on entry with `UIRoam::instance().drawReport(r)`; any button
returns to `SCREEN_MENU`.

## What `end()` already did

By the time you have the report, `RoamModule::end()` has already committed to
`PetState`:

```
roamPoints   += report.pointsEarned
stepCount    += report.stepsTaken     // only when report.stepsMeasured
roamSessions += 1
dirty         = true
```

It has **not** touched anything else. In particular:

* **No XP, food, mood or trust.** Rewards belong to the rules layer per
  `docs/w2-wiring.md`, and roam should not be the one exception.
* **No behaviour counter.** Roam is the natural evidence source for
  `FORM_PATHFINDER`, but the form owner also credits travel-ish events, and two
  independent writers to `behaviour[FORM_PATHFINDER - 1]` would double count. The
  form owner should credit it from the report, once, at a rate they choose.
* **No journal entry.** If you write one, the naming rule above applies to it.
  "Explored 1284 pts across 37 samples" is fine. "Walked 1284" is not.
* **No inventory write.** See below.

## Materials

`RoamReport::finds[]` is a small fixed list of `{material, count}`. **The ids
are roam-local `RoamMaterial` values, deliberately NOT inventory item ids.** The
inventory work is happening in parallel and this module does not include its
header, so numbering against it would mean depending on something that does not
exist yet.

The integrator owns exactly one translation table:

```cpp
static const uint8_t ROAM_MATERIAL_TO_ITEM[ROAM_MAT_COUNT] = {
    0,                    // ROAM_MAT_NONE
    ITEM_SIGNAL_DUST,     // ROAM_MAT_SIGNAL_DUST
    ITEM_DRIFT_GLASS,     // ROAM_MAT_DRIFT_GLASS
    ITEM_BEACON_SHARD,    // ROAM_MAT_BEACON_SHARD
    ITEM_PACED_ALLOY,     // ROAM_MAT_PACED_ALLOY
};

for (int i = 0; i < r.findCount; i++) {
    inventoryAdd(ROAM_MATERIAL_TO_ITEM[r.finds[i].material], r.finds[i].count);
}
```

Targets must stay below `ITEM_TYPE_COUNT` (24) and above 0, because 0 is the
"empty" sentinel `denSlots` already uses.

Yields are deterministic, no RNG - what an expedition brought back should be
explainable from what it did:

| Material | Earned from | Rate |
|---|---|---|
| `ROAM_MAT_SIGNAL_DUST` | exploration points | 1 per 25 |
| `ROAM_MAT_DRIFT_GLASS` | identifiers never seen before | 1 per 4 |
| `ROAM_MAT_BEACON_SHARD` | BLE identifiers never seen before | 1 per 3 |
| `ROAM_MAT_PACED_ALLOY` | **real steps** | 1 per 500 |

`ROAM_MAT_PACED_ALLOY` can only ever be produced when `stepsMeasured` is true,
which makes it a material that means something on the one board that can earn
it. That is intentional and is worth preserving if the crafting recipes are
designed around it.

## How exploration points are actually earned

Every `ROAM_SAMPLE_INTERVAL_MS` (20 s) the module takes one radio sample. One
sample in every three listens on BLE instead of Wi-Fi when `includeBle` is set;
they are sequential, never concurrent, because inviting a Wi-Fi/NimBLE
coexistence problem into a background feature on a 2 MB-PSRAM board is not worth
the sample rate.

Each sample is reduced to at most 24 16-bit hashes. Wi-Fi is fingerprinted by
**BSSID**, not SSID: two access points sharing an ESSID are two different
places, and a campus-wide SSID would otherwise make a whole building look like
one room. Lifetime novelty is a separate question and uses the pet's own
capture sets, which are keyed by SSID (`markWifiSeen`) and BLE address
(`markBleSeen`).

Points per sample:

```
churn  = (appeared + vanished) / union, as a percentage
if churn >= 25%   ->  2 points per changed identifier, capped at 12 changes
plus              ->  5 points per identifier never seen in the pet's life,
                      capped at 8
total capped at 60 per sample
```

The 25% floor is the **"sat in the same room for an hour" defence** and is the
single most important number in the file. A stationary device in a busy
building watches two or three access points at the edge of its sensitivity
flicker in and out forever; without a floor that flicker is a points fountain
for a device that has not moved an inch. There is a test pinning exactly this
case (`test_same_room_flicker_earns_nothing`).

The first sample of a band in a session earns no churn, because there is
nothing to compare it against yet. Lifetime novelty still applies to it.

## Battery

Where `Caps::has(CAP_BATTERY)`:

* Roam refuses to start at or below 15% (`begin()` returns false,
  `report().endReason == ROAM_END_REFUSED`).
* The expedition budget is one minute per point of charge above 15%, capped at
  45 minutes. At 80% that is the 45-minute cap; at 20% it is 5 minutes.
* If charge falls to 15% mid-expedition it ends itself with
  `ROAM_END_BATTERY`.

Where the board cannot measure charge, **the expedition is simply not bounded**.
No ceiling is invented for it, `plannedMs()` returns 0, `progressPct()` returns
-1, and the progress bar / rim ring is not drawn at all. Guessing "about forty
minutes" for hardware with no fuel gauge would be a fabricated number on a
screen, and the owner ending the roam is a perfectly good stopping rule.

One trap worth knowing about: **`BatteryModule::percent()` returns 0 until the
first reading lands**, and 0 is indistinguishable from flat. `batteryBounded()`
therefore requires `isAvailable() && hasReading()` as well as the capability,
otherwise every roam started in the first two seconds after boot would be
refused for a low battery that was never measured.

## Tuning constants a human must calibrate after walk-testing

All of these live in `namespace roamtune` at the top of
`src/modules/roam_module.h`, with per-constant rationale. **Every step-detector
value is a bench estimate, not a measured one.** Walk a counted route (200 steps
is enough), compare, and move these in this order:

| Constant | Now | Symptom it fixes | Direction |
|---|---|---|---|
| `STEP_THRESHOLD_NUM` / `_DEN` | 5/4 (1.25x) | **Move this first.** Undercounts a normal walk -> lower toward 1.0. Counts while carrying the board in a hand without walking -> raise toward 1.5. | It sets the threshold as a multiple of the observed noise floor and is the single most sensitive dial. |
| `STEP_THRESHOLD_MIN_MG` | 120 | Counts anything at all while the board sits on a desk -> raise. Misses a very light walk with the board on a lanyard -> lower, but not below the resting noise, which on the QMI8658 at +/-8g is roughly 10-20 mg. | This, not the ratio above, is what protects the still-device case. |
| `STEP_SHAKE_MG` | 1400 | Shaking the pet to wake it also adds steps -> lower. A brisk stride is being swallowed as a shake -> raise. | Deviation above this arms a 1.5 s lockout instead of counting. |
| `STEP_REFRACTORY_MS` | 250 | A slow deliberate walk double-counts -> raise. A jog undercounts -> lower. | Measured edge-to-edge, see below. |
| `STEP_THRESHOLD_MAX_MG` | 900 | Steps stop registering in a bag or a bouncing rucksack -> raise. | Ceiling on the dynamic threshold. |
| `STEP_ENV_DIV` / `STEP_NOISE_DIV` | 16 / 32 | The count takes many seconds to start after setting off -> lower (faster followers). The count reacts to a single loud sample -> raise. | EMA rates. |
| `STEP_CADENCE_MIN` | 2 | Isolated knocks are counted -> raise to 3 (costs one more real step at the start of every walk). | Peaks that must arrive in cadence before any count. |
| `ROAM_CHURN_FLOOR_PCT` | 25 | Points accumulate while sitting still -> raise. A genuine walk through a quiet area earns nothing -> lower. | The "same room" defence. |
| `ROAM_SAMPLE_INTERVAL_MS` | 20000 | Roam is eating the battery -> raise. | The main power dial: every sample is a radio scan. |

Two behaviours that look like tuning bugs but are not:

* **The refractory window is measured edge-to-edge, not step-to-step.** The
  obvious implementation measures from the last *accepted* step, and it has a
  hole: a 5 Hz vibration (a rough car ride, a hand drumming on a desk) puts a
  threshold crossing every 200 ms, every other one clears a 250 ms window
  measured from the last accepted step, and the vibration gets counted at a
  perfectly plausible 2.5 steps per second indefinitely. Rejecting any crossing
  that follows another crossing too closely closes that alias and still keeps
  real footfalls, because a footfall's own ringing only pushes the edge clock
  forward by tens of milliseconds. This is pinned by
  `test_refractory_period_rejects_impossible_cadence`; do not "simplify" it back.
* **A run's opening peak is paid retroactively.** `STEP_CADENCE_MIN` peaks must
  arrive in cadence before any of them count, then the opener is paid along with
  the one that qualified the run, so N in-cadence peaks yield N steps rather than
  N-1. An isolated bump yields 0.

### What the synthetic tests can and cannot prove

Nobody can validate a pedometer from a desk, and these tests do not claim to.
What they prove is that the detector rejects things that are obviously not
steps (a still device over 30 s, a device being shaken for 10 s, a single knock,
an impossible cadence) and accepts things that are obviously a walk. The
synthetic walk is a sine, which has a *lower* crest factor than real gait, so it
is a pessimistic stimulus for threshold crossing and an optimistic one for
regularity.

`pio test -e native -f test_roam` prints a calibration table alongside the
assertions. As committed:

```
walk 2.0Hz 350mg upright   truth  40  counted  38  (-2)
walk 1.2Hz 260mg upright   truth  24  counted  23  (-1)
walk 2.6Hz 420mg upright   truth  52  counted  50  (-2)
walk 2.0Hz 240mg pocket    truth  40  counted  38  (-2)
walk 1.2Hz 180mg pocket    truth  24  counted  23  (-1)
```

The constant one-to-two step shortfall is the 600 ms warmup at the start of each
session, which is expected and is not worth removing.

## Panel families

All three are handled and were verified by rendering both screens into the SDL
simulator on each, through a temporary capture path that was reverted
afterwards.

| Family | Live screen | Report |
|---|---|---|
| 160x80 | Size-1 rows at 11 px pitch, fixed-width numeric fields, progress bar above the footer rule. | Size-1 rows at 9 px pitch; materials pack **two to a line** with short names (`Dust x51  Glass x5`) because the panel is 26 columns. |
| 320x172 (`SCREEN_H > 100`) | Size-2 rows at 22 px pitch via `uiBigHeader` / `uiBigFooter`. | Size-2 rows at **18 px** pitch, one material per line with full names. |
| 240x240 (`HEXHOUND_PANEL_ROUND`) | Hero exploration number at size 3, steps at size 2, rim `progressRing` when bounded. Values are drawn centered at natural width over a chord-clipped `rowFill` erase. | Vertically centered block, size 2, 22 px pitch, up to six rows. |

Two layout notes that were found by rendering and are easy to reintroduce:

* The 320x172 report pitch is 18 rather than the usual 20. The worst-case report
  is exploration + steps + all four materials, and at 20 the last material fell
  past the footer rule and was **silently dropped**.
* The round screen does not pad numeric fields. Padding a *centered* field pushes
  the digits off the optical centre and they visibly drift as the count grows,
  so the row band is erased with `uiround::rowFill` first. The rectangular panels
  keep the padding, because there the fields are left-aligned and padding is
  what stops a stale digit.

## Cost

Measured on both validated boards. Baseline is `fa5d9e6` with the four new
source files removed from the tree.

Nothing references roam yet, so the linker drops almost all of it and the plain
build barely moves. **That number is not the cost of this work.** The real
figure came from a link probe: a temporary translation unit whose global
constructor touches every entry point, so `.init_array` keeps them alive. The
probe was deleted after measurement and is not in the tree.

### `lilygo-t-dongle-s3-vendor-app` (no IMU, no PSRAM, the design target)

| Build | RAM | Flash |
|---|---|---|
| Baseline, W4 files absent | 78,916 | 1,165,941 |
| W4 files present, unrouted | 78,916 | 1,166,261 |
| Probe holding roam alive | **79,220** | **1,169,609** |

**Once wired: +304 bytes RAM (24.1% -> 24.2%), +3,668 bytes flash.**

Of the 320 flash bytes in the middle row, none is executable code; it is
retained metadata, same as the W5 measurement noted.

### `waveshare-esp32-s3-lcd-128` (round, the only board with an IMU)

| Build | RAM | Flash |
|---|---|---|
| Baseline, W4 files absent | 76,784 | 1,499,549 |
| W4 files present, unrouted | 76,784 | 1,499,941 |
| Probe holding roam alive | **77,072** | **1,503,529** |

**Once wired: +288 bytes RAM (23.4% -> 23.5%), +3,980 bytes flash.**

### Where it goes

`xtensa-esp32s3-elf-nm -S` on the probe builds, identical on both boards:

| Symbol | Bytes |
|---|---|
| `RoamModule::instance()::mod` | 264 |
| `UIRoam::instance()::ui` | 28 |
| **Total module + screen state** | **292** |

The 264 bytes are three fingerprint buffers (`2 x 24 + 24` 16-bit hashes = 144),
the `RoamReport` including its six find slots, the `StepDetector`, and the
session timers. Nothing else: **there is no allocation anywhere in the module**,
and the largest stack frame on any polling path is the 8-entry `BLEResult` drain
batch (~384 bytes), which is deliberately a batch rather than the full
`MAX_BLE_DEVICES` array because that would put 1.5 KB on the main loop stack.

The 40 bytes of `StepDetector` inside that 264 are carried on boards that will
never use it. That is a deliberate trade: `capabilities.h` asks gameplay code to
use `Caps` rather than `#if`, and 40 bytes of 320 KB is not worth putting
preprocessor into a class definition to reclaim.

The flash difference between the two boards is the step detector itself. On the
round board `RoamModule::pollSteps` is 372 bytes of inlined detector; on the
T-Dongle it does not exist in the ELF at all, and neither does `roamIsqrt`. That
is the structural version of the guarantee in the section at the top of this
file: `stepCount` cannot move on a board without an IMU because the code that
could move it is not there.

## Verified

| Environment | Result |
|---|---|
| `lilygo-t-dongle-s3-vendor-app` | SUCCESS, RAM 78,916 / flash 1,166,261 |
| `waveshare-esp32-s3-lcd-128` | SUCCESS, RAM 76,784 / flash 1,499,941 |
| `desktop-sim` (160x80) | SUCCESS, both screens rendered |
| `desktop-sim-wave` (320x172) | SUCCESS, both screens rendered |
| `desktop-sim-round` (240x240) | SUCCESS, both screens rendered |
| `pio test -e native` | 7 suites PASSED (6 existing + `test_roam`, 18 assertions) |

Screens were rendered on every family in five states: expedition just started,
expedition after its first radio sample, report with steps, report without
steps, and an empty report with no materials.
