// ── HexHound - Behavioural Form Unit Tests ───────────────────────
//
// Form is the second identity axis (P1-W1). The tests that matter here are the
// ones that prove it stays QUIET: it does not name a form off thin evidence, it
// does not oscillate on a near-tie, and it does not touch age.

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"
#include "../../src/pet/pet_forms.h"
#include "../../src/pet/pet_forms.cpp"

#include <ArduinoJson.h>

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        s_testsFailed++; return; \
    } \
} while(0)

// Build a behaviour array from six counters, in PetForm - 1 order.
static void setBehaviour(PetState& pet, uint16_t pathfinder, uint16_t guardian,
                         uint16_t archivist, uint16_t cipher,
                         uint16_t gremlin, uint16_t packmaster) {
    pet.behaviour[0] = pathfinder;
    pet.behaviour[1] = guardian;
    pet.behaviour[2] = archivist;
    pet.behaviour[3] = cipher;
    pet.behaviour[4] = gremlin;
    pet.behaviour[5] = packmaster;
}

static PetState& freshPet() {
    PetCore::instance().state() = PetState();
    return PetCore::instance().state();
}

// ── The evidence floor ────────────────────────────────────────────────────

TEST(test_new_pet_has_no_form) {
    PetState pet;
    ASSERT_EQ(pet.form, FORM_UNSET);
    for (int i = 0; i < FORM_BEHAVIOUR_COUNT; i++) ASSERT_EQ(pet.behaviour[i], 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, pet.form), FORM_UNSET);
}

TEST(test_below_the_floor_stays_unset) {
    // One behaviour leads by a mile in ratio terms and is still not enough.
    // A pet is not a Pathfinder because it took three steps.
    PetState pet;
    setBehaviour(pet, FORM_MIN_EVIDENCE - 1, 0, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, pet.form), FORM_UNSET);

    // Every counter busy but none of them past the floor: still nothing to say.
    setBehaviour(pet, 20, 18, 14, 9, 22, 11);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, pet.form), FORM_UNSET);
}

TEST(test_the_floor_is_exactly_min_evidence) {
    PetState pet;
    setBehaviour(pet, FORM_MIN_EVIDENCE, 0, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, pet.form), FORM_PATHFINDER);
}

TEST(test_lost_evidence_gives_the_form_back_up) {
    // A save whose counters were wiped can no longer support the label, so the
    // firmware stops asserting it rather than carrying a claim it cannot back.
    PetState pet;
    setBehaviour(pet, 0, 0, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_GUARDIAN), FORM_UNSET);
}

// ── The lead bar and the near-tie ─────────────────────────────────────────

TEST(test_clear_leader_takes_the_form) {
    PetState pet;
    setBehaviour(pet, 40, 200, 12, 3, 30, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, pet.form), FORM_GUARDIAN);
}

TEST(test_near_tie_from_unset_stays_unset) {
    // 100 vs 90 is a leader, but not a decisive one. FORM_LEAD_PERCENT is 140,
    // so 90 * 1.4 = 126 and 100 does not clear it.
    PetState pet;
    setBehaviour(pet, 100, 90, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_UNSET), FORM_UNSET);
}

TEST(test_lead_bar_boundary) {
    // Exactly at the bar counts as ahead; one short does not.
    PetState pet;
    setBehaviour(pet, 140, 100, 0, 0, 0, 0);      // 14000 vs 14000
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_UNSET), FORM_PATHFINDER);

    setBehaviour(pet, 139, 100, 0, 0, 0, 0);      // 13900 vs 14000
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_UNSET), FORM_UNSET);
}

// THE test for this workstream. A near-tie ticking back and forth must not
// repaint the pet's identity every 30 seconds.
TEST(test_near_tie_does_not_flip_flop_as_counters_tick) {
    PetState& pet = freshPet();

    // Walk a long way first, so Pathfinder is properly earned.
    setBehaviour(pet, 100, 71, 0, 0, 0, 0);
    ASSERT_TRUE(PetForms::refresh(pet));
    ASSERT_EQ(pet.form, FORM_PATHFINDER);

    // Now let the runner-up creep past the bar, one tick at a time, and keep
    // going well past parity. A stateless rule would drop to FORM_UNSET on the
    // very first of these and then thrash.
    for (uint16_t guard = 72; guard <= 139; guard++) {
        pet.behaviour[1] = guard;
        PetForms::refresh(pet);
        ASSERT_EQ(pet.form, FORM_PATHFINDER);
    }

    // Only a challenger that clears the same bar against the incumbent takes
    // over. 140 vs 100 is exactly that.
    pet.behaviour[1] = 140;
    ASSERT_TRUE(PetForms::refresh(pet));
    ASSERT_EQ(pet.form, FORM_GUARDIAN);

    // And the round trip is expensive on purpose: Pathfinder does not get it
    // straight back by drawing level again.
    pet.behaviour[0] = 140;
    PetForms::refresh(pet);
    ASSERT_EQ(pet.form, FORM_GUARDIAN);
}

