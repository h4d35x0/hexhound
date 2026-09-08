// ── HexHound - PetMemory Unit Tests ──────────────────────────────

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"
#include "../../src/content/content_store.h"
#include "../../src/content/content_crypto.cpp"
#include "../../src/content/content_cbor.cpp"
#include "../../src/content/content_pack.cpp"
#include "../../src/content/content_store.cpp"
#include "../../src/content/dialogue_engine.h"
#include "../../src/content/dialogue_engine.cpp"
#include "../../src/pet/pet_memory.h"
#include "../../src/pet/pet_memory.cpp"

#include <ArduinoJson.h>

// Every slot the accessor claims to serve must come from a real counter.
TEST(test_accessor_maps_slots_to_real_counters) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().checkIns             = 12;
    pet.state().wifiScans            = 41;
    pet.state().seenWifiCount        = 23;
    pet.state().threatOpenCount      = 3;
    pet.state().threatDuplicateCount = 2;
    pet.state().threatTrackerCount   = 5;
    pet.state().questsCompleted      = 7;
    pet.state().bestGameScore        = 1840;

    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_VISITS, nullptr), 12);
    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_PATROLS, nullptr), 41);
    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_NEW_NETWORKS, nullptr), 23);
    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_THREATS, nullptr), 10);  // 3+2+5
    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_QUESTS_DONE, nullptr), 7);
    ASSERT_EQ((int)PetMemory::accessor(MEM_SLOT_BEST_SCORE, nullptr), 1840);
}

// The one that matters. There is no RTC, so days-away is unknowable and must
// be declared unavailable rather than answered.
TEST(test_unknowable_slot_is_declared_unavailable) {
    const uint32_t mask = PetMemory::unavailableSlotMask();
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_DAYS_AWAY)) != 0);

    // Everything else must be answerable, or the pet has less to say than it
    // could. This catches a slot being quietly disabled instead of sourced.
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_VISITS)) == 0);
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_PATROLS)) == 0);
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_NEW_NETWORKS)) == 0);
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_THREATS)) == 0);
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_QUESTS_DONE)) == 0);
    ASSERT_TRUE((mask & (1UL << MEM_SLOT_BEST_SCORE)) == 0);
}

// End to end: an unavailable slot must make its line ineligible, so the engine
// renders nothing rather than rendering the absence line with a filler number.
TEST(test_unavailable_slot_suppresses_its_line) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    ContentStore::instance().begin();
    DialogueEngine::instance().begin(1234);
    DialogueEngine::instance().setMemoryAccessor(PetMemory::accessor, nullptr);
    DialogueEngine::instance().setUnavailableSlots(PetMemory::unavailableSlotMask());

    // The only baseline line for this context asks for MEM_SLOT_DAYS_AWAY,
    // which this hardware cannot supply, so there must be nothing to say.
    const char* line = DialogueEngine::instance().pick(
        DLG_RETURN_AFTER_ABSENCE, TRAIT_CURIOUS, TRAIT_BRAVE, STAGE_GREMLIN);
    ASSERT_TRUE(line == nullptr);

    DialogueEngine::instance().setUnavailableSlots(0);
}

// A maximum-valued counter is a real number, not a missing one. This is the
// regression that killed the in-band sentinel approach: 0xFFFFFFFF is a
// legitimate best score, and treating it as "unavailable" would silently
// delete the line of a player who earned it.
TEST(test_max_value_counter_is_not_treated_as_missing) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().bestGameScore = 0xFFFFFFFFUL;

    ASSERT_TRUE(PetMemory::accessor(MEM_SLOT_BEST_SCORE, nullptr) == 0xFFFFFFFFUL);
    ASSERT_TRUE((PetMemory::unavailableSlotMask() & (1UL << MEM_SLOT_BEST_SCORE)) == 0);
}

// A slot that IS available still substitutes normally, so the decline path has
// not simply broken substitution for everyone.
TEST(test_available_slot_still_substitutes) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().checkIns = 17;

    ContentStore::instance().begin();
    DialogueEngine::instance().begin(99);
    DialogueEngine::instance().setMemoryAccessor(PetMemory::accessor, nullptr);

    // Deliberately traits with NO dedicated greeting line in the baseline.
    // A trait-tagged line outranks a generic one by design, and the baseline
    // tags greetings for CURIOUS and SLEEPY, so asking as either of those
    // would win with the tagged line every time and the generic line carrying
    // {n} would never come up. That is the scoring rule working, not a bug.
    bool sawNumber = false;
    for (int i = 0; i < 40 && !sawNumber; i++) {
        const char* line = DialogueEngine::instance().pick(
            DLG_GREETING, TRAIT_BRAVE, TRAIT_GREEDY, STAGE_GREMLIN);
        if (line && strstr(line, "17") != nullptr) sawNumber = true;
    }
    ASSERT_TRUE(sawNumber);
}

TEST(test_best_score_keeps_only_the_record) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    ASSERT_TRUE(PetMemory::recordGameScore(100));
    ASSERT_EQ((int)pet.state().bestGameScore, 100);
    ASSERT_TRUE(!PetMemory::recordGameScore(50));      // worse round, no change
    ASSERT_EQ((int)pet.state().bestGameScore, 100);
    ASSERT_TRUE(!PetMemory::recordGameScore(100));     // ties do not beat
    ASSERT_TRUE(PetMemory::recordGameScore(101));
    ASSERT_EQ((int)pet.state().bestGameScore, 101);
}

TEST(test_counters_saturate_rather_than_wrap) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().checkIns        = 0xFFFF;
    pet.state().questsCompleted = 0xFFFF;

    PetMemory::recordCheckIn();
    PetMemory::recordQuestComplete();

    // Wrapping to zero would make a long-lived pet claim it had never met you.
    ASSERT_EQ((int)pet.state().checkIns, 0xFFFF);
    ASSERT_EQ((int)pet.state().questsCompleted, 0xFFFF);
}

// The new counters are optional fields, so an older save must still load and
// simply start them at zero.
TEST(test_memory_counters_roundtrip_and_default) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().checkIns        = 5;
    pet.state().questsCompleted = 3;
    pet.state().bestGameScore   = 900;

    String json = pet.saveToJson();
    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom(json.c_str()));
    ASSERT_EQ((int)pet.state().checkIns, 5);
    ASSERT_EQ((int)pet.state().questsCompleted, 3);
    ASSERT_EQ((int)pet.state().bestGameScore, 900);

    // A save predating these fields carries none of them.
    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom("{\"name\":\"Old\",\"stage\":2,\"xp\":10}"));
    ASSERT_EQ((int)pet.state().checkIns, 0);
    ASSERT_EQ((int)pet.state().questsCompleted, 0);
    ASSERT_EQ((int)pet.state().bestGameScore, 0);
}

int main() {
    printf("\n=== PetMemory Tests ===\n");
    EventBus::instance().reset();

    RUN_TEST(test_accessor_maps_slots_to_real_counters);
    RUN_TEST(test_unknowable_slot_is_declared_unavailable);
    RUN_TEST(test_unavailable_slot_suppresses_its_line);
    RUN_TEST(test_max_value_counter_is_not_treated_as_missing);
    RUN_TEST(test_available_slot_still_substitutes);
    RUN_TEST(test_best_score_keeps_only_the_record);
    RUN_TEST(test_counters_saturate_rather_than_wrap);
    RUN_TEST(test_memory_counters_roundtrip_and_default);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
