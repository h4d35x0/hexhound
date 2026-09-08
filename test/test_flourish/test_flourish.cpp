// ── HexHound - Flourish Unit Tests ──────────────────────────────
//
// The motes' PIXELS are verified by rendering them; the RULES are verified
// here. Four of them are the reason this file exists, and three are the same
// four test_wear pins one slot over, because the two arrays can be broken
// independently and a shared test would only pin whichever was edited last:
//
//   1. The id-0-means-none invariant. PetState::flourishSlot stores a raw item
//      id and uses 0 for "none", but item id 0 is a real item (mat.scrap).
//      That pun is only safe while no COSMETIC_FLOURISH item has id 0. If
//      somebody ever edits row zero of ITEM_DEFS, every save in the field
//      quietly starts showing a flourish nobody chose. This suite fails first.
//   2. Only a COSMETIC_FLOURISH item may ever reach the byte, because the
//      renderer looks it up in the item table on every frame the pet is drawn.
//   3. The cycle. A long press is the only verb the hardware has, so it has to
//      be choose, swap and turn off at once, and reversible without a mode.
//   4. ONE at a time. Unlike a worn cosmetic there is no anchor to separate two
//      of them, and the type is a single byte precisely so that "two flourishes
//      at once" is not a state the firmware can be in. Selecting a second one
//      must REPLACE the first rather than adding to it.
//
// pet_flourish.h is included and src/ui is NOT: the rules are free inline
// functions on purpose, so they can be exercised without a display attached.

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"
#include "../../src/pet/pet_inventory.h"
#include "../../src/content/content_crypto.cpp"
#include "../../src/content/content_cbor.cpp"
#include "../../src/content/content_pack.cpp"
#include "../../src/pet/pet_inventory.cpp"
#include "../../src/pet/pet_flourish.h"

static PetState& reset(PetStage stage = STAGE_SENTINEL) {
    Inventory::instance().resetToBaseline();
    Inventory::instance().clearAll();
    PetState& st = PetCore::instance().state();
    st.stage = stage;
    st.dirty = false;
    st.flourishSlot = 0;
    return st;
}

static uint8_t id(const char* s) {
    return Inventory::instance().idFor(s);
}

static uint8_t own(const char* strId) {
    const uint8_t i = id(strId);
    Inventory::instance().add(i, 1);
    return i;
}

// ── The invariant that makes 0 mean none ──────────────────────────────────

TEST(test_item_zero_is_not_a_flourish) {
    reset();
    ASSERT_TRUE(flourishZeroMeansNoneIsSafe());
    ASSERT_FALSE(flourishIsFlourish(FLOURISH_NONE));
}

TEST(test_every_flourish_item_has_a_nonzero_id) {
    reset();
    Inventory& inv = Inventory::instance();
    uint8_t found = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || d->kind != ITEM_COSMETIC || d->slot != COSMETIC_FLOURISH) {
            continue;
        }
        found++;
        // Nothing selectable may sit at id 0, or "none" becomes ambiguous.
        ASSERT_TRUE(i != 0);
        ASSERT_TRUE(flourishIsFlourish(i));
    }
    // If this ever reads zero the loop above is vacuous and proves nothing.
    ASSERT_TRUE(found >= 3);
}

TEST(test_only_flourish_cosmetics_are_selectable) {
    reset();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d) continue;
        const bool shouldBe = (d->kind == ITEM_COSMETIC &&
                               d->slot == COSMETIC_FLOURISH);
        ASSERT_EQ(flourishIsFlourish(i), shouldBe);
    }
    // Named cases, so a change to the table cannot make the loop above agree
    // with itself while both halves are wrong.
    ASSERT_FALSE(flourishIsFlourish(id("mat.scrap")));
    ASSERT_FALSE(flourishIsFlourish(id("mat.glitch")));
    ASSERT_FALSE(flourishIsFlourish(id("cos.lamp")));      // a DEN cosmetic
    ASSERT_FALSE(flourishIsFlourish(id("cos.dish")));      // a NEW den cosmetic
    ASSERT_FALSE(flourishIsFlourish(id("cos.antenna")));   // a WORN cosmetic
    ASSERT_TRUE(flourishIsFlourish(id("cos.confetti")));
    ASSERT_TRUE(flourishIsFlourish(id("cos.sparks")));
    ASSERT_TRUE(flourishIsFlourish(id("cos.aurora")));
}

