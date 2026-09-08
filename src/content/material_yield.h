#pragma once

#include <stdint.h>

// ── HexHound - Where craftable materials come from ──────────────
//
// One place owns this, because until it existed the answer was NOWHERE:
// `Inventory::add()` had no caller anywhere in the firmware, only in the unit
// tests. The Kit screen, the Den, the nine recipes, the item icons and their
// tests were all finished around a supply of exactly zero, so the Kit could
// never fill, nothing could ever be crafted, and the Den could never be
// furnished. The Kit's own footer said "go roaming", which was advice the
// firmware could not honour.
//
// Three rules shaped the tables below.
//
// 1. **Every material must be reachable on every board.** Four of the eight are
//    only asked for by the Beacon Beast recipes, and the roam's Paced Alloy
//    needs an IMU that the T-RGB does not have. So nothing may sit behind steps
//    alone, or that board could never craft four of the nine recipes. Steps are
//    an accelerator here, never the only door to a material.
//
// 2. **The everyday action pays, in something spendable TODAY.** A patrol is
//    what someone actually repeats, so it is a steady drip of the common three.
//    A roam is a deliberate expedition, so it pays more and is the main source
//    of the rarer half. A game pays circuits and glitches - and scrap, because
//    those two are Beacon Beast tier only and a first cut without the scrap
//    handed a young pet a currency it could not spend on anything it owned.
//    Every activity must yield at least one material an early recipe wants;
//    test_material_yield fails if one stops doing so.
//
// 3. **Nothing here touches a stat.** Materials are cosmetic input only; see
//    the rule at the top of item_defs.h. Crafting spends them for a cosmetic
//    and nothing else.
//
// Every yield is integer division, so a short outing can legitimately return
// nothing rather than rounding up into a free material. The divisors are a
// first cut meant to be tuned by playing, which is why they are named
// constants in one place rather than scattered literals.
//
// Deliberately free of every other header: it takes plain counters, not a
// RoamReport or a Minigame, so the whole economy is exercisable in a native
// test with no board, no radio and no display.

struct MaterialGrant {
    const char* item;   // an ITEM_DEFS string id, e.g. "mat.scrap"
    uint16_t    qty;
};

// Roam is the widest table: seven entries plus the step accelerator.
#define MATERIAL_GRANT_MAX 10

namespace matyield {

// ── Patrol: the steady drip ───────────────────────────────────────────────
constexpr uint16_t PATROL_NETS_PER_SCRAP    = 6;
constexpr uint16_t PATROL_NEW_PER_SIGNAL    = 3;
constexpr uint16_t PATROL_NEWBLE_PER_SHARD  = 2;

// ── Roam: the expedition, and the only source of three rare materials ─────
constexpr uint32_t ROAM_POINTS_PER_SCRAP    = 25;
constexpr uint32_t ROAM_POINTS_PER_COPPER   = 60;
constexpr uint32_t ROAM_POINTS_PER_FERRITE  = 100;
constexpr uint16_t ROAM_NEW_PER_SIGNAL      = 4;
constexpr uint16_t ROAM_NEW_PER_CIRCUIT     = 10;
constexpr uint16_t ROAM_BLE_PER_SHARD       = 3;
constexpr uint16_t ROAM_BLE_PER_LENS        = 8;
constexpr uint32_t ROAM_STEPS_PER_COPPER    = 500;   // accelerator only

// ── Minigames ─────────────────────────────────────────────────────────────
// Scrap is here for a reason found by playing: circuit and glitch are BOTH
// Beacon Beast tier only - circuit appears in goggles, collar, fern and
// lantern, glitch only in collar - so a first cut that paid only those two gave
// a Packet Pup a pile of currency it could not spend on anything. Games are
// what a young pet does most, and an activity that pays in nothing usable is
// worse than one that pays nothing at all, because it looks like progress.
//
// Scrap is the right answer rather than a token: it is the bottleneck in all
// three den recipes a Packet Pup can actually reach.
constexpr uint32_t GAME_SCORE_PER_SCRAP     = 50;
constexpr uint32_t GAME_SCORE_PER_CIRCUIT   = 40;
constexpr uint32_t GAME_SCORE_PER_GLITCH    = 120;

inline void push(MaterialGrant* out, uint8_t& n, const char* item, uint32_t qty) {
    if (qty == 0 || !out || n >= MATERIAL_GRANT_MAX) return;
    if (qty > 0xFFFFu) qty = 0xFFFFu;
    out[n].item = item;
    out[n].qty  = (uint16_t)qty;
    n++;
}

// What one completed patrol puts in the kit.
inline uint8_t patrolYield(uint16_t networks, uint16_t newWifi, uint16_t newBle,
                           MaterialGrant* out) {
    uint8_t n = 0;
    push(out, n, "mat.scrap",  (uint32_t)networks / PATROL_NETS_PER_SCRAP);
    push(out, n, "mat.signal", (uint32_t)newWifi  / PATROL_NEW_PER_SIGNAL);
    push(out, n, "mat.shard",  (uint32_t)newBle   / PATROL_NEWBLE_PER_SHARD);
    return n;
}

// What one expedition brings home. `stepsMeasured` is false on every board that
// cannot count steps, and the yield must still be complete without it.
inline uint8_t roamYield(uint32_t points, uint16_t newWifi, uint16_t newBle,
                         uint32_t steps, bool stepsMeasured,
                         MaterialGrant* out) {
    uint8_t n = 0;
    push(out, n, "mat.scrap",   points / ROAM_POINTS_PER_SCRAP);
    push(out, n, "mat.copper",  points / ROAM_POINTS_PER_COPPER);
    push(out, n, "mat.ferrite", points / ROAM_POINTS_PER_FERRITE);
    push(out, n, "mat.signal",  (uint32_t)newWifi / ROAM_NEW_PER_SIGNAL);
    push(out, n, "mat.circuit", (uint32_t)newWifi / ROAM_NEW_PER_CIRCUIT);
    push(out, n, "mat.shard",   (uint32_t)newBle  / ROAM_BLE_PER_SHARD);
    push(out, n, "mat.lens",    (uint32_t)newBle  / ROAM_BLE_PER_LENS);
    if (stepsMeasured) {
        // A second copper entry rather than a bigger first one: the step bonus
        // is a separate thing that happened and reads that way in a test.
        push(out, n, "mat.copper", steps / ROAM_STEPS_PER_COPPER);
    }
    return n;
}

// What a finished round of a minigame is worth. An ABANDONED round yields
// nothing, for the same reason it does not count toward a quest: entering and
// leaving a game must not farm anything.
inline uint8_t gameYield(uint32_t score, bool completed, MaterialGrant* out) {
    if (!completed) return 0;
    uint8_t n = 0;
    push(out, n, "mat.scrap",   score / GAME_SCORE_PER_SCRAP);
    push(out, n, "mat.circuit", score / GAME_SCORE_PER_CIRCUIT);
    push(out, n, "mat.glitch",  score / GAME_SCORE_PER_GLITCH);
    return n;
}

}  // namespace matyield
