# P1-W6 - Field report screen (SCREEN_REPORT) wiring notes

New files, and the only files this workstream added:

- `src/ui/ui_report.h`
- `src/ui/ui_report.cpp`

Nothing else in the tree was changed. `src/main.cpp`, `src/config.h`,
`src/ui/ui_menu.*`, `src/pet/*`, `src/content/*`, `src/games/` and
`src/modules/` are all untouched, so the screen is inert until the integrator
routes it. `build_src_filter` is `+<*>` on every board env, so the translation
unit already compiles everywhere; the linker discards it while nothing
references it.

---

## 1. What the integrator has to add

### 1.1 Include and init

```cpp
#include "ui/ui_report.h"
```

In `setup()`, next to the other UI singletons:

```cpp
UIReport::instance().init(tft);
```

### 1.2 Entering the screen

From wherever the report is offered (a menu row, a post-patrol prompt, a
"the pet wants to show you something" nudge):

```cpp
switchScreen(SCREEN_REPORT);
UIReport::instance().open();
```

`open()` rebuilds the body from current pet state and resets the scroll. Call
it every time the screen is entered; the pet's numbers move between visits.

### 1.3 Short press - scroll

```cpp
case SCREEN_REPORT:
    UIReport::instance().scrollDown();
    break;
```

`scrollDown()` advances one line and wraps to the top after the last page. If
the whole report already fits, it does nothing (no redraw, no flicker).

### 1.4 Long press - exit, and the ONE place lastReportDay moves

```cpp
case SCREEN_REPORT:
    UIReport::instance().markReported();   // consume the report
    switchScreen(SCREEN_MENU);
    UIMenu::instance().open();
    break;
```

`markReported()` sets `PetState::lastReportDay = PetState::questDay` and marks
the pet dirty, and it is the only thing in this workstream that writes pet
state.

**It must not be called from a draw path.** `draw()` runs on every scroll and
on every repaint; a draw that moved `lastReportDay` would reset the span each
time the screen refreshed, and the pet would permanently believe zero days had
passed. Call it once, on the deliberate exit.

Whether an abandoned report (screen timeout, an alert taking the screen, a
patrol starting) should also count as consumed is an integrator decision. The
conservative choice is no: leaving `lastReportDay` alone means the next report
covers a longer span, which understates nothing.

### 1.5 Optional - the simulator force-screen hook

A temporary hook was used to capture the screenshots below and then reverted,
because `src/main.cpp` is off limits to this workstream. The integrator should
add the permanent version alongside the existing `quests` / `games` entries in
the `HEXHOUND_SIM_FORCE_SCREEN` block:

```cpp
} else if (!strcmp(fs, "report")) {
    switchScreen(SCREEN_REPORT); UIReport::instance().open();
}
```

---

## 2. What this screen will and will not claim

These are not style preferences. Each one is a claim the hardware either can or
cannot substantiate, and the screen is written to only make the ones it can.

### 2.1 There is no RTC, so there is no week

The only elapsed quantity on this screen is
`PetState::questDay - PetState::lastReportDay`, rendered as "days since last
report" (or "1 day since last report", or "Same day as last report", or "First
field report"). `questDay` is Phase 0's approximation: it advances on power-on
and after 24h of powered ticks. That is enough to say "days since we last did
this" and nowhere near enough to say "this week", so the words "week", "7
days", and any calendar term appear nowhere in the file. No second time
approximation was invented.

### 2.2 The counts are lifetime totals, and are never dressed as deltas

`PetState` keeps running totals (`wifiScans`, `seenWifiCount`, the three threat
counters, `questsCompleted`, `roamPoints`, `stepCount`, `roamSessions`,
`bestGameScore`) and keeps **no snapshot of what those totals were at the
previous report**. Without that snapshot a per-span delta cannot be computed.

A RAM-only baseline captured at `markReported()` was considered and rejected:
it would read as a true delta right up until the first reboot silently turned
it back into a lifetime total, and a number that is sometimes a delta and
sometimes not is worse than one that is honestly always a total.

So the body is presented as totals and the header carries the span. **If real
deltas are wanted later, that is a `PetState` change, not a UI change:** add a
`reportBaseline` snapshot (roughly 20 bytes: the same counters at the moment of
the last report) under a schema v3 bump, populate it in `markReported()`, and
this screen can then show "+6 since last report" honestly. Until that exists,
do not add delta wording here.

### 2.3 Steps and exploration are two different numbers

- `stepCount` is **real physical movement** and only ever advances on a board
  with an IMU. Rendered as "Steps walked", and only when
  `Caps::canMeasureMotion()` is true.
- `roamPoints` is a **derived exploration heuristic** from radio-environment
  change and advances on every board. Rendered as "Exploration pts", never as a
  distance, because nothing on any of these boards measures distance.