TEST(test_an_out_of_range_id_is_not_a_flourish_and_reads_none) {
    PetState& st = reset();
    // Deliberately past the item table, the shape a save from a newer firmware
    // or a flipped byte would have. It must not index off the end.
    st.flourishSlot = 250;
    ASSERT_FALSE(flourishIsFlourish(250));
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
    // And 255, which is ITEM_ID_NONE itself, gets the same answer rather than
    // being mistaken for a sentinel with meaning.
    st.flourishSlot = ITEM_ID_NONE;
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
}

// ── Choosing, swapping, turning off ───────────────────────────────────────

TEST(test_a_new_pet_shows_nothing_and_owns_nothing_to_show) {
    PetState& st = reset();
    ASSERT_EQ(flourishOwnedCount(), 0);
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
    // Nothing owned means nothing to offer, so the cycle stays off.
    ASSERT_EQ(flourishNext(st), FLOURISH_NONE);
}

TEST(test_owning_something_does_not_switch_it_on) {
    PetState& st = reset();
    own("cos.confetti");
    ASSERT_EQ(flourishOwnedCount(), 1);
    // Crafting a flourish must not silently start playing it. The owner
    // decides, which is also what the v4 -> v5 migration promises.
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
}

TEST(test_nothing_selected_takes_the_first_owned_flourish) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    ASSERT_EQ(flourishNext(st), conf);
    ASSERT_TRUE(flourishSet(st, conf));
    ASSERT_EQ(flourishActive(st), conf);
    ASSERT_TRUE(st.dirty);
}

TEST(test_the_cycle_walks_every_owned_flourish_then_off) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    const uint8_t spk  = own("cos.sparks");
    const uint8_t aur  = own("cos.aurora");

    // none -> confetti -> sparks -> aurora -> none -> confetti ...  Item id
    // order, which is the authored order of ITEM_DEFS, so the cycle is the
    // same every time rather than depending on what was showing when.
    ASSERT_EQ(flourishNext(st), conf);
    flourishSet(st, conf);
    ASSERT_EQ(flourishNext(st), spk);
    flourishSet(st, spk);
    ASSERT_EQ(flourishNext(st), aur);
    flourishSet(st, aur);
    ASSERT_EQ(flourishNext(st), FLOURISH_NONE);
    flourishSet(st, FLOURISH_NONE);
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
    ASSERT_EQ(flourishNext(st), conf);
}

TEST(test_the_cycle_only_offers_flourishes) {
    PetState& st = reset();
    own("cos.antenna");     // worn
    own("cos.lamp");        // den
    own("cos.crate");       // den, the new set
    own("mat.copper");      // material
    const uint8_t spk = own("cos.sparks");

    // Whatever else is owned, the cycle sees the one flourish and nothing else.
    ASSERT_EQ(flourishOwnedCount(), 1);
    ASSERT_EQ(flourishNext(st), spk);
    flourishSet(st, spk);
    ASSERT_EQ(flourishNext(st), FLOURISH_NONE);
}

TEST(test_only_one_flourish_at_a_time) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    const uint8_t aur  = own("cos.aurora");

    ASSERT_TRUE(flourishSet(st, conf));
    ASSERT_EQ(flourishActive(st), conf);
    // Selecting a second REPLACES the first. There is no anchor to separate
    // two of them and no second byte to hold one, and this is the assertion
    // that says so out loud rather than leaving it implied by the type.
    ASSERT_TRUE(flourishSet(st, aur));
    ASSERT_EQ(flourishActive(st), aur);
    ASSERT_EQ(st.flourishSlot, aur);
}

TEST(test_something_not_owned_cannot_be_selected) {
    PetState& st = reset();
    const uint8_t conf = id("cos.confetti");
    ASSERT_FALSE(flourishSet(st, conf));
    ASSERT_EQ(st.flourishSlot, 0);
    ASSERT_FALSE(st.dirty);
}