TEST(test_interleaved_ticks_never_change_the_form_twice) {
    // Two behaviours advancing together, which is the realistic case for
    // someone who both walks and watches. The form must settle, not strobe.
    PetState& pet = freshPet();
    int changes = 0;
    PetForm last = FORM_UNSET;
    for (uint16_t i = 0; i < 400; i++) {
        PetForms::addEvidence(pet, FORM_PATHFINDER, 3);
        PetForms::addEvidence(pet, FORM_GUARDIAN, 2);
        if (pet.form != last) { changes++; last = pet.form; }
    }
    // Exactly one transition: FORM_UNSET -> FORM_PATHFINDER. 3:2 is 150%,
    // which clears 140% and then stays clear forever.
    ASSERT_EQ(changes, 1);
    ASSERT_EQ(pet.form, FORM_PATHFINDER);
}

// ── Recorders ─────────────────────────────────────────────────────────────

TEST(test_each_recorder_feeds_its_own_behaviour) {
    PetState& pet = freshPet();
    PetForms::recordWalk(1);
    PetForms::recordGuard(2);
    PetForms::recordCollect(3);
    PetForms::recordPuzzle(4);
    PetForms::recordPlay(5);
    PetForms::recordSocial(6);

    ASSERT_EQ(pet.behaviour[0], 1);
    ASSERT_EQ(pet.behaviour[1], 2);
    ASSERT_EQ(pet.behaviour[2], 3);
    ASSERT_EQ(pet.behaviour[3], 4);
    ASSERT_EQ(pet.behaviour[4], 5);
    ASSERT_EQ(pet.behaviour[5], 6);
}

TEST(test_recording_marks_the_pet_dirty) {
    PetState& pet = freshPet();
    pet.dirty = false;
    PetForms::recordWalk();
    ASSERT_TRUE(pet.dirty);
}

TEST(test_evidence_saturates_and_never_wraps) {
    PetState pet;
    pet.behaviour[0] = 0xFFF0;
    PetForms::addEvidence(pet, FORM_PATHFINDER, 100);
    ASSERT_EQ(pet.behaviour[0], 0xFFFF);
    PetForms::addEvidence(pet, FORM_PATHFINDER, 1);
    ASSERT_EQ(pet.behaviour[0], 0xFFFF);
}

TEST(test_saturated_counters_still_pick_a_leader) {
    // The overflow trap: 65535 * 140 does not fit in 16 bits, and a wrapped
    // comparison would hand the form to the wrong behaviour.
    PetState pet;
    setBehaviour(pet, 0xFFFF, 100, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_UNSET), FORM_PATHFINDER);

    setBehaviour(pet, 0xFFFF, 0xFFFF, 0, 0, 0, 0);
    ASSERT_EQ(PetForms::evaluate(pet.behaviour, FORM_UNSET), FORM_UNSET);
}

TEST(test_evidence_never_decays) {
    // There is no decay entry point at all, which is the guarantee. Time
    // passing on the pet must not move a counter.
    PetState& pet = freshPet();
    PetForms::recordWalk(200);
    for (int i = 0; i < 50; i++) PetCore::instance().decayTick();
    ASSERT_EQ(pet.behaviour[0], 200);
}

TEST(test_out_of_range_evidence_is_ignored) {
    PetState pet;
    PetForms::addEvidence(pet, FORM_UNSET, 10);
    PetForms::addEvidence(pet, FORM_COUNT, 10);
    for (int i = 0; i < FORM_BEHAVIOUR_COUNT; i++) ASSERT_EQ(pet.behaviour[i], 0);

    // A zero-weight report is a no-op, not a dirty flag.
    pet.dirty = false;
    PetForms::addEvidence(pet, FORM_PATHFINDER, 0);
    ASSERT_FALSE(pet.dirty);
}

TEST(test_null_behaviour_keeps_the_current_form) {
    ASSERT_EQ(PetForms::evaluate(nullptr, FORM_CIPHER), FORM_CIPHER);
}

// ── Age is untouched ──────────────────────────────────────────────────────

TEST(test_form_never_moves_stage_or_xp) {
    PetState& pet = freshPet();
    pet.stage = STAGE_BEACON_BEAST;
    pet.hatched = true;
    pet.stats.xp = 400;

    for (int i = 0; i < 500; i++) {
        PetForms::recordWalk(9);
        PetForms::recordPuzzle(1);
    }

    ASSERT_EQ(pet.form, FORM_PATHFINDER);
    ASSERT_EQ(pet.stage, STAGE_BEACON_BEAST);
    ASSERT_EQ((int)pet.stats.xp, 400);
}

TEST(test_stage_and_form_are_independent_axes) {
    // The whole reason form exists: same age, different pet.
    PetState a;
    PetState b;
    a.stage = b.stage = STAGE_SENTINEL;
    setBehaviour(a, 900, 10, 0, 0, 0, 0);
    setBehaviour(b, 10, 0, 0, 900, 0, 0);
    a.form = PetForms::evaluate(a.behaviour, a.form);
    b.form = PetForms::evaluate(b.behaviour, b.form);

    ASSERT_EQ(a.stage, b.stage);
    ASSERT_TRUE(a.form != b.form);
    ASSERT_STREQ(PetForms::formTitle(a.stage, a.form), "Legendary Pathfinder");
    ASSERT_STREQ(PetForms::formTitle(b.stage, b.form), "Legendary Cipher");
}

