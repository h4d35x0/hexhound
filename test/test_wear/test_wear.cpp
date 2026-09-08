// ── HexHound - Worn Cosmetic Unit Tests ─────────────────────────
//
// The closet screen's PIXELS are verified by rendering it; its RULES are
// verified here. Four of them are the reason this file exists:
//
//   1. The id-0-means-bare invariant. PetState::wornSlots[] stores a raw item
//      id and uses 0 for "bare", but item id 0 is a real item (mat.scrap).
//      That pun is only safe while no wearable item has id 0. If somebody ever
//      edits row zero of ITEM_DEFS, every bare pet in every existing save
//      starts wearing a piece of scrap, silently. This suite fails first.
//      Exactly the argument test_den makes for denSlots[], and it is repeated
//      rather than shared because the two arrays can be broken independently.
//   2. Only a COSMETIC_WORN item with a real anchor can be worn. Nothing else
//      may ever reach the array, because the renderer looks every id in it up
//      in the item table on every frame, on every screen.
//   3. The cycle. A long press is the only verb the hardware has, so it has to
//      be wear, swap and take off at once, and reversible without a mode.
//   4. An anchor is a BODY POSITION. An item belongs to exactly one, and the
//      two anchors are independent: putting a hat on must never disturb a
//      scarf. That independence is the whole reason there are two slots rather
//      than one, so it is pinned here.
//
// pet_wear.h is included and ui_closet.cpp is NOT: the rules are free inline
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
#include "../../src/pet/pet_wear.h"

static PetState& reset(PetStage stage = STAGE_SENTINEL) {
    Inventory::instance().resetToBaseline();
    Inventory::instance().clearAll();
    PetState& st = PetCore::instance().state();
    st.stage = stage;
    st.dirty = false;
    memset(st.wornSlots, 0, sizeof(st.wornSlots));
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

// ── The invariant that makes 0 mean bare ──────────────────────────────────

TEST(test_item_zero_is_not_wearable) {
    reset();
    ASSERT_TRUE(wornZeroMeansBareIsSafe());
    ASSERT_FALSE(wearIsWearable(WORN_BARE));
}

TEST(test_every_wearable_item_has_a_nonzero_id_and_a_real_anchor) {
    reset();
    Inventory& inv = Inventory::instance();
    uint8_t found = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || d->kind != ITEM_COSMETIC || d->slot != COSMETIC_WORN) continue;
        found++;
        // Nothing wearable may sit at id 0, or "bare" becomes ambiguous.
        ASSERT_TRUE(i != 0);
        // And every worn item must name a body position, or it is authored
        // into a slot the closet cannot show and the renderer cannot place.
        ASSERT_TRUE(d->anchor == WEAR_HEAD || d->anchor == WEAR_NECK);
        ASSERT_TRUE(wearIsWearable(i));
    }
    // If this ever reads zero the loop above is vacuous and proves nothing.
    ASSERT_TRUE(found >= 4);
}

TEST(test_only_worn_cosmetics_are_wearable) {
    reset();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d) continue;
        const bool shouldBe = (d->kind == ITEM_COSMETIC &&
                               d->slot == COSMETIC_WORN &&
                               d->anchor != WEAR_NONE);
        ASSERT_EQ(wearIsWearable(i), shouldBe);
    }
    // Named cases, so a change to the table cannot make the loop above agree
    // with itself while both halves are wrong.
    ASSERT_FALSE(wearIsWearable(id("mat.scrap")));
    ASSERT_FALSE(wearIsWearable(id("cos.lamp")));       // a DEN cosmetic
    ASSERT_FALSE(wearIsWearable(id("cos.confetti")));   // a FLOURISH
    ASSERT_TRUE(wearIsWearable(id("cos.antenna")));
}

TEST(test_anchor_of_a_non_worn_item_is_none) {
    reset();
    ASSERT_EQ(wearAnchorOf(id("mat.copper")), WEAR_NONE);
    ASSERT_EQ(wearAnchorOf(id("cos.rug")), WEAR_NONE);
    ASSERT_EQ(wearAnchorOf(id("cos.antenna")), WEAR_HEAD);
    ASSERT_EQ(wearAnchorOf(id("cos.scarf")), WEAR_NECK);
    ASSERT_EQ(wearAnchorOf(id("cos.goggles")), WEAR_HEAD);
    ASSERT_EQ(wearAnchorOf(id("cos.collar")), WEAR_NECK);
}

TEST(test_an_out_of_range_id_is_not_wearable_and_reads_bare) {
    PetState& st = reset();
    // Deliberately past the item table, the shape a save from a newer firmware
    // or a flipped byte would have. It must not index off the end.
    st.wornSlots[wearSlotOf(WEAR_HEAD)] = 250;
    ASSERT_FALSE(wearIsWearable(250));
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
}

// ── Wearing, swapping, taking off ─────────────────────────────────────────

