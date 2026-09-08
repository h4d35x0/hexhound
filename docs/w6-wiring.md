# W6 Minigames - Integration Wiring

Two minigames now exist under `src/games/`. Nothing in `main.cpp`, `ui_menu.*`,
`config.h` or `pet_core.*` was touched, so the games are compiled but not yet
reachable. This is the list of what the integrator has to connect.

## Files added

| File | What it is |
|------|-----------|
| `src/games/game_layout.h` | Header-only layout kit. The only place that knows a panel dimension. Play rectangle, status line, centered text, panel-family constant, small PRNG. |
| `src/games/packet_chase.h` / `.cpp` | One-button endless runner. Implements `Minigame`. |
| `src/games/signal_memory.h` / `.cpp` | One-button Simon. Implements `Minigame`. |
| `src/games/game_registry.h` / `.cpp` | The list of games plus capability filtering. |

No existing file was modified. `build_src_filter` is `+<*>` on every firmware
and simulator environment, so the new translation units are picked up with no
platformio.ini change.

## Registry API

```cpp
#include "../games/game_registry.h"

int        games::playableCount();        // games this board can actually run
Minigame*  games::playableAt(int index);  // nth of those, or nullptr
Minigame*  games::byId(const char* id);   // stable id, for saved state
bool       games::playable(const Minigame*);
```

Walk `playableAt()` for the select screen, never `at()`. `playable()` compares
`Minigame::requiredCaps()` against `Caps::mask()`, so a game that needs hardware
this board lacks is never offered. Both games currently declare `0` (any board),
but the filter is the contract that keeps that true when a third game needs the
IMU.

Stable ids for high-score storage: `packet_chase`, `signal_memory`.

## SCREEN_GAMES - the select screen

Not built here. It needs to:

1. List `games::playableAt(0..playableCount()-1)`, showing `Minigame::name()`.
2. Advance the highlight on short press, choose on long press (or whatever the
   existing menu convention is - `ui_menu.cpp` owns that pattern).
3. On choose, store the chosen `Minigame*` and switch to `SCREEN_GAME_PLAY`.

## SCREEN_GAME_PLAY - the run loop

```cpp
static Minigame* g_active = nullptr;

// entering the screen
g_active = chosen;
g_active->begin(&tft);

// every main-loop iteration while SCREEN_GAME_PLAY is current
if (g_active && !g_active->update(millis())) {
    MinigameResult r = g_active->result();   // already valid here
    g_active->end();                         // frees the game's buffers
    g_active = nullptr;
    // award XP / mood from r, then return to SCREEN_GAMES
}

// short button press while SCREEN_GAME_PLAY is current
if (g_active) g_active->onPress();

// long button press: abandon
if (g_active) {
    g_active->end();
    MinigameResult r = g_active->result();   // completed == false
    g_active = nullptr;
    // no reward for an abandoned round
}
```

Four things this loop must get right:

* **`update()` must be called every loop iteration**, not on a UI timer. Both
  games rate-limit themselves internally (they early-return until 33 ms have
  passed), so calling them often is cheap and calling them rarely makes them
  stutter.
* **`end()` must always be called**, including on the abandon path and on any
  path that leaves `SCREEN_GAME_PLAY` for another reason (alert takeover,
  evolution cutscene, low battery). `end()` is what frees the round buffers. It
  is idempotent.
* **`onPress()` is only the short press.** Long press is the universal exit and
  the games never see it.
* **The games own the whole panel** while they are the active screen. They draw
  their own header, footer and status line, and they do not clear the screen
  between frames, so nothing else may draw over them.

## Rewards

`MinigameResult` is deliberately uniform so the reward path is written once:

```cpp
struct MinigameResult {
    uint32_t score;
    uint16_t durationMs;
    bool     completed;   // played out vs abandoned
    bool     newBest;
};
```

* `completed == false` means the player walked away. Suggest no reward, or a
  token one, so quitting a bad round is not a strategy.
* `newBest` is left `false` by both games. It belongs to whatever high-score
  store the integrator adds, keyed on `Minigame::id()`; the games have no
  persistence of their own and must not get any, because saving is the pet's
  job and games writing to flash on a no-PSRAM board is how the save file gets
  corrupted.
* `durationMs` is 16 bits, so it saturates at 65535 ms. A long round reports
  65535, not a wrapped small number. If real round lengths are ever needed,
  widen the field in `content_types.h`; do not work around it in the games.

Score scales are not comparable between games and are not meant to be:

* Packet Chase: body-widths travelled plus 5 per valid packet. Normalised by
  the sprite size, so the same play produces a similar score on a 160x80 and a
  320x172 panel.
* Signal Memory: the longest sequence reproduced. Range 0 to 32.

## Panel behaviour

`games::panel()` is `constexpr` and folds to one of `PANEL_COMPACT` (160x80),
`PANEL_BIG` (320x172) or `PANEL_ROUND` (240x240). Everything else derives from
`games::playArea()`. On the round board the play rectangle is inscribed in the
safe circle, so both games can treat it as an ordinary rectangle and still never
draw behind the bezel.

Titles are shortened on the round panel only (`PKT CHASE`, `SIGNAL`), because
the title row there is a 128 px chord and the long forms would run under the
bezel. That choice is made at compile time and costs nothing elsewhere.

## Memory

Verified on `lilygo-t-dongle-s3-vendor-app` (the no-PSRAM design target):

* Static: 276 bytes total once the registry is referenced - `PacketChase` 108 B
  `.data`, `SignalMemory` 136 B `.bss`, plus 32 B of guard and table words.
* Heap while a round is running: Packet Chase 80 B (10 entities), Signal Memory
  32 B (the sequence). One game at a time, allocated in `begin()`, freed in
  `end()`, and `update()` never touches the heap.
* Flash: about 3.1 KB for Packet Chase and 3.5 KB for Signal Memory once
  reachable.

Until the integrator references the registry, the linker garbage-collects
almost all of it: the tree currently builds at the same 71636 bytes of RAM as
before and 808 bytes more flash (retained `.eh_frame` only).