On a board with no IMU the steps line is **omitted entirely**. It is not shown
as "Steps 0" (which reads as "you did not walk" rather than "this device cannot
count steps") and exploration is not relabelled to fill the gap. The test is
`constexpr`, so the omitted branch costs the T-Dongle nothing.

Of the supported boards, only the Waveshare ESP32-S3-LCD-1.28 sets
`HEXHOUND_HAS_IMU`, so today it is the only one that shows a steps line.

### 2.4 Findings are flagged, not confirmed

The heading is "Findings flagged", not "threats found". The breakdown reads
"open networks", "repeat SSIDs" and "tracker leads" - the BLE heuristic matches
a manufacturer/RSSI pattern, so a hit is a **candidate worth a human look**,
not a tracker proven to exist. When anything is flagged, the line
"Flagged, not confirmed" is emitted directly under the breakdown.

### 2.5 An empty report is a sentence, not a blank screen

When every reportable counter is still zero the body is:

```
1 day since last report
Nothing logged yet
Take me out on patrol
```

This is a normal state for a new pet, not an error path. The screen never
renders an empty body, never divides by a count it did not check, and
`draw()` rebuilds the lines if it is somehow called before `open()`.

### 2.6 Form direction is a hint, and only when one is emerging

- `PetState::form` set -> "Form: Pathfinder" (green). The form field is owned by
  another workstream; this screen reports it, it never decides it.
- `PetState::form` unset -> "Leaning Pathfinder" (amber), and only when the
  leading `behaviour[]` counter is both at least `FORM_MIN_EVIDENCE / 2` and
  ahead of the runner-up by `FORM_LEAD_PERCENT`. Below that, no line at all.
  The bar is lower than the bar for actually becoming a form because a lean is
  a weaker claim than an identity; it is not zero because "you are leaning
  Gremlin" off two data points is noise wearing a hat.

Form display names live in a local static table in `ui_report.cpp` rather than
in `config.h`, because this is currently the only screen that renders one and
`config.h` is shared Phase 1 ground. Promote the table when a second consumer
appears, not before.

---

## 3. Layout

One implementation, three panel families, all derived from `SCREEN_W` /
`SCREEN_H` and `HEXHOUND_PANEL_ROUND` rather than from a board name.

| Panel | Font | Row pitch | Rows visible |
|---|---|---|---|
| 160x80 (T-Dongle S3) | size 1 | 9 px | 5 |
| 320x172 (Waveshare 1.47) | size 2 | 20 px | 5 |
| 240x240 round (Waveshare 1.28) | size 1 | 15 px | 8 |

- Every row is authored to `REPORT_TEXT_COLS` (25 glyphs), which is what the
  narrowest panel in the family can show. Anything longer is clipped with a
  visible ".." rather than cut off silently.
- The round panel clips to `uiround::chordHalfW(y)` **per row**, so nothing
  runs under the bezel at the top or bottom of the glass, and rows are centered
  rather than edge-aligned.
- More lines than fit scroll. The footer shows `first-last/total` (for example
  `4-8/13`) whenever there is more than one page, so a reader can see that more
  exists instead of having the tail vanish.
- A full populated report is 13 lines on a board with no IMU and 14 with one,
  against a `REPORT_MAX_LINES` of 14. `addNote()` / `addPair()` both refuse to
  write past the end.

---

## 4. Measured cost

Link probe: the temporary wiring from section 1 was applied, both boards were
built, and the delta against the same tree without it was measured. Unrouted
code is discarded by the linker, so building with the files merely present
reports zero, which is not the real number.

| Board | RAM before | RAM after | delta | Flash before | Flash after | delta |
|---|---|---|---|---|---|---|
| T-Dongle S3 (`lilygo-t-dongle-s3-vendor-app`) | 78916 | 79524 | **+608** | 1166197 | 1168353 | **+2156** |
| Waveshare 1.28 round (`waveshare-esp32-s3-lcd-128`) | 76784 | 77392 | **+608** | 1499781 | 1501745 | **+1964** |

The 608 bytes of RAM is the `UIReport` singleton exactly, confirmed against
`xtensa-esp32s3-elf-size -A` on `ui_report.cpp.o`: `.bss` for the instance is
600 bytes (14 `ReportLine` rows at 42 bytes each - 26 label, 12 value, colour,
kind, padded - plus the display pointer, count and scroll) and 8 more for the
function-local static guard. It is allocated once and the screen performs no
heap allocation at any point, which is the whole reason the rows are
fixed-size char arrays instead of `String`.

Flash side, from the same object: about 1.5 KB of `.text`, 0.4 KB of
`.literal` and 0.4 KB of `.rodata`; the rest of the measured delta is the
integrator's own wiring plus linker padding.

---

## 5. Verification performed

- `pio run -e lilygo-t-dongle-s3-vendor-app` - SUCCESS
- `pio run -e waveshare-esp32-s3-lcd-128` - SUCCESS
- `pio run -e desktop-sim`, `-e desktop-sim-wave`, `-e desktop-sim-round` - SUCCESS
- `pio test -e native` - 6/6 suites PASSED (test_content, test_event_bus,
  test_pet_core, test_pet_memory, test_pet_rules, test_storage)
- Rendered headlessly on all three panel families via
  `HEXHOUND_SIM_FORCE_SCREEN` + `HEXHOUND_SIM_SHOT_MS`, in the populated state,
  the scrolled state, the last page, and the nothing-logged state. The
  screenshot pass caught a real bug that compiling did not: notes longer than
  19 characters were being cut off by an under-sized buffer, with nothing on
  screen to indicate it. That is what `REPORT_TEXT_COLS` and the visible ".."
  clip now prevent.