TEST(test_a_new_pet_is_bare_and_owns_nothing_to_wear) {
    PetState& st = reset();
    ASSERT_EQ(wornOwnedTotal(), 0);
    ASSERT_EQ(wornCount(st), 0);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
    ASSERT_EQ(wornItemAt(st, WEAR_NECK), WORN_BARE);
    // Nothing owned means nothing to offer, so the cycle stays bare.
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), WORN_BARE);
}

TEST(test_owning_something_does_not_put_it_on) {
    PetState& st = reset();
    own("cos.antenna");
    ASSERT_EQ(wornOwnedTotal(), 1);
    // Crafting a hat must not silently dress the pet in it. The owner decides.
    ASSERT_EQ(wornCount(st), 0);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
}

TEST(test_a_bare_anchor_takes_the_first_owned_item) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), hat);
    ASSERT_TRUE(wearSet(st, WEAR_HEAD, hat));
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), hat);
    ASSERT_EQ(wornCount(st), 1);
    ASSERT_TRUE(st.dirty);
}

TEST(test_the_cycle_walks_every_owned_item_for_the_anchor_then_bare) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    const uint8_t gog = own("cos.goggles");

    // bare -> hat -> goggles -> bare -> hat ...  Item id order, which is the
    // authored order of ITEM_DEFS, so the cycle is the same every time.
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), hat);
    wearSet(st, WEAR_HEAD, hat);
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), gog);
    wearSet(st, WEAR_HEAD, gog);
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), WORN_BARE);
    wearSet(st, WEAR_HEAD, WORN_BARE);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
    ASSERT_EQ(wornNextForAnchor(st, WEAR_HEAD), hat);
}

TEST(test_the_cycle_only_offers_items_that_fit_the_anchor) {
    PetState& st = reset();
    own("cos.antenna");
    const uint8_t scarf = own("cos.scarf");
    own("cos.lamp");        // a den item; must never be offered to a body part

    // The neck cycle sees the scarf and nothing else, however much is owned.
    ASSERT_EQ(wornNextForAnchor(st, WEAR_NECK), scarf);
    wearSet(st, WEAR_NECK, scarf);
    ASSERT_EQ(wornNextForAnchor(st, WEAR_NECK), WORN_BARE);
    ASSERT_EQ(wornOwnedCount(WEAR_NECK), 1);
    ASSERT_EQ(wornOwnedCount(WEAR_HEAD), 1);
}

TEST(test_the_two_anchors_are_independent) {
    PetState& st = reset();
    const uint8_t hat   = own("cos.antenna");
    const uint8_t scarf = own("cos.scarf");
    const uint8_t gog   = own("cos.goggles");

    wearSet(st, WEAR_HEAD, hat);
    wearSet(st, WEAR_NECK, scarf);
    ASSERT_EQ(wornCount(st), 2);

    // Changing what is on the head must leave the neck exactly as it was.
    // This is the entire reason there are two slots and not one, and it is the
    // thing a single worn slot would have quietly broken.
    wearSet(st, WEAR_HEAD, gog);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), gog);
    ASSERT_EQ(wornItemAt(st, WEAR_NECK), scarf);

    wearSet(st, WEAR_HEAD, WORN_BARE);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
    ASSERT_EQ(wornItemAt(st, WEAR_NECK), scarf);
    ASSERT_EQ(wornCount(st), 1);
}

TEST(test_an_item_cannot_be_worn_on_the_wrong_anchor) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    // A hat on the neck is not a swap the UI can ask for, but wearSet() is the
    // single gate on writes and has to refuse it anyway.
    ASSERT_FALSE(wearSet(st, WEAR_NECK, hat));
    ASSERT_EQ(wornItemAt(st, WEAR_NECK), WORN_BARE);
}

TEST(test_something_not_owned_cannot_be_worn) {
    PetState& st = reset();
    const uint8_t hat = id("cos.antenna");   // known, but NOT owned
    ASSERT_FALSE(wearSet(st, WEAR_HEAD, hat));
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
}

TEST(test_an_item_no_longer_owned_reads_as_bare) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    wearSet(st, WEAR_HEAD, hat);
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), hat);

    // The byte stays, but the pet is not wearing something it does not have.
    // Reading through the rules rather than trusting the array is what keeps
    // the renderer from drawing an item the owner cannot see in their Kit.
    Inventory::instance().clearAll();
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
    ASSERT_EQ(wornCount(st), 0);
}

TEST(test_a_den_item_in_a_worn_slot_reads_as_bare) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    // The shape a corrupt byte or a save from a build with a different table
    // would have. It must be ignored, not drawn on the pet's head.
    st.wornSlots[wearSlotOf(WEAR_HEAD)] = lamp;
    ASSERT_EQ(wornItemAt(st, WEAR_HEAD), WORN_BARE);
}

