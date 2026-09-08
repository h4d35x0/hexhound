# W5 Quest and Game Screens - Integrator Wiring

Two screens now exist: the daily quest list for `SCREEN_QUESTS` and the minigame
select for `SCREEN_GAMES`. They compile on every target but nothing routes to
them yet, so the linker still garbage-collects both. This file is the handover.

The files that must be edited to finish the wiring (`src/main.cpp`,
`src/ui/ui_menu.*`, `src/config.h`) are owned by other work, so W5 did not touch
them. Neither did it touch `src/pet/*`, `src/content/*` or `src/games/*`.

## Files added

| File | Purpose |
|------|---------|
| `src/ui/ui_quests.h` / `.cpp` | Daily quest list: text, kind, progress, completion, reroll. |
| `src/ui/ui_games.h` / `.cpp` | Minigame select, built from `games::playableAt()`. |

No existing file was modified. `build_src_filter` is `+<*>` on every firmware
and simulator environment, so the new translation units are picked up with no
`platformio.ini` change.

## Boot wiring

Both screens are singletons and both need the TFT pointer before anything can
draw, exactly like `UIMissions`. Add them beside the other `init()` calls in
`setup()`:

```cpp
UIQuests::instance().init(tft);
UIGames::instance().init(tft);
```

`UIQuests` reads `QuestEngine` on every draw, so `ContentStore::begin()` and
`QuestEngine::begin(daySeed)` must have run first (see `docs/w2-wiring.md` for
the boot order). If they have not, the screen draws its empty state rather than
misbehaving, so the ordering is a content problem and not a crash.

`UIGames` has no such dependency: the registry is self-contained.

## Menu entry

`MENU_QUESTS` and `MENU_GAMES` already exist in `MenuItem` and already have
labels in `MENU_LABELS`. The menu owner adds them to `MENU_ORDER` in
`ui_menu.cpp` and `main.cpp` switches on them:

```cpp
case MENU_QUESTS:
    switchScreen(SCREEN_QUESTS);
    UIQuests::instance().open();
    break;
case MENU_GAMES:
    switchScreen(SCREEN_GAMES);
    UIGames::instance().open();
    break;
```

## Button handling

Same one-button convention as `SCREEN_MISSIONS`: short press moves the cursor,
long press acts on the highlighted row. Row 0 of each list is `BACK`.

### SCREEN_QUESTS

```cpp
// short press
UIQuests::instance().scrollDown();

// long press
if (UIQuests::instance().backSelected()) {
    switchScreen(SCREEN_MENU);
    UIMenu::instance().open();
} else if (!UIQuests::instance().rerollSelected(rerollSeed())) {
    // Nothing changed: no budget left, or the quest is already complete.
    // The footer already says which, so a toast here is optional.
}
```

`rerollSelected()` redraws on success and returns false without touching
anything on failure, so the caller never has to re-derive the reroll rules.
`canRerollSelected()` is exposed for a caller that wants to check first.

`rerollSeed()` only has to vary per call; `millis()` is fine. It is mixed into
the engine's RNG, not used as a fresh seed, so a poor one cannot make the day
reproducible in a way it should not be.

The reroll budget is `QUEST_REROLLS_PER_DAY` (currently 1) and is restored by
`QuestEngine::rollDaily()`, not by this screen.

### SCREEN_GAMES

```cpp
// short press
UIGames::instance().scrollDown();

// long press
if (UIGames::instance().backSelected()) {
    switchScreen(SCREEN_MENU);
    UIMenu::instance().open();
} else if (Minigame* g = UIGames::instance().selectedGame()) {
    g_active = g;
    g_active->begin(tft);
    switchScreen(SCREEN_GAME_PLAY);
}
```

`selectedGame()` already walks `games::playableAt()`, so the caller never
touches the registry and cannot accidentally use `games::at()` and hand a
capability-gated game to a board that lacks the hardware. It returns `nullptr`
on `BACK` and on a list that shifted underneath the cursor; treat that as "do
nothing".

`SCREEN_GAME_PLAY` itself is W6 territory. Its run loop, the mandatory `end()`
on every exit path, and the reward shape are all in `docs/w6-wiring.md`.

When the round ends, return to `SCREEN_GAMES` with `UIGames::instance().draw()`
rather than `open()`, so the cursor stays on the game just played.

## Quest progress and rewards

This screen only renders. It never pays a reward and never reports progress;
both belong to the rules layer, per `docs/w2-wiring.md`. The one exception the
integrator has to build is `QUEST_LIFE`: nothing in the firmware can observe a
user-defined quest, so it can only ever be confirmed by the user. Today this
screen offers reroll on long press, not completion. If `QUEST_LIFE` should be
confirmable here, that is a deliberate product decision and a second action on
an already-single-button screen; it was left out rather than guessed at.

