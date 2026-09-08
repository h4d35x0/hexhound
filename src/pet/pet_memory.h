#pragma once

#include <stdint.h>

// ── HexHound - Pet Memory ────────────────────────────────────────
//
// The bridge between what the pet has actually done and what it is allowed to
// say about it.
//
// Design rules, in order of importance:
//
//  1. SUMMARIES, NOT HISTORY. The pet can say "we have run 41 patrols" without
//     the device holding a log of where and when it ran them. On a security
//     product a location-tagged activity history is a liability, not a
//     feature, and a giveaway device that quietly accumulates one is exactly
//     the outcome this project should never ship.
//
//  2. THE FIRMWARE OWNS THE NUMBERS. Content declares which statistic it wants
//     by slot; this module supplies the value. Content can name a number, never
//     invent one.
//
//  3. AN UNKNOWABLE NUMBER IS DECLINED, NOT GUESSED. Slots this hardware
//     cannot compute return DIALOGUE_MEMORY_UNAVAILABLE, which makes the line
//     ineligible. Nothing is rendered rather than something plausible and
//     wrong. See MEM_SLOT_DAYS_AWAY below.
//
// Most slots are served from counters PetState already kept. Only the three
// that had no source at all were added, rather than aliasing a nearby field
// that would have made the pet assert something untrue.

namespace PetMemory {

// Install this as the DialogueEngine memory accessor. `user` is ignored; the
// pet is a singleton, so there is nothing to thread through.
uint32_t accessor(uint8_t slot, void* user);

// Bitmask of MemorySlot values this build cannot compute, for
// DialogueEngine::setUnavailableSlots(). Constant for a given board, so the
// wiring sets it once at init.
uint32_t unavailableSlotMask();

// ── Recorders ─────────────────────────────────────────────────────────────
// Call these from the rules layer as things happen. Each marks the pet dirty.

// The screen was woken and the pet looked at. This is the ten-second loop, so
// it is deliberately separate from recordInteraction(), which counts anything
// the user does at all.
void recordCheckIn();

// A daily quest was completed. Feeds MEM_SLOT_QUESTS_DONE.
void recordQuestComplete();

// A minigame round finished. Keeps the all-time best only, which is one
// number rather than a score history. Returns true if this beat the record,
// so the caller can set MinigameResult::newBest without a second lookup.
bool recordGameScore(uint32_t score);

}  // namespace PetMemory