TEST(test_a_flourish_no_longer_owned_reads_as_none) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    flourishSet(st, conf);
    ASSERT_EQ(flourishActive(st), conf);

    // A cosmetic can only leave the inventory through a future "start over",
    // but if it ever does, the pet must not keep showing it. The byte is left
    // alone on purpose - the RULES answer none, so the renderer never sees it.
    Inventory::instance().clearAll();
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
    ASSERT_EQ(st.flourishSlot, conf);
}

TEST(test_a_den_item_in_the_flourish_byte_reads_as_none) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    // A byte that is a real item but the wrong KIND of one. In range, so the
    // bound check in the loader passes it through; it is the rules that have
    // to stop it, and they have to stop it before the renderer indexes
    // FLOURISH_ART_DATA with it.
    st.flourishSlot = lamp;
    ASSERT_EQ(flourishActive(st), FLOURISH_NONE);
    ASSERT_FALSE(flourishSet(st, lamp));
}

TEST(test_selecting_the_same_thing_twice_changes_nothing) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    ASSERT_TRUE(flourishSet(st, conf));
    st.dirty = false;
    // Returns false and does NOT mark the save dirty, so holding the button on
    // a one-flourish inventory does not write flash on every press.
    ASSERT_FALSE(flourishSet(st, conf));
    ASSERT_FALSE(st.dirty);
    ASSERT_EQ(flourishActive(st), conf);
}

TEST(test_turning_it_off_when_it_is_already_off_changes_nothing) {
    PetState& st = reset();
    ASSERT_FALSE(flourishSet(st, FLOURISH_NONE));
    ASSERT_FALSE(st.dirty);
}

TEST(test_the_name_of_no_flourish_is_a_word_and_not_a_dash) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    ASSERT_TRUE(strcmp(flourishName(FLOURISH_NONE), "NONE") == 0);
    ASSERT_TRUE(strcmp(flourishName(conf), "PACKET FLURRY") == 0);
    // An id that is not a flourish gets the same honest answer as none, rather
    // than a name for something the pet is not showing.
    ASSERT_TRUE(strcmp(flourishName(250), "NONE") == 0);
    (void)st;
}

// ── It survives the things that must not disturb it ───────────────────────

TEST(test_evolving_does_not_clear_the_flourish) {
    PetState& st = reset(STAGE_GREMLIN);
    const uint8_t conf = own("cos.confetti");
    flourishSet(st, conf);

    // A flourish the owner chose is theirs, and taking it away because the pet
    // grew would be the firmware undoing a choice nobody asked it to undo.
    // Same argument test_wear makes for a hat. The stage gate in ITEM_DEFS is
    // about what can be CRAFTED, not about what may keep playing.
    for (int s = STAGE_EGG; s <= STAGE_SENTINEL; s++) {
        st.stage = (PetStage)s;
        ASSERT_EQ(flourishActive(st), conf);
    }
}

TEST(test_the_flourish_survives_a_save_and_a_reload) {
    PetState& st = reset();
    const uint8_t aur = own("cos.aurora");
    flourishSet(st, aur);

    const String json = PetCore::instance().saveToJson();
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(json.c_str()));
    ASSERT_EQ(PetCore::instance().state().flourishSlot, aur);
}

TEST(test_a_hostile_flourish_byte_cannot_survive_a_load) {
    // The bug class this is here to prevent: an id past the item table read
    // straight into the renderer, which on a board with no MMU indexes
    // FLOURISH_ART_DATA out of range rather than faulting somewhere obvious.
    // The loader bounds it, and the rules reject what is left.
    static const char* hostile =
        "{\"schema\":5,\"name\":\"x\",\"stage\":5,\"flourishSlot\":250}";
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(hostile));
    ASSERT_EQ(PetCore::instance().state().flourishSlot, 0);

    static const char* negative =
        "{\"schema\":5,\"name\":\"x\",\"stage\":5,\"flourishSlot\":-3}";
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(negative));
    ASSERT_EQ(PetCore::instance().state().flourishSlot, 0);
}

// ── Nothing here touches a stat ───────────────────────────────────────────

TEST(test_choosing_a_flourish_never_touches_a_stat) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    const PetStats before = st.stats;
    const PetStage stage  = st.stage;

    flourishSet(st, conf);
    flourishSet(st, FLOURISH_NONE);
    flourishSet(st, conf);

    ASSERT_EQ(st.stats.hunger, before.hunger);
    ASSERT_EQ(st.stats.mood,   before.mood);
    ASSERT_EQ(st.stats.energy, before.energy);
    ASSERT_EQ(st.stats.xp,     before.xp);
    ASSERT_EQ(st.stats.trust,  before.trust);
    ASSERT_EQ(st.stage, stage);
}