## Empty lists are normal, not faults

Both screens are built for a list that is short or empty, because both sources
can legitimately produce one:

* `QuestEngine::activeCount()` can be less than `QUEST_MAX_ACTIVE`, or zero, when
  a content pack leaves this board with too few eligible definitions. The screen
  draws `BACK` plus a "No quests today" line, and the footer reads `0/0 done`.
* `games::playableCount()` can be zero on a board where every game is
  capability-gated out. The screen draws `BACK` plus "No games here".

Neither case is an error path, and neither should be routed around by hiding the
menu entry.

## Panel families

All three families are handled and were verified by rendering into the SDL
simulator on each:

| Family | Layout |
|--------|--------|
| 160x80 | Size-1 rows, 13 px pitch, 4 visible. Compact header and footer. |
| 320x172 (`SCREEN_H > 100`) | Size-2 rows, 22 px pitch, 5 visible, via `uiBigHeader` / `uiBigFooter`. |
| 240x240 (`HEXHOUND_PANEL_ROUND`) | Centered rows with chord-clipped selection capsules, via `uiround::*`. Quest rows are two size-1 lines (text, then kind and progress) because a size-2 quest text is wider than any chord on that glass. |

Every position derives from `SCREEN_W` / `SCREEN_H`, and the round path derives
its horizontal extents from `uiround::chordHalfW()` per row, so nothing is drawn
under the bezel.

### Quest text is truncated, deliberately and visibly

A quest row shows the kind tag, the text, and the progress on one line, and the
text gets whatever is left. On the rectangular panels that is about **14
characters**; the round panel renders the text at size 1 and fits roughly **30**.
Text that does not fit ends in `..` so a truncated row never reads as a short
quest.

The big panel could show more text at size 1, but every other 320x172 screen in
this firmware is size 2 for legibility and breaking that convention on one screen
is worse than truncating. The practical consequence is for whoever authors
content: **front-load the distinguishing word of a quest**. "Run two patrols."
survives truncation; "Meet five networks you have never seen." reads as "Meet
five net..". Nothing in the code needs to change for this, it is an authoring
note.

## Cost

Measured on `lilygo-t-dongle-s3-vendor-app`, the no-PSRAM design target.

As the tree stands, nothing references either screen, so the linker drops almost
all of both and the build is unchanged at **71,652 bytes of RAM** (the same
baseline W6 reports) and 1,139,153 of flash, which is 416 bytes more than before
purely from retained `.eh_frame`. That number is not the cost of this work and
should not be quoted as it.

The real cost was measured with a link probe: a temporary translation unit whose
global constructor calls into both screens, so `.init_array` keeps them alive.
Three builds, each differing from the last by exactly one thing:

| Build | RAM | Flash |
|-------|-----|-------|
| Baseline, W5 files absent | 71,652 | 1,138,737 |
| Probe referencing only `QuestEngine` + `games::` | 78,660 | 1,153,153 |
| Probe referencing the two W5 screens | 78,684 | 1,155,433 |

* **The screens themselves cost 24 bytes of RAM and 2,280 bytes of flash.** The
  24 bytes are exactly the two singletons, which `xtensa-esp32s3-elf-size -A`
  confirms independently: `.bss._ZZN8UIQuests8instanceEvE2ui` is 12 bytes and
  `.bss._ZZN7UIGames8instanceEvE2ui` is 12. Neither screen owns a buffer; the
  largest allocation on either draw path is a 96-byte stack scratch for one row
  of truncated text.
* **Reaching them costs 7,008 bytes of RAM and 14,416 of flash**, which is
  `ContentStore`'s two fixed content tables plus the game registry. That is W2's
  and W6's already-documented cost and the integrator pays it the moment
  anything at all touches those engines, whoever draws the pixels.
* **Total once wired: RAM 71,652 -> 78,684 (+7,032, 21.9% -> 24.0%), flash
  1,138,737 -> 1,155,433 (+16,696).**

The probe was deleted after measurement; it is not in the tree.

## Verified

| Environment | Result |
|-------------|--------|
| `lilygo-t-dongle-s3-vendor-app` | SUCCESS, RAM 71,652 / flash 1,139,153 |
| `waveshare-esp32-s3-lcd-128` (round) | SUCCESS, RAM 69,504 / flash 1,473,109 |
| `desktop-sim` (160x80) | SUCCESS |
| `desktop-sim-round` (240x240) | SUCCESS |
| `desktop-sim-wave` (320x172) | SUCCESS |

Rendering was checked on all three panel families by driving the screens through
the simulator's headless capture path: populated list, zero-quest empty state,
cursor on `BACK`, cursor on a completed quest (footer reads `complete`, no
reroll offered), cursor on an open quest (footer reads `hold=roll`), and the
scrolled window on the 160x80 where five entries do not fit in four rows.
