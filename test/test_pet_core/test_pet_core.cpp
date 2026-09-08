// ── HexHound - PetCore Unit Tests ────────────────────────────────

// Shared stubs (must come before any src/ includes)
#include "../test_stubs.h"

// Real source files (native env has build_src_filter = -<*>)
#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"

#include <ArduinoJson.h>

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(test_initial_stats) {
    PetState state;
    ASSERT_EQ(state.stats.hunger, 50);
    ASSERT_EQ(state.stats.mood, 50);
    ASSERT_EQ(state.stats.energy, 100);
    ASSERT_EQ(state.stats.xp, 0);
    ASSERT_EQ(state.stats.trust, 0);
    ASSERT_EQ(state.stats.mischief, 0);
    ASSERT_EQ(state.stats.health, 100);
    ASSERT_EQ(state.stage, STAGE_EGG);
    ASSERT_FALSE(state.hatched);
}

TEST(test_hunger_decay) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    int16_t before = pet.state().stats.hunger;
    pet.decayTick();
    ASSERT_EQ(pet.state().stats.hunger, before - HUNGER_DECAY);
}

TEST(test_xp_gain) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_BRAVE;
    pet.state().traits[1] = TRAIT_BRAVE;
    uint32_t before = pet.state().stats.xp;
    pet.addXP(10);
    ASSERT_EQ(pet.state().stats.xp, before + 10);
}

TEST(test_evolution_threshold) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_BRAVE;
    pet.state().traits[1] = TRAIT_SLEEPY;
    pet.state().stats.xp = STAGE3_XP - 1;
    pet.addXP(1);
    ASSERT_EQ(pet.state().stage, STAGE_BEACON_BEAST);
}

TEST(test_trait_assignment) {
    auto& pet = PetCore::instance();
    EventBus::instance().reset();
    pet.state() = PetState();
    pet.state().hatched = false;
    pet.state().interactions = EGG_INTERACTIONS;
    pet.state().wifiScans = EGG_WIFI_SCANS;
    pet.state().usbConnects = EGG_USB_CONNECTS - 1;
    pet.recordUsbConnect();
    // checkEvolution() is triggered by addXP(), not recordUsbConnect()
    // so we call it explicitly
    pet.checkEvolution();
    EventBus::instance().processAll();
    ASSERT_TRUE(pet.state().hatched);
    ASSERT_TRUE(pet.state().traits[0] != pet.state().traits[1]);
}

TEST(test_stat_clamp) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_BRAVE;
    pet.state().traits[1] = TRAIT_SLEEPY;

    pet.feedHunger(200);
    ASSERT_EQ(pet.state().stats.hunger, STAT_MAX);

    pet.state().stats.hunger = 5;
    pet.feedHunger(-100);
    ASSERT_EQ(pet.state().stats.hunger, STAT_MIN);
}

// A save written before schema versioning existed has no "schema" key at all.
// Those saves are sitting on real devices right now, so the single thing this
// must prove is that upgrading firmware never costs someone their pet.
// Hibernation exists so that leaving a powered device untouched never costs
// the owner anything. Decay must stop dead once asleep.
TEST(test_hibernation_stops_decay) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().stats.hunger = 90;

    for (int i = 0; i < HIBERNATE_AFTER_TICKS; i++) {
        pet.decayTick();
    }
    ASSERT_TRUE(pet.isHibernating());

    const int16_t hungerAsleep = pet.state().stats.hunger;
    for (int i = 0; i < 500; i++) {
        pet.decayTick();
    }
    // Half a day of ticks while asleep must change nothing at all.
    ASSERT_EQ(pet.state().stats.hunger, hungerAsleep);
}

// Coming back must be a welcome, not a bill.
TEST(test_rouse_wakes_and_is_idempotent) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().stats.mood = 40;

    for (int i = 0; i < HIBERNATE_AFTER_TICKS; i++) {
        pet.decayTick();
    }
    ASSERT_TRUE(pet.isHibernating());

    ASSERT_TRUE(pet.rouse());              // first call actually wakes it
    ASSERT_TRUE(!pet.isHibernating());
    ASSERT_EQ((int)pet.state().idleTicks, 0);

    const int16_t moodAfterWake = pet.state().stats.mood;
    ASSERT_TRUE(!pet.rouse());             // second call reports no wake
    // and must not stack another mood bonus for the same return.
    ASSERT_EQ(pet.state().stats.mood, moodAfterWake);
}