// ── The content the flourish and the den were extended with ───────────────

TEST(test_the_den_has_a_cosmetic_for_every_slot_it_shows) {
    reset();
    Inventory& inv = Inventory::instance();
    uint8_t denItems = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (d && d->kind == ITEM_COSMETIC && d->slot == COSMETIC_DEN) {
            denItems++;
        }
    }
    // DEN_SLOT_COUNT brackets and fewer pieces than brackets is a room that
    // tells its keeper they are not finished when they are. This is the
    // assertion that stops the two drifting apart again.
    ASSERT_TRUE(denItems >= DEN_SLOT_COUNT);
}

TEST(test_every_den_cosmetic_is_craftable_at_the_stage_that_unlocks_it) {
    reset();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || d->kind != ITEM_COSMETIC || d->slot != COSMETIC_DEN) continue;

        // A cosmetic with no recipe is unreachable content: it shows in the
        // Kit as something to want and there is no way to get it. Every den
        // piece must have a recipe, and that recipe must not be gated LATER
        // than the item, or the Kit offers a stage gate the crafting screen
        // then refuses to honour.
        bool found = false;
        for (uint8_t r = 0; r < inv.recipeCount(); r++) {
            const Recipe* rec = inv.recipe(r);
            if (!rec || rec->output != i) continue;
            found = true;
            ASSERT_TRUE(rec->minStage <= d->minStage);
        }
        ASSERT_TRUE(found);
    }
}

TEST(test_every_flourish_is_craftable) {
    reset();
    Inventory& inv = Inventory::instance();
    uint8_t checked = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || !flourishIsFlourish(i)) continue;
        checked++;
        bool found = false;
        for (uint8_t r = 0; r < inv.recipeCount(); r++) {
            const Recipe* rec = inv.recipe(r);
            if (rec && rec->output == i) { found = true; break; }
        }
        ASSERT_TRUE(found);
    }
    ASSERT_TRUE(checked >= 3);
}

int main() {
    printf("\n=== Flourish Tests ===\n");

    RUN_TEST(test_item_zero_is_not_a_flourish);
    RUN_TEST(test_every_flourish_item_has_a_nonzero_id);
    RUN_TEST(test_only_flourish_cosmetics_are_selectable);
    RUN_TEST(test_an_out_of_range_id_is_not_a_flourish_and_reads_none);

    RUN_TEST(test_a_new_pet_shows_nothing_and_owns_nothing_to_show);
    RUN_TEST(test_owning_something_does_not_switch_it_on);
    RUN_TEST(test_nothing_selected_takes_the_first_owned_flourish);
    RUN_TEST(test_the_cycle_walks_every_owned_flourish_then_off);
    RUN_TEST(test_the_cycle_only_offers_flourishes);
    RUN_TEST(test_only_one_flourish_at_a_time);
    RUN_TEST(test_something_not_owned_cannot_be_selected);
    RUN_TEST(test_a_flourish_no_longer_owned_reads_as_none);
    RUN_TEST(test_a_den_item_in_the_flourish_byte_reads_as_none);
    RUN_TEST(test_selecting_the_same_thing_twice_changes_nothing);
    RUN_TEST(test_turning_it_off_when_it_is_already_off_changes_nothing);
    RUN_TEST(test_the_name_of_no_flourish_is_a_word_and_not_a_dash);

    RUN_TEST(test_evolving_does_not_clear_the_flourish);
    RUN_TEST(test_the_flourish_survives_a_save_and_a_reload);
    RUN_TEST(test_a_hostile_flourish_byte_cannot_survive_a_load);
    RUN_TEST(test_choosing_a_flourish_never_touches_a_stat);

    RUN_TEST(test_the_den_has_a_cosmetic_for_every_slot_it_shows);
    RUN_TEST(test_every_den_cosmetic_is_craftable_at_the_stage_that_unlocks_it);
    RUN_TEST(test_every_flourish_is_craftable);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
