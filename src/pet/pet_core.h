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
#include "../config.h"
#include "../social/hexpass_types.h"

// ── HexHound - Pet Core ──────────────────────────────────────────

struct PetStats {
    int16_t  hunger   = 50;
    int16_t  mood     = 50;
    int16_t  energy   = 100;
    uint32_t xp       = 0;
    int16_t  trust    = 0;
    int16_t  mischief = 0;
    int16_t  health   = 100;
};

enum PetPerk : uint16_t {
    PERK_SIGNAL_CARTOGRAPHER = 1 << 0,
    PERK_BEACON_HUNTER       = 1 << 1,
    PERK_ANOMALY_ARCHIVIST   = 1 << 2,
    PERK_MISSION_OPERATOR    = 1 << 3,
    PERK_FIELD_SENTINEL      = 1 << 4
};

// Bump when the persisted shape changes in a way a plain `doc["x"] | default`
// read cannot absorb: a renamed field, a changed unit, a field whose default is
// wrong for old saves. Adding a brand-new optional field does NOT need a bump,
// because every read already supplies a default.
//
// PetCore::migrate() runs on load for any save older than this. Keep each step
// small and additive; never drop a field an older step still reads.
//
//   1 - original shape, no version key present (treated as 1 on load)
//   2 - Phase 1: behavioural form axis, inventory, den layout, roam counters
//   3 - Phase 2 W1: HexPass identity, encounter ring, block list, settings
//   4 - Phase 4: worn cosmetics (wornSlots[])
//   5 - Phase 4: the flourish slot (flourishSlot)
#define PET_SCHEMA_VERSION 5

struct PetState {
    uint16_t  schemaVersion = PET_SCHEMA_VERSION;
    char      name[20]    = "HexHound";
    PetStage  stage       = STAGE_EGG;
    PetStats  stats;
    Trait     traits[2]   = { TRAIT_CURIOUS, TRAIT_BRAVE };
    uint16_t  interactions = 0;
    uint16_t  wifiScans   = 0;
    uint16_t  usbConnects = 0;
    uint16_t  seenWifiCount = 0;
    uint16_t  seenBleCount  = 0;
    char      seenWifi[MAX_SEEN_WIFI_CAPTURES][33] = {};
    char      seenBle[MAX_SEEN_BLE_CAPTURES][18]   = {};
    uint32_t  masteryXP   = 0;
    uint16_t  masteryRank = 0;
    uint16_t  threatOpenCount = 0;
    uint16_t  threatDuplicateCount = 0;
    uint16_t  threatTrackerCount = 0;
    uint16_t  missionsCompleted  = 0;
    uint16_t  perkMask    = 0;
    uint32_t  saveSeq     = 0;
    bool      hatched     = false;
    bool      dirty       = false;  // needs save

    // ── Hibernation ────────────────────────────────────────────────────────
    // Ticks since the last interaction. decayTick() only runs from the powered
    // main loop, so an unplugged pet already does not decay at all: leaving
    // HexHound in a drawer for a month costs nothing. The case that actually
    // punished people is the opposite one, a device left plugged in on a desk,
    // grinding hunger and mood down for hours with nobody watching.
    //
    // So hibernation is keyed to powered idle time, not wall-clock absence.
    // There is no RTC to measure absence with anyway, and inventing one from
    // boot counts would be a guess presented as a fact.
    uint16_t  idleTicks   = 0;
    bool      hibernating = false;

    // ── Memory counters ────────────────────────────────────────────────────
    // Summaries, deliberately not a raw activity log. The pet should be able
    // to say "we have run 41 patrols together" without the device carrying a
    // history of where and when, which on a security product is a liability
    // rather than a feature.
    //
    // Every other MemorySlot already had a source (patrols from wifiScans,
    // new networks from seenWifiCount, threats from the three threat counts).
    // These three did not, and inventing them from a nearby-looking field
    // would have made the pet state things that were not true.
    uint16_t  checkIns        = 0;   // MEM_SLOT_VISITS
    uint16_t  questsCompleted = 0;   // MEM_SLOT_QUESTS_DONE
    uint32_t  bestGameScore   = 0;   // MEM_SLOT_BEST_SCORE