// Any interaction is proof someone is present, so it resets the idle clock.
TEST(test_interaction_resets_idle) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    for (int i = 0; i < HIBERNATE_AFTER_TICKS - 1; i++) {
        pet.decayTick();
    }
    ASSERT_TRUE(!pet.isHibernating());
    pet.recordInteraction();
    ASSERT_EQ((int)pet.state().idleTicks, 0);

    // Having been touched, it needs the full idle window again before sleeping.
    for (int i = 0; i < HIBERNATE_AFTER_TICKS - 1; i++) {
        pet.decayTick();
    }
    ASSERT_TRUE(!pet.isHibernating());
}

// The three surfaced states are derived, never stored, so they cannot drift
// from the stats underneath them.
TEST(test_three_states_derive_from_stats) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().stats.trust  = 62;
    pet.state().stats.mood   = 80;
    pet.state().stats.health = 60;
    pet.state().stats.energy = 45;

    ASSERT_EQ(pet.bond(), 62);
    ASSERT_EQ(pet.spirit(), 70);   // (80 + 60) / 2
    ASSERT_EQ(pet.energy(), 45);
}

TEST(test_load_unversioned_legacy_save) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    // Verbatim shape of a pre-versioning save: no "schema" key.
    const char* legacy =
        "{\"name\":\"Rex\",\"stage\":4,\"hunger\":80,\"mood\":60,\"energy\":90,"
        "\"xp\":750,\"trust\":30,\"mischief\":15,\"health\":95,"
        "\"trait0\":0,\"trait1\":2,\"interactions\":42,\"wifiScans\":17,"
        "\"usbConnects\":5,\"masteryXP\":420,\"threatOpenCount\":3,"
        "\"missionsCompleted\":2,\"saveSeq\":33,\"hatched\":true}";

    ASSERT_TRUE(pet.loadFrom(legacy));

    // Everything the owner would notice must survive untouched.
    ASSERT_EQ(pet.state().stage, STAGE_GREMLIN);
    ASSERT_EQ((int)pet.state().stats.xp, 750);
    ASSERT_EQ(pet.state().stats.hunger, 80);
    ASSERT_EQ(pet.state().traits[0], TRAIT_CURIOUS);
    ASSERT_EQ(pet.state().traits[1], TRAIT_CHAOTIC);
    ASSERT_EQ(pet.state().interactions, 42);
    ASSERT_TRUE(pet.state().hatched);
    ASSERT_EQ((int)pet.state().masteryXP, 420);

    // An absent "schema" key means version 1, not "whatever is current".
    // Defaulting to current would skip every future migration step for exactly
    // the saves that need them.
    ASSERT_EQ((int)pet.state().schemaVersion, PET_SCHEMA_VERSION);

    // Round-trips back out carrying the version.
    String out = pet.saveToJson();
    JsonDocument doc;
    ASSERT_TRUE(!deserializeJson(doc, out));
    ASSERT_EQ((int)(doc["schema"] | 0), PET_SCHEMA_VERSION);
}

// migrate() must be idempotent: a current-version save is left alone and
// reports no change, so loading does not mark state dirty on every boot.
// v2 is the first migration that does real work. A v1 save has none of the
// Phase 1 fields, and must arrive with correct defaults rather than garbage.
TEST(test_migrate_v1_to_v2_defaults) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    const char* v1 =
        "{\"schema\":1,\"name\":\"Rex\",\"stage\":4,\"xp\":1200,\"hatched\":true,"
        "\"wifiScans\":300,\"interactions\":500,\"trust\":70}";

    ASSERT_TRUE(pet.loadFrom(v1));

    // The pet itself is untouched.
    ASSERT_EQ(pet.state().stage, STAGE_GREMLIN);
    ASSERT_EQ((int)pet.state().stats.xp, 1200);
    ASSERT_EQ((int)pet.state().schemaVersion, PET_SCHEMA_VERSION);

    // New axes arrive empty, not garbage.
    ASSERT_EQ((int)pet.state().roamPoints, 0);
    ASSERT_EQ((int)pet.state().stepCount, 0);
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) {
        ASSERT_EQ((int)pet.state().items[i], 0);
    }
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        ASSERT_EQ((int)pet.state().denSlots[i], 0);
    }
}