TEST(test_wearing_the_same_thing_twice_changes_nothing) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    ASSERT_TRUE(wearSet(st, WEAR_HEAD, hat));
    st.dirty = false;
    // A no-op write must not mark the save dirty, or the pet writes a save
    // every time the closet is opened. Saves are the one thing on this device
    // with a wear budget.
    ASSERT_FALSE(wearSet(st, WEAR_HEAD, hat));
    ASSERT_FALSE(st.dirty);
}

TEST(test_wear_none_addresses_no_slot) {
    PetState& st = reset();
    ASSERT_EQ(wearSlotOf(WEAR_NONE), (uint8_t)WEAR_SLOT_COUNT);
    ASSERT_EQ(wornItemAt(st, WEAR_NONE), WORN_BARE);
    ASSERT_EQ(wornNextForAnchor(st, WEAR_NONE), WORN_BARE);
    ASSERT_FALSE(wearSet(st, WEAR_NONE, id("cos.antenna")));
}

TEST(test_the_slot_array_is_exactly_as_wide_as_the_anchors) {
    // config.h holds the width because pet_core.h cannot see the enum. If the
    // two ever disagree, wornSlots[] is written past its end - into whatever
    // PetState declares next, and then persisted.
    ASSERT_EQ((int)WEAR_SLOT_COUNT, (int)WEAR_ANCHOR_COUNT - 1);
    PetState& st = reset();
    ASSERT_EQ((int)sizeof(st.wornSlots), (int)WEAR_SLOT_COUNT);
    for (uint8_t a = 1; a < WEAR_ANCHOR_COUNT; a++) {
        ASSERT_TRUE(wearSlotOf((WearAnchor)a) < WEAR_SLOT_COUNT);
    }
}

// ── Evolution ─────────────────────────────────────────────────────────────

TEST(test_evolving_does_not_undress_the_pet) {
    PetState& st = reset(STAGE_PACKET_PUP);
    const uint8_t hat   = own("cos.antenna");
    const uint8_t scarf = own("cos.scarf");
    wearSet(st, WEAR_HEAD, hat);
    wearSet(st, WEAR_NECK, scarf);

    // A hat the owner put on is theirs. Taking it off because the pet grew
    // would be the firmware undoing a choice nobody asked it to undo, and the
    // anchors are authored for every stage precisely so it does not have to.
    for (int s = STAGE_EGG; s <= STAGE_SENTINEL; s++) {
        st.stage = (PetStage)s;
        ASSERT_EQ(wornItemAt(st, WEAR_HEAD), hat);
        ASSERT_EQ(wornItemAt(st, WEAR_NECK), scarf);
    }
}

// ── Nothing here touches a stat ───────────────────────────────────────────

TEST(test_wearing_never_touches_a_stat) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    const PetStats before = st.stats;
    const PetStage stage  = st.stage;

    wearSet(st, WEAR_HEAD, hat);
    wearSet(st, WEAR_HEAD, WORN_BARE);
    wearSet(st, WEAR_HEAD, hat);

    ASSERT_EQ(st.stats.hunger, before.hunger);
    ASSERT_EQ(st.stats.mood,   before.mood);
    ASSERT_EQ(st.stats.energy, before.energy);
    ASSERT_EQ(st.stats.xp,     before.xp);
    ASSERT_EQ(st.stats.trust,  before.trust);
    ASSERT_EQ(st.stage, stage);
}

int main() {
    printf("\n=== Worn Cosmetic Tests ===\n");

    RUN_TEST(test_item_zero_is_not_wearable);
    RUN_TEST(test_every_wearable_item_has_a_nonzero_id_and_a_real_anchor);
    RUN_TEST(test_only_worn_cosmetics_are_wearable);
    RUN_TEST(test_anchor_of_a_non_worn_item_is_none);
    RUN_TEST(test_an_out_of_range_id_is_not_wearable_and_reads_bare);

    RUN_TEST(test_a_new_pet_is_bare_and_owns_nothing_to_wear);
    RUN_TEST(test_owning_something_does_not_put_it_on);
    RUN_TEST(test_a_bare_anchor_takes_the_first_owned_item);
    RUN_TEST(test_the_cycle_walks_every_owned_item_for_the_anchor_then_bare);
    RUN_TEST(test_the_cycle_only_offers_items_that_fit_the_anchor);
    RUN_TEST(test_the_two_anchors_are_independent);
    RUN_TEST(test_an_item_cannot_be_worn_on_the_wrong_anchor);
    RUN_TEST(test_something_not_owned_cannot_be_worn);
    RUN_TEST(test_an_item_no_longer_owned_reads_as_bare);
    RUN_TEST(test_a_den_item_in_a_worn_slot_reads_as_bare);
    RUN_TEST(test_wearing_the_same_thing_twice_changes_nothing);
    RUN_TEST(test_wear_none_addresses_no_slot);
    RUN_TEST(test_the_slot_array_is_exactly_as_wide_as_the_anchors);

    RUN_TEST(test_evolving_does_not_undress_the_pet);
    RUN_TEST(test_wearing_never_touches_a_stat);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