// ── Presentation ──────────────────────────────────────────────────────────

TEST(test_form_names_cover_every_form) {
    for (int f = FORM_PATHFINDER; f < FORM_COUNT; f++) {
        const char* name = PetForms::formName((PetForm)f);
        ASSERT_TRUE(name[0] != '\0');
    }
    ASSERT_STREQ(PetForms::formName(FORM_UNSET), "");
    ASSERT_STREQ(PetForms::formName(FORM_COUNT), "");
    ASSERT_STREQ(PetForms::formName(FORM_PACKMASTER), "Packmaster");
}

TEST(test_title_is_stage_adjective_plus_form) {
    ASSERT_STREQ(PetForms::formTitle(STAGE_SENTINEL, FORM_PATHFINDER),
                 "Legendary Pathfinder");
    ASSERT_STREQ(PetForms::formTitle(STAGE_PACKET_PUP, FORM_GREMLIN),
                 "Fledgling Gremlin");
}

TEST(test_unset_form_has_no_title_to_announce) {
    // Not "Legendary Nobody", not "???", not a progress hint. Nothing. The UI
    // falls back to the stage name it already had.
    ASSERT_STREQ(PetForms::formTitle(STAGE_SENTINEL, FORM_UNSET), "");
    ASSERT_STREQ(PetForms::formTitle(STAGE_EGG, FORM_UNSET), "");
}

TEST(test_title_survives_an_out_of_range_stage) {
    ASSERT_STREQ(PetForms::formTitle((PetStage)0, FORM_CIPHER), "Cipher");
    ASSERT_STREQ(PetForms::formTitle((PetStage)99, FORM_CIPHER), "Cipher");
}

TEST(test_accents_are_distinct_and_never_the_chroma_key) {
    uint16_t seen[FORM_COUNT] = {};
    for (int f = FORM_PATHFINDER; f < FORM_COUNT; f++) {
        uint16_t c = PetForms::formAccent((PetForm)f);
        ASSERT_TRUE(c != 0xF81F);          // the transparent key
        for (int g = FORM_PATHFINDER; g < f; g++) ASSERT_TRUE(seen[g] != c);
        seen[f] = c;
    }
    // FORM_UNSET still yields something drawable, so no caller has to branch.
    ASSERT_TRUE(PetForms::formAccent(FORM_UNSET) != 0xF81F);
}

TEST(test_form_index_matches_the_behaviour_array) {
    ASSERT_EQ(PetForms::formIndex(FORM_UNSET), 0xFF);
    ASSERT_EQ(PetForms::formIndex(FORM_COUNT), 0xFF);
    for (int f = FORM_PATHFINDER; f < FORM_COUNT; f++) {
        ASSERT_EQ(PetForms::formIndex((PetForm)f), f - 1);
        ASSERT_TRUE(PetForms::formIndex((PetForm)f) < FORM_BEHAVIOUR_COUNT);
    }
}

TEST(test_refresh_reports_only_real_changes) {
    PetState pet;
    setBehaviour(pet, 300, 0, 0, 0, 0, 0);
    ASSERT_TRUE(PetForms::refresh(pet));
    ASSERT_FALSE(PetForms::refresh(pet));
}

int main() {
    printf("\n=== Behavioural Form Tests ===\n");

    RUN_TEST(test_new_pet_has_no_form);
    RUN_TEST(test_below_the_floor_stays_unset);
    RUN_TEST(test_the_floor_is_exactly_min_evidence);
    RUN_TEST(test_lost_evidence_gives_the_form_back_up);

    RUN_TEST(test_clear_leader_takes_the_form);
    RUN_TEST(test_near_tie_from_unset_stays_unset);
    RUN_TEST(test_lead_bar_boundary);
    RUN_TEST(test_near_tie_does_not_flip_flop_as_counters_tick);
    RUN_TEST(test_interleaved_ticks_never_change_the_form_twice);

    RUN_TEST(test_each_recorder_feeds_its_own_behaviour);
    RUN_TEST(test_recording_marks_the_pet_dirty);
    RUN_TEST(test_evidence_saturates_and_never_wraps);
    RUN_TEST(test_saturated_counters_still_pick_a_leader);
    RUN_TEST(test_evidence_never_decays);
    RUN_TEST(test_out_of_range_evidence_is_ignored);
    RUN_TEST(test_null_behaviour_keeps_the_current_form);

    RUN_TEST(test_form_never_moves_stage_or_xp);
    RUN_TEST(test_stage_and_form_are_independent_axes);

    RUN_TEST(test_form_names_cover_every_form);
    RUN_TEST(test_title_is_stage_adjective_plus_form);
    RUN_TEST(test_unset_form_has_no_title_to_announce);
    RUN_TEST(test_title_survives_an_out_of_range_stage);
    RUN_TEST(test_accents_are_distinct_and_never_the_chroma_key);
    RUN_TEST(test_form_index_matches_the_behaviour_array);
    RUN_TEST(test_refresh_reports_only_real_changes);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