// The judgement call in the v2 migration, pinned so nobody "helpfully" changes
// it later: a long-standing pet must NOT be handed a form on the strength of
// counters accumulated when patrolling was the only thing it could do. That
// would declare almost every existing pet a Pathfinder, an identity its owner
// never chose and cannot see the reason for.
TEST(test_migrate_v1_does_not_fabricate_a_form) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    // A heavily used v1 pet: 300 patrols, 500 interactions.
    const char* veteran =
        "{\"schema\":1,\"name\":\"Vet\",\"stage\":5,\"xp\":5000,\"hatched\":true,"
        "\"wifiScans\":300,\"interactions\":500,\"seenWifi\":[\"a\",\"b\",\"c\"]}";

    ASSERT_TRUE(pet.loadFrom(veteran));

    ASSERT_EQ(pet.state().form, FORM_UNSET);
    for (uint8_t i = 0; i < FORM_BEHAVIOUR_COUNT; i++) {
        ASSERT_EQ((int)pet.state().behaviour[i], 0);
    }
}

// Arrays are read by index, so a save written by a build with LONGER arrays
// must not overflow ours. This is the shape of bug that corrupts adjacent
// fields and shows up as something unrelated.
TEST(test_oversized_arrays_do_not_overflow) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    String json = "{\"schema\":2,\"name\":\"Big\",\"stage\":2,\"items\":[";
    for (int i = 0; i < ITEM_TYPE_COUNT + 40; i++) {
        json += (i ? ",1" : "1");
    }
    json += "],\"denSlots\":[";
    for (int i = 0; i < DEN_SLOT_COUNT + 40; i++) {
        json += (i ? ",2" : "2");
    }
    json += "],\"behaviour\":[9,9,9,9,9,9,9,9,9,9,9,9]}";

    ASSERT_TRUE(pet.loadFrom(json.c_str()));
    // Read what fits, ignore the rest, corrupt nothing.
    ASSERT_EQ((int)pet.state().items[ITEM_TYPE_COUNT - 1], 1);
    ASSERT_EQ((int)pet.state().denSlots[DEN_SLOT_COUNT - 1], 2);
    ASSERT_EQ((int)pet.state().behaviour[FORM_BEHAVIOUR_COUNT - 1], 9);
}

// A short array must leave the remaining entries zeroed rather than stale.
TEST(test_short_arrays_zero_the_remainder) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) pet.state().items[i] = 77;

    ASSERT_TRUE(pet.loadFrom("{\"schema\":2,\"name\":\"S\",\"stage\":2,\"items\":[5,6]}"));
    ASSERT_EQ((int)pet.state().items[0], 5);
    ASSERT_EQ((int)pet.state().items[1], 6);
    ASSERT_EQ((int)pet.state().items[2], 0);
    ASSERT_EQ((int)pet.state().items[ITEM_TYPE_COUNT - 1], 0);
}

TEST(test_phase1_state_roundtrips) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().form = FORM_CIPHER;
    pet.state().behaviour[3] = 140;
    pet.state().items[7] = 42;
    pet.state().denSlots[2] = 5;
    pet.state().roamPoints = 900;
    pet.state().stepCount = 12345;
    pet.state().roamSessions = 8;
    pet.state().lastReportDay = 3;

    String json = pet.saveToJson();
    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom(json.c_str()));

    ASSERT_EQ(pet.state().form, FORM_CIPHER);
    ASSERT_EQ((int)pet.state().behaviour[3], 140);
    ASSERT_EQ((int)pet.state().items[7], 42);
    ASSERT_EQ((int)pet.state().denSlots[2], 5);
    ASSERT_EQ((int)pet.state().roamPoints, 900);
    ASSERT_EQ((int)pet.state().stepCount, 12345);
    ASSERT_EQ((int)pet.state().roamSessions, 8);
    ASSERT_EQ((int)pet.state().lastReportDay, 3);
}

TEST(test_migrate_is_noop_at_current_version) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().stats.xp = 123;

    ASSERT_TRUE(!pet.migrate(PET_SCHEMA_VERSION));
    ASSERT_EQ((int)pet.state().schemaVersion, PET_SCHEMA_VERSION);
    ASSERT_EQ((int)pet.state().stats.xp, 123);
}