    // Seed for the daily quest roll. Monotonic, and stable for as long as the
    // "day" lasts, which is what makes the same three quests survive a reboot
    // instead of rerolling on every boot. See TICKS_PER_QUEST_DAY.
    uint16_t  questDay        = 0;
    uint16_t  dayTicks        = 0;

    // ── Phase 1 state ──────────────────────────────────────────────────────
    // Added centrally in one migration rather than by each workstream, because
    // four of the six add persisted state and three teams independently
    // bumping the schema version would collide. ~80 bytes total.

    // Behavioural form: a second axis, independent of stage. See PetForm.
    PetForm   form = FORM_UNSET;
    // Evidence for each form, indexed by (PetForm - 1). Saturating counters,
    // never decayed: an identity is earned over the pet's life, and letting it
    // rot would punish someone for a quiet fortnight.
    uint16_t  behaviour[FORM_BEHAVIOUR_COUNT] = {};

    // Inventory counts by item id. Fixed size: a growable structure on a
    // no-PSRAM board fragments the heap and fails somewhere unrelated.
    uint16_t  items[ITEM_TYPE_COUNT] = {};

    // Den placement: slot -> item id, 0 meaning empty. Full array on every
    // board even though TIER_CORE shows fewer slots, so a pet keeps its whole
    // room when the save moves between boards.
    uint8_t   denSlots[DEN_SLOT_COUNT] = {};

    // ── Phase 4 state ──────────────────────────────────────────────────────
    // What the pet is WEARING: anchor -> item id, 0 meaning bare. Indexed by
    // (WearAnchor - 1), the same "enum minus its NONE row" shape `behaviour`
    // uses, so WEAR_NONE cannot address a slot.
    //
    // Item id 0 works as "bare" for the same reason it works as "empty" in
    // denSlots[]: id 0 is mat.scrap, a material, and nothing that is not
    // COSMETIC_WORN can ever be written here. src/pet/pet_wear.h holds the
    // gate and a test pins the invariant.
    //
    // Persisted, and deliberately NOT cleared on evolution: a hat the owner
    // put on is theirs, and taking it off because the pet grew would be the
    // firmware undoing a choice nobody asked it to undo.
    uint8_t   wornSlots[WEAR_SLOT_COUNT] = {};

    // The pet's FLOURISH: one item id, 0 meaning none.
    //
    // One byte and not an array, because a flourish is the whole picture round
    // the pet rather than a thing hung on a body part. Two of them at once
    // would be two particle systems fighting over the same ring of pixels at
    // two cadences, which is noise, not decoration. So it is one at a time and
    // the type says so.
    //
    // Id 0 means NONE for the same reason it means bare in wornSlots[] and
    // empty in denSlots[]: id 0 is mat.scrap, a material, and nothing that is
    // not COSMETIC_FLOURISH can ever be written here. src/pet/pet_flourish.h
    // holds the gate and test_flourish pins the invariant.
    //
    // Persisted, and NOT cleared on evolution: same argument as wornSlots[].
    uint8_t   flourishSlot = 0;

    // Roam. stepCount is REAL steps and only ever advances on a board with an
    // IMU; roamPoints is exploration derived from radio-environment change and
    // advances everywhere. Two fields, never one, so a derived number can
    // never be displayed as a step count.
    uint32_t  roamPoints   = 0;
    uint32_t  stepCount    = 0;
    uint16_t  roamSessions = 0;

    // questDay at the last field report, so the report can say "since last
    // time" without a clock it does not have.
    uint16_t  lastReportDay = 0;

    // ── Phase 2 state ──────────────────────────────────────────────────────
    // HexPass: the device's own rotating identity, who it has passed, and the
    // owner's consent settings. Owned and written only by src/social/hexpass.*;
    // it lives here because this is the struct that gets persisted, and there
    // must be exactly one copy of a number. Same arrangement as items[] and
    // the inventory engine.
    //
    // `enabled` inside defaults to FALSE. That default is the feature's whole
    // consent story and must not be changed without changing the threat model
    // first: docs/hexpass-threat-model.md, "Off by default. Not opt-out, not
    // on-with-a-notice."
    HexPassState hexpass;
};

