#pragma once

#include "../content/minigame.h"
#include "../board/capabilities.h"

// ── HexHound - Minigame Registry ────────────────────────────────
//
// The one place that knows which games exist. Adding a game is a line in
// game_registry.cpp; nothing in main.cpp, the menu or the reward path has to
// change, which was the whole point of the Minigame interface.
//
// Every game is a singleton with no per-round state until begin() is called,
// so the registry is an array of pointers with no allocation of its own.
//
// The list is filtered by capability, not by board name. A game that needs the
// IMU must never be offered on a board without one: an option that cannot work
// is worse than an option that is not there.

namespace games {

// Number of registered games, before capability filtering.
int count();

// Game by index in [0, count()). Returns nullptr out of range.
Minigame* at(int index);

// Game by its stable id(), or nullptr.
Minigame* byId(const char* id);

// True when this board satisfies everything the game declared it needs.
inline bool playable(const Minigame* g) {
    return g && (g->requiredCaps() & ~Caps::mask()) == 0;
}

// Number of games this board can actually run.
int playableCount();

// The nth game this board can actually run, or nullptr. This is the accessor a
// select screen should walk, so an unplayable game can never take a menu slot.
Minigame* playableAt(int index);

}  // namespace games