TEST(test_save_load) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_GREMLIN;
    pet.state().stats.xp = 750;
    pet.state().stats.hunger = 80;
    pet.state().masteryXP = 420;
    pet.state().masteryRank = 2;
    pet.state().threatOpenCount = 3;
    pet.state().threatDuplicateCount = 1;
    pet.state().threatTrackerCount = 4;
    pet.state().missionsCompleted = 2;
    pet.state().perkMask = PERK_SIGNAL_CARTOGRAPHER | PERK_ANOMALY_ARCHIVIST;
    pet.state().traits[0] = TRAIT_CURIOUS;
    pet.state().traits[1] = TRAIT_CHAOTIC;
    pet.markWifiSeen("TestNet");
    pet.markBleSeen("AA:BB:CC:DD:EE:FF");
    strlcpy(pet.state().name, "TestPet", sizeof(pet.state().name));

    String json = pet.saveToJson();
    ASSERT_TRUE(json.length() > 0);

    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom(json.c_str()));

    ASSERT_EQ(pet.state().stage, STAGE_GREMLIN);
    ASSERT_EQ(pet.state().stats.xp, 750);
    ASSERT_EQ(pet.state().stats.hunger, 80);
    ASSERT_EQ(pet.state().masteryXP, 420);
    ASSERT_EQ(pet.state().masteryRank, 2);
    ASSERT_EQ(pet.state().threatOpenCount, 3);
    ASSERT_EQ(pet.state().threatDuplicateCount, 1);
    ASSERT_EQ(pet.state().threatTrackerCount, 4);
    ASSERT_EQ(pet.state().missionsCompleted, 2);
    ASSERT_EQ(pet.state().traits[0], TRAIT_CURIOUS);
    ASSERT_EQ(pet.state().traits[1], TRAIT_CHAOTIC);
    ASSERT_EQ(pet.state().seenWifiCount, 1);
    ASSERT_EQ(pet.state().seenBleCount, 1);
    ASSERT_TRUE(strcmp(pet.state().seenWifi[0], "TestNet") == 0);
    ASSERT_TRUE(strcmp(pet.state().seenBle[0], "AA:BB:CC:DD:EE:FF") == 0);
    ASSERT_TRUE(strcmp(pet.state().name, "TestPet") == 0);
}

TEST(test_mastery_rank_progression) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_SENTINEL;

    pet.addMasteryXP(80);
    ASSERT_EQ(pet.state().masteryRank, 1);
    ASSERT_EQ(pet.masteryToNextRank(), 160);

    pet.addMasteryXP(160);
    ASSERT_EQ(pet.state().masteryRank, 2);
    ASSERT_TRUE(strcmp(pet.masteryTitle(), "WATCHER") == 0);
}

// The migration tests so far all feed WELL-FORMED saves. Real data loss comes
// from the malformed ones, so these are the adversarial cases: a save must
// never be able to drive the pet into a state the firmware cannot represent.
TEST(test_hostile_saves_cannot_corrupt_state) {
    auto& pet = PetCore::instance();

    struct Case { const char* json; const char* what; };
    const Case cases[] = {
        // A form ordinal past the end of the enum. Casting it straight in
        // would index formName()/formAccent() out of bounds on the next draw.
        { "{\"schema\":2,\"name\":\"A\",\"stage\":2,\"form\":250}", "form out of range" },
        // A negative-looking form.
        { "{\"schema\":2,\"name\":\"B\",\"stage\":2,\"form\":-3}", "negative form" },
        // A stage past the end. STAGE_NAMES is indexed by stage-1.
        { "{\"schema\":2,\"name\":\"C\",\"stage\":99}", "stage out of range" },
        { "{\"schema\":2,\"name\":\"D\",\"stage\":0}", "stage zero" },
        // A schema from the future: this firmware cannot know what it means.
        { "{\"schema\":99,\"name\":\"E\",\"stage\":2}", "future schema" },
        // Wrong types where numbers belong.
        { "{\"schema\":2,\"name\":\"F\",\"stage\":\"two\",\"xp\":\"lots\"}", "string for number" },
        { "{\"schema\":2,\"name\":\"G\",\"stage\":2,\"items\":\"nope\"}", "string for array" },
        { "{\"schema\":2,\"name\":\"H\",\"stage\":2,\"behaviour\":{\"a\":1}}", "object for array" },
        // Array of wrong-typed entries.
        { "{\"schema\":2,\"name\":\"I\",\"stage\":2,\"denSlots\":[\"x\",\"y\"]}", "string array entries" },
    };

    for (const Case& c : cases) {
        pet.state() = PetState();
        // Whether it loads or is rejected is not the assertion. The assertion
        // is that afterwards the state is always representable, because the
        // draw paths index tables with these values.
        pet.loadFrom(c.json);

        const PetState& s = pet.state();
        ASSERT_TRUE(s.form < FORM_COUNT);
        ASSERT_TRUE(s.stage >= STAGE_EGG && s.stage <= STAGE_SENTINEL);
        for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
            // A den slot must be empty or a real item id, never past the table.
            ASSERT_TRUE(s.denSlots[i] < ITEM_TYPE_COUNT);
        }
    }
}

TEST(test_save_load_invalid_json) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_GREMLIN;
    pet.state().stats.xp = 750;

    ASSERT_FALSE(pet.loadFrom("{not valid json"));
    ASSERT_EQ(pet.state().stage, STAGE_GREMLIN);
    ASSERT_EQ(pet.state().stats.xp, 750);
}

