#pragma once

#if defined(UNIT_TEST)
// Unit tests provide their own stubs
#include <cstdint>
#include <cstring>
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

#include "content_types.h"
#include "content_store.h"

// ── HexHound - Quest Engine ──────────────────────────────────────
//
// Offers up to QUEST_MAX_ACTIVE quests a day, tracks their progress, and
// retires them when they are done.
//
// The rule that matters most: a quest is only offered if this board can
// actually finish it. requiresCaps is checked against Caps::mask() before a
// definition ever reaches a slot, because a quest the hardware cannot complete
// is worse than an empty list. It reads as a bug, it cannot be cleared, and it
// makes the pet look like it does not know what it is running on.
//
// The roll is seeded and reproducible: the same seed on the same board yields
// the same day's quests. That is what makes it testable and what lets a save
// restore the day without storing every field.
//
// Fixed storage, no allocation. Progress is reported by kind, matching the
// split in content_types.h: the rules layer reports events, this engine counts.

// One reroll a day, shared across the slots. Enough to escape a quest that
// does not fit your day, not enough to shop for the cheapest one.
#define QUEST_REROLLS_PER_DAY  1

class QuestEngine {
public:
    static QuestEngine& instance();

    // Rolls the first day. Safe to call before ContentStore::begin(), in which
    // case it simply offers nothing until rollDaily() is called again.
    void begin(uint32_t seed);

    // Replaces every slot and restores the reroll budget. Call on a new day.
    void rollDaily(uint32_t seed);

    uint8_t activeCount() const { return _count; }
    const QuestInstance* quest(uint8_t index) const;

    // The definition behind a slot, for reward values and the capability gate.
    // nullptr if the index is out of range or the pack no longer defines it.
    const QuestDef* definitionFor(uint8_t index) const;

    // Advances every unfinished quest of this kind. Returns true if at least
    // one of them completed on this call, so the caller knows to celebrate.
    bool reportProgress(QuestKind kind, uint16_t amount = 1);

    // Same, for an event that maps to one specific quest rather than a kind.
    bool reportProgressById(const char* id, uint16_t amount = 1);

    // Swaps one slot for a different eligible quest. Fails when the index is
    // out of range, the quest is already complete, the budget is spent, or
    // there is no other eligible definition to swap in.
    bool reroll(uint8_t index, uint32_t seed);
    uint8_t rerollsLeft() const { return _rerollsLeft; }

    // Slots that transitioned to COMPLETE since the last call, then forgets
    // them. Fills `out` and returns how many. See the definition for why the
    // engine reports indices rather than reward values.
    uint8_t takeJustCompleted(uint8_t* out, uint8_t max);

    uint8_t completedCount() const;
    bool allComplete() const;

    // True when this board can complete the quest. Public because the UI and
    // any future quest browser must apply exactly the same gate as the roll.
    static bool isOfferable(const QuestDef& def);

private:
    QuestEngine() = default;

    // Bitmask of slots that completed since takeJustCompleted() last ran.
    // NOT persisted, and must not be: it records an event, and an event that
    // survived a reboot would pay its reward a second time.
    uint8_t _justCompleted = 0;

    // Fills one slot from the eligible pool, avoiding ids already on offer.
    // `preferredKind` is tried first and QUEST_KIND_COUNT means "any kind".
    // `excludeId` blocks one more id, which is how a reroll guarantees it does
    // not hand back the quest the player just rejected.
    bool fillSlot(uint8_t index, uint8_t preferredKind,
                  const char* excludeId = nullptr);
    bool isOffered(const char* id, uint8_t exceptIndex) const;
    void assign(uint8_t index, const QuestDef& def);

    QuestInstance _quests[QUEST_MAX_ACTIVE];
    uint8_t       _count = 0;
    uint8_t       _rerollsLeft = QUEST_REROLLS_PER_DAY;
    uint32_t      _rng = 0;
};
