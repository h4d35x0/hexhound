#include "pet_memory.h"
#include "pet_core.h"
#include "../content/content_store.h"
#include "../content/dialogue_engine.h"

// ── HexHound - Pet Memory Implementation ─────────────────────────

namespace PetMemory {

uint32_t accessor(uint8_t slot, void* user) {
    (void)user;
    const PetState& s = PetCore::instance().state();

    switch (slot) {
    case MEM_SLOT_VISITS:
        return s.checkIns;

    case MEM_SLOT_PATROLS:
        // A patrol is one Wi-Fi scan cycle, which is what wifiScans counts.
        return s.wifiScans;

    case MEM_SLOT_NEW_NETWORKS:
        // Unique SSIDs ever logged, not total sightings. "I have met 23
        // networks" is true; counting repeat sightings would not be.
        return s.seenWifiCount;

    case MEM_SLOT_THREATS:
        return (uint32_t)s.threatOpenCount
             + (uint32_t)s.threatDuplicateCount
             + (uint32_t)s.threatTrackerCount;

    case MEM_SLOT_QUESTS_DONE:
        return s.questsCompleted;

    case MEM_SLOT_BEST_SCORE:
        return s.bestGameScore;

    case MEM_SLOT_DAYS_AWAY:
    default:
        // Unreachable in practice: DAYS_AWAY is declared unavailable by
        // unavailableSlotMask(), so no line asking for it is ever eligible,
        // and an unknown slot is rejected by the engine's range check. Return
        // 0 only as a defined value for a call that should not happen, never
        // as an answer anyone renders.
        return 0;
    }
}

uint32_t unavailableSlotMask() {
    // MEM_SLOT_DAYS_AWAY is genuinely unknowable here. There is no RTC, and
    // millis() resets on every boot, so the device cannot tell four minutes
    // unplugged from four months. Wi-Fi NTP could answer it one day, and the
    // content line is already written for that day. Until then the line simply
    // never comes up, which is better than it coming up wrong.
    //
    // This is NOT the same question as hibernation. That measures POWERED idle
    // time, which the device can count, so it needs no clock. See
    // PetState::idleTicks.
    return (1UL << MEM_SLOT_DAYS_AWAY);
}

void recordCheckIn() {
    PetState& s = PetCore::instance().state();
    if (s.checkIns < 0xFFFF) {
        s.checkIns++;
    }
    s.dirty = true;
}

void recordQuestComplete() {
    PetState& s = PetCore::instance().state();
    if (s.questsCompleted < 0xFFFF) {
        s.questsCompleted++;
    }
    s.dirty = true;
}

bool recordGameScore(uint32_t score) {
    PetState& s = PetCore::instance().state();
    if (score <= s.bestGameScore) {
        return false;
    }
    s.bestGameScore = score;
    s.dirty = true;
    return true;
}

}  // namespace PetMemory