TEST(test_seen_capture_dedup) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    ASSERT_TRUE(pet.markWifiSeen("SameNet"));
    ASSERT_FALSE(pet.markWifiSeen("SameNet"));
    ASSERT_TRUE(pet.markBleSeen("11:22:33:44:55:66"));
    ASSERT_FALSE(pet.markBleSeen("11:22:33:44:55:66"));
    ASSERT_EQ(pet.state().seenWifiCount, 1);
    ASSERT_EQ(pet.state().seenBleCount, 1);
}

// The table used to fill at MAX_SEEN_WIFI_CAPTURES and then refuse everything
// forever, so a pet that had met 64 networks could never register another one
// and patrol XP collapsed permanently. It is a ring now: oldest out, newest in.
//
// Written so it FAILS against the old code - the assertions that matter are
// that a 65th distinct identifier is ACCEPTED, and that the oldest is what
// left. A test that only checked the count would have passed either way.
TEST(test_seen_capture_evicts_oldest_when_full) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();

    char name[16];
    for (int i = 0; i < MAX_SEEN_WIFI_CAPTURES; i++) {
        snprintf(name, sizeof(name), "net%d", i);
        ASSERT_TRUE(pet.markWifiSeen(name));
    }
    ASSERT_EQ(pet.state().seenWifiCount, MAX_SEEN_WIFI_CAPTURES);
    ASSERT_TRUE(strcmp(pet.state().seenWifi[0], "net0") == 0);

    // The 65th. Under the old code this returned false and nothing was learned.
    ASSERT_TRUE(pet.markWifiSeen("brand_new"));
    ASSERT_EQ(pet.state().seenWifiCount, MAX_SEEN_WIFI_CAPTURES);

    // net0 was pushed out; net1 is oldest now and the newcomer is last.
    ASSERT_TRUE(strcmp(pet.state().seenWifi[0], "net1") == 0);
    ASSERT_TRUE(strcmp(pet.state().seenWifi[MAX_SEEN_WIFI_CAPTURES - 1],
                       "brand_new") == 0);

    // Evicted means forgotten, so meeting it again counts as new...
    ASSERT_TRUE(pet.markWifiSeen("net0"));
    // ...and something still in memory does not.
    ASSERT_FALSE(pet.markWifiSeen("brand_new"));

    // The BLE table is a separate instantiation of the same helper with a
    // different slot width, so it gets its own check rather than an assumption.
    pet.state() = PetState();
    char addr[24];
    for (int i = 0; i < MAX_SEEN_BLE_CAPTURES; i++) {
        snprintf(addr, sizeof(addr), "aa:bb:cc:00:00:%02x", i);
        ASSERT_TRUE(pet.markBleSeen(addr));
    }
    ASSERT_TRUE(pet.markBleSeen("ff:ff:ff:ff:ff:ff"));
    ASSERT_EQ(pet.state().seenBleCount, MAX_SEEN_BLE_CAPTURES);
    ASSERT_TRUE(strcmp(pet.state().seenBle[0], "aa:bb:cc:00:00:01") == 0);
}

int main() {
    printf("\n=== PetCore Tests ===\n");
    EventBus::instance().reset();

    RUN_TEST(test_initial_stats);
    RUN_TEST(test_hunger_decay);
    RUN_TEST(test_xp_gain);
    RUN_TEST(test_evolution_threshold);
    RUN_TEST(test_trait_assignment);
    RUN_TEST(test_stat_clamp);
    RUN_TEST(test_hibernation_stops_decay);
    RUN_TEST(test_rouse_wakes_and_is_idempotent);
    RUN_TEST(test_interaction_resets_idle);
    RUN_TEST(test_three_states_derive_from_stats);
    RUN_TEST(test_load_unversioned_legacy_save);
    RUN_TEST(test_migrate_v1_to_v2_defaults);
    RUN_TEST(test_migrate_v1_does_not_fabricate_a_form);
    RUN_TEST(test_oversized_arrays_do_not_overflow);
    RUN_TEST(test_short_arrays_zero_the_remainder);
    RUN_TEST(test_phase1_state_roundtrips);
    RUN_TEST(test_migrate_is_noop_at_current_version);
    RUN_TEST(test_save_load);
    RUN_TEST(test_hostile_saves_cannot_corrupt_state);
    RUN_TEST(test_save_load_invalid_json);
    RUN_TEST(test_seen_capture_dedup);
    RUN_TEST(test_seen_capture_evicts_oldest_when_full);
    RUN_TEST(test_mastery_rank_progression);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