class PetCore {
public:
    static PetCore& instance();

    void init();

    // Patrol reward accumulator - tracks raw food gained before stat clamping
    void resetFoodAccum()            { _foodAccum = 0; }
    int  foodAccum() const           { return _foodAccum; }

    // Stat modifiers (clamped to 0-100 except XP)
    void feedHunger(int16_t amount);
    void changeMood(int16_t amount);
    void changeEnergy(int16_t amount);
    void addXP(uint32_t amount);
    void addMasteryXP(uint32_t amount);
    void changeTrust(int16_t amount);
    void changeMischief(int16_t amount);
    void changeHealth(int16_t amount);

    // Decay stats on timer tick
    void decayTick();

    // Evolution check
    void checkEvolution();

    // Record interactions for egg hatching
    void recordInteraction();
    void recordWifiScan();
    void recordUsbConnect();
    void recordThreatOpen();
    void recordThreatDuplicate();
    void recordThreatTracker();
    void recordMissionComplete();
    bool markWifiSeen(const char* ssid);
    bool markBleSeen(const char* addr);

    // Trait helpers
    bool hasTrait(Trait t) const;
    float traitMultiplier(Trait t) const;

    // Accessors
    PetState&       state()       { return _state; }
    const PetState& state() const { return _state; }
    const char*     stageName() const;
    uint32_t        xpForCurrentStage() const;
    uint32_t        xpForNextStage() const;
    uint32_t        xpToNextStage() const;
    uint32_t        masteryForCurrentRank() const;
    uint32_t        masteryForNextRank() const;
    uint32_t        masteryToNextRank() const;
    const char*     nextStageName() const;
    const char*     masteryTitle() const;
    uint16_t        totalThreatFindings() const;
    uint8_t         unlockedPerkCount() const;
    bool            hasPerk(PetPerk perk) const;

    // Serialization
    // ── The three surfaced states ─────────────────────────────────────────
    // Seven visible stat bars is more than anyone tracks, so the UI shows
    // three and the seven stay underneath as the computation layer. These are
    // derived, never stored: there is exactly one source of truth for each
    // number, so they cannot drift out of sync with the stats that feed them.
    //
    //   Bond   how attached it is to you     <- trust
    //   Spirit how it is doing               <- mood and health
    //   Energy how much it has left in it    <- energy
    int16_t bond() const;
    int16_t spirit() const;
    int16_t energy() const;

    // Wake from hibernation and reset the idle counter. Safe to call when not
    // hibernating. Returns true if this call actually woke it, so the caller
    // can show the warm greeting exactly once.
    bool rouse();
    bool isHibernating() const { return _state.hibernating; }

    // ── Quest day ─────────────────────────────────────────────────────────
    // Call once at boot, after the save has loaded. Rolls the quest day
    // forward, because a power-on is the closest thing this hardware has to
    // "a new day started".
    void beginSession();
    // Call once per timer tick. Returns true when the day rolled over, so the
    // caller can re-roll the quest list.
    bool tickQuestDay();
    uint32_t questDaySeed() const { return _state.questDay; }

    bool loadFrom(const char* json);
    String saveToJson() const;

    // Upgrade a just-loaded state from `fromVersion` to PET_SCHEMA_VERSION.
    // Called by loadFrom(); public so unit tests can drive it directly with a
    // synthetic old save. Returns true if anything was changed.
    bool migrate(uint16_t fromVersion);

private:
    PetCore() = default;
    // Pure: depends only on its argument, so it is static and callable from
    // the const derived-state accessors.
    static int16_t clampStat(int16_t val);
    void    assignRandomTraits();
    void    refreshPerks();
    uint32_t masteryRequirementForRank(uint16_t rank) const;

    PetState _state;
    int      _foodAccum = 0;   // raw food gained since last resetFoodAccum()
};
