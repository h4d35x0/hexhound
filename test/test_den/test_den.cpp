// ── HexHound - Den Placement Unit Tests ─────────────────────────
//
// The den screen's PIXELS are verified by rendering it; its RULES are verified
// here. Three of them are the reason this file exists:
//
//   1. The id-0-means-empty invariant. PetState::denSlots[] stores a raw item
//      id and uses 0 for "empty", but item id 0 is a real item (mat.scrap).
//      That pun is only safe while no den-placeable item has id 0. If somebody
//      ever edits row zero of ITEM_DEFS, every empty slot in every existing
//      save becomes a piece of scrap on the floor, silently. This suite fails
//      first.
//   2. Slot placement. A long press is the only verb the hardware has, so it
//      has to be place, swap and remove at once, and it has to be reversible
//      without a mode. The cycle is what makes that true, so the cycle is
//      pinned here in full.
//   3. Tier slot limits. TIER_CORE shows fewer slots than TIER_RICH, but the
//      whole array persists on both, so a save that moves between boards keeps
//      its room. Hiding must not be clearing.
//
// ui_den.h is included and ui_den.cpp is NOT: the rules are free inline
// functions on purpose, so they can be exercised without a display attached.
// A rule that can only be reached by drawing it is a rule that does not get
// tested.

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
#include "../../src/ui/ui_den.h"

#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != nullptr)

// Both the pet and the inventory are singletons, so every test starts from a
// known state rather than from whatever the previous one left behind.
static PetState& reset(PetStage stage = STAGE_SENTINEL) {
    Inventory::instance().resetToBaseline();
    Inventory::instance().clearAll();
    PetState& st = PetCore::instance().state();
    st.stage = stage;
    st.dirty = false;
    memset(st.denSlots, 0, sizeof(st.denSlots));
    return st;
}

static uint8_t id(const char* s) {
    return Inventory::instance().idFor(s);
}

// Grant a den cosmetic directly. Crafting it would also work and is covered by
// test_inventory; here the point is the placement rules, not how it was earned.
static uint8_t own(const char* strId) {
    const uint8_t i = id(strId);
    Inventory::instance().add(i, 1);
    return i;
}

// ── The invariant that makes 0 mean empty ─────────────────────────────────

TEST(test_item_zero_is_not_a_den_item) {
    reset();
    // The whole denSlots[] encoding rests on this one fact.
    ASSERT_TRUE(denZeroMeansEmptyIsSafe());

    // Stated the other way round, against the table itself: id 0 is a real
    // item, and it is a MATERIAL, so a 0 in denSlots[] is unambiguous.
    const ItemDef* zero = Inventory::instance().definition(0);
    ASSERT_NOT_NULL(zero);
    ASSERT_EQ(zero->kind, (uint8_t)ITEM_MATERIAL);
    ASSERT_TRUE(zero->slot != COSMETIC_DEN);
    ASSERT_FALSE(denIsPlaceable(0));
}

TEST(test_every_den_item_has_a_nonzero_id) {
    reset();
    Inventory& inv = Inventory::instance();
    uint8_t denItems = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || d->slot != COSMETIC_DEN) continue;
        denItems++;
        // This is the assertion. Everything else in the den is downstream.
        ASSERT_TRUE(i != DEN_SLOT_EMPTY);
    }
    // A table with no den items at all would make the check above vacuous.
    ASSERT_TRUE(denItems > 0);
}

TEST(test_only_den_cosmetics_are_placeable) {
    reset();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        const bool shouldBe = d && d->kind == ITEM_COSMETIC &&
                              d->slot == COSMETIC_DEN && i != DEN_SLOT_EMPTY;
        ASSERT_EQ(denIsPlaceable(i), shouldBe);
    }
    // A material, a worn cosmetic and an id off the end of the table all fail.
    ASSERT_FALSE(denIsPlaceable(id("mat.scrap")));
    ASSERT_FALSE(denIsPlaceable(id("cos.scarf")));
    ASSERT_FALSE(denIsPlaceable(id("cos.confetti")));
    ASSERT_TRUE(denIsPlaceable(id("cos.lamp")));
    ASSERT_FALSE(denIsPlaceable(ITEM_ID_NONE));
    ASSERT_FALSE(denIsPlaceable(200));
}

TEST(test_a_slot_holding_a_non_den_item_reads_as_empty) {
    PetState& st = reset();
    // Exactly what a corrupt byte or a save from a firmware with a different
    // item table looks like. It must be drawn as an empty slot, never as
    // whatever that id happens to mean here.
    st.denSlots[0] = id("mat.scrap");     // a material
    st.denSlots[1] = id("cos.goggles");   // a cosmetic, but worn not placed
    st.denSlots[2] = 200;                 // off the end of the table
    ASSERT_EQ(denItemAt(st, 0), DEN_SLOT_EMPTY);
    ASSERT_EQ(denItemAt(st, 1), DEN_SLOT_EMPTY);
    ASSERT_EQ(denItemAt(st, 2), DEN_SLOT_EMPTY);
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), 0);
}

TEST(test_slot_index_out_of_range_is_empty_not_a_read_off_the_array) {
    PetState& st = reset();
    own("cos.lamp");
    ASSERT_EQ(denItemAt(st, DEN_SLOT_COUNT), DEN_SLOT_EMPTY);
    ASSERT_EQ(denItemAt(st, 255), DEN_SLOT_EMPTY);
    ASSERT_EQ(denNextForSlot(st, DEN_SLOT_COUNT), DEN_SLOT_EMPTY);
}

// ── An empty den, owning nothing ──────────────────────────────────────────

TEST(test_a_new_pet_has_an_empty_den_and_owns_nothing_for_it) {
    PetState& st = reset();
    ASSERT_EQ(denOwnedCount(), 0);
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), 0);
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        ASSERT_EQ(denItemAt(st, i), DEN_SLOT_EMPTY);
    }
}

TEST(test_placing_with_nothing_owned_leaves_the_slot_empty) {
    PetState& st = reset();
    // Owning nothing is normal early on. The rule layer says "nothing to put
    // here" by returning empty; the screen is what turns that into a sentence.
    ASSERT_EQ(denNextForSlot(st, 0), DEN_SLOT_EMPTY);
    ASSERT_EQ(denOwnedCount(), 0);
}

TEST(test_owning_a_den_item_without_placing_it_changes_no_slot) {
    PetState& st = reset();
    own("cos.lamp");
    ASSERT_EQ(denOwnedCount(), 1);
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), 0);
    ASSERT_EQ(denItemAt(st, 0), DEN_SLOT_EMPTY);
}

// ── Placement, swapping and removal ───────────────────────────────────────

TEST(test_an_empty_slot_takes_the_first_owned_item) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    ASSERT_EQ(denNextForSlot(st, 0), lamp);
}

TEST(test_the_cycle_walks_every_owned_item_then_empties) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");    // id 12
    const uint8_t rug  = own("cos.rug");     // id 13
    const uint8_t fern = own("cos.fern");    // id 15
    ASSERT_TRUE(lamp < rug && rug < fern);

    // Empty -> lamp -> rug -> fern -> empty -> lamp. One verb, and it is
    // reversible: keep pressing and you get back where you started.
    st.denSlots[0] = DEN_SLOT_EMPTY;
    ASSERT_EQ(denNextForSlot(st, 0), lamp);
    st.denSlots[0] = lamp;
    ASSERT_EQ(denNextForSlot(st, 0), rug);
    st.denSlots[0] = rug;
    ASSERT_EQ(denNextForSlot(st, 0), fern);
    st.denSlots[0] = fern;
    ASSERT_EQ(denNextForSlot(st, 0), DEN_SLOT_EMPTY);
    st.denSlots[0] = DEN_SLOT_EMPTY;
    ASSERT_EQ(denNextForSlot(st, 0), lamp);
}

TEST(test_the_cycle_skips_an_item_standing_in_another_slot) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    const uint8_t rug  = own("cos.rug");

    // An item is one object, so it can be in one place at a time.
    st.denSlots[0] = lamp;
    ASSERT_TRUE(denIsPlacedElsewhere(st, lamp, 1));
    ASSERT_FALSE(denIsPlacedElsewhere(st, lamp, 0));
    ASSERT_EQ(denNextForSlot(st, 1), rug);
}

TEST(test_a_slot_can_always_step_off_its_own_item) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    st.denSlots[0] = lamp;
    // The only owned item is the one already here, so the only place left to
    // go is empty. If the "already placed elsewhere" filter also excluded the
    // slot's own item, this slot could never be cleared.
    ASSERT_EQ(denNextForSlot(st, 0), DEN_SLOT_EMPTY);
}

TEST(test_every_owned_item_out_leaves_a_spare_slot_empty) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    const uint8_t rug  = own("cos.rug");
    st.denSlots[0] = lamp;
    st.denSlots[1] = rug;
    // Slot 2 has nothing available: everything owned is already out. It stays
    // empty, and the screen reports DEN_NONE_FREE rather than doing nothing
    // and saying nothing.
    ASSERT_EQ(denNextForSlot(st, 2), DEN_SLOT_EMPTY);
    ASSERT_EQ(denOwnedCount(), 2);
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), 2);
}

TEST(test_a_slot_holding_something_no_longer_owned_clears_itself) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");
    st.denSlots[0] = lamp;
    Inventory::instance().remove(lamp, 1);
    // The item left the kit behind the den's back. The cycle must recover
    // rather than pin the slot to something that is gone.
    ASSERT_EQ(denNextForSlot(st, 0), DEN_SLOT_EMPTY);
}

TEST(test_the_full_array_can_be_filled) {
    PetState& st = reset();
    const char* const ALL[] = { "cos.lamp", "cos.rug", "cos.poster",
                                "cos.fern", "cos.lantern" };
    const uint8_t n = (uint8_t)(sizeof(ALL) / sizeof(ALL[0]));
    for (uint8_t i = 0; i < n; i++) own(ALL[i]);
    ASSERT_EQ(denOwnedCount(), n);

    // Walk the cycle exactly as the button would, slot by slot.
    for (uint8_t s = 0; s < DEN_SLOT_COUNT; s++) {
        st.denSlots[s] = denNextForSlot(st, s);
    }
    // Five items into eight slots: five full, three empty, no duplicates.
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), n);
    for (uint8_t a = 0; a < DEN_SLOT_COUNT; a++) {
        if (st.denSlots[a] == DEN_SLOT_EMPTY) continue;
        for (uint8_t b = (uint8_t)(a + 1); b < DEN_SLOT_COUNT; b++) {
            ASSERT_TRUE(st.denSlots[a] != st.denSlots[b]);
        }
    }
}

// ── Tier slot limits ──────────────────────────────────────────────────────

TEST(test_tier_decides_how_many_slots_are_shown) {
    // Both answers in one build. This is why denSlotCap() takes a tier
    // instead of calling Caps::tier() itself.
    ASSERT_EQ(denSlotCap(TIER_CORE), (uint8_t)DEN_SLOTS_CORE);
    ASSERT_EQ(denSlotCap(TIER_RICH), (uint8_t)DEN_SLOT_COUNT);
    ASSERT_TRUE(denSlotCap(TIER_CORE) <= denSlotCap(TIER_RICH));
    // A core board must show at least one slot, or the screen has no cursor
    // positions besides BACK.
    ASSERT_TRUE(denSlotCap(TIER_CORE) >= 1);
}

TEST(test_the_room_grows_with_the_collection_and_never_shows_dead_slots) {
    PetState& st = reset();
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) st.denSlots[i] = DEN_SLOT_EMPTY;

    // An empty den is still a ROOM: never fewer than the core count, so the
    // screen has places in it before anything has been crafted.
    ASSERT_EQ(denOwnedCount(), 0);
    ASSERT_EQ(denRoomSlots(TIER_RICH, st), (uint8_t)DEN_SLOTS_CORE);
    ASSERT_EQ(denRoomSlots(TIER_CORE, st), (uint8_t)DEN_SLOTS_CORE);

    // Owning n den items shows n + 1 slots, so there is always exactly one
    // free place to put the next thing and never a wall of brackets that no
    // amount of play could fill. This is the defect that was reported twice
    // from the board: a rich board showed eight slots when the entire game
    // contains five COSMETIC_DEN items, three of them before Beacon Beast.
    own("cos.lamp");
    own("cos.rug");
    own("cos.poster");
    ASSERT_EQ(denOwnedCount(), 3);
    ASSERT_EQ(denRoomSlots(TIER_RICH, st), (uint8_t)4);

    // A core board is still capped by its tier.
    ASSERT_EQ(denRoomSlots(TIER_CORE, st), denSlotCap(TIER_CORE));

    // And the room never grows past the array.
    for (uint8_t i = 0; i < ITEM_DEF_COUNT; i++) {
        if (denIsPlaceable(i)) Inventory::instance().add(i, 1);
    }
    ASSERT_TRUE(denRoomSlots(TIER_RICH, st) <= (uint8_t)DEN_SLOT_COUNT);
    ASSERT_TRUE(denRoomSlots(TIER_CORE, st) <= denSlotCap(TIER_CORE));
}

TEST(test_the_room_never_shrinks_over_something_standing_in_it) {
    PetState& st = reset();
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) st.denSlots[i] = DEN_SLOT_EMPTY;

    // A save furnished on a board that showed more slots than this one wants:
    // one item owned, but it is standing in the LAST slot. Sizing the room by
    // ownership alone would hide it, and a hidden item is still placed - so it
    // could never be reached again and its slot could never be reused.
    const uint8_t lamp = own("cos.lamp");
    st.denSlots[DEN_SLOT_COUNT - 1] = lamp;

    ASSERT_EQ(denHighestOccupied(st), (uint8_t)DEN_SLOT_COUNT);
    ASSERT_EQ(denRoomSlots(TIER_RICH, st), (uint8_t)DEN_SLOT_COUNT);
    ASSERT_EQ(denPlacedCount(st, denRoomSlots(TIER_RICH, st)), 1);
}

TEST(test_a_core_board_hides_the_extra_slots_and_never_clears_them) {
    PetState& st = reset();
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        st.denSlots[i] = DEN_SLOT_EMPTY;
    }
    // A room furnished on a rich board: something in a slot a core board
    // cannot show.
    const uint8_t lamp = own("cos.lamp");
    const uint8_t rug  = own("cos.rug");
    st.denSlots[0] = lamp;
    st.denSlots[DEN_SLOT_COUNT - 1] = rug;

    const uint8_t core = denSlotCap(TIER_CORE);
    const uint8_t rich = denSlotCap(TIER_RICH);

    // The core board counts only what it shows...
    ASSERT_EQ(denPlacedCount(st, core), 1);
    ASSERT_EQ(denPlacedCount(st, rich), 2);
    // ...but the byte is still there, so plugging the save back into a rich
    // board gets the whole room back.
    ASSERT_EQ(st.denSlots[DEN_SLOT_COUNT - 1], rug);

    // And a hidden item is still "placed", so the visible slots do not offer
    // it again and produce a second copy of a one-of-a-kind object.
    ASSERT_TRUE(denIsPlacedElsewhere(st, rug, 0));
    ASSERT_EQ(denNextForSlot(st, 1), DEN_SLOT_EMPTY);
}

TEST(test_placed_count_never_reads_past_the_array) {
    PetState& st = reset();
    own("cos.lamp");
    st.denSlots[DEN_SLOT_COUNT - 1] = id("cos.lamp");
    // A caller passing a larger "visible" than the array holds is clamped
    // rather than walking off the end.
    ASSERT_EQ(denPlacedCount(st, 255), 1);
    ASSERT_EQ(denPlacedCount(st, DEN_SLOT_COUNT), 1);
    ASSERT_EQ(denPlacedCount(st, 0), 0);
}

// ── The den changes no stat ───────────────────────────────────────────────

TEST(test_placing_never_touches_a_stat) {
    PetState& st = reset();
    own("cos.lamp");
    own("cos.rug");

    st.stats.hunger = 41; st.stats.mood = 42; st.stats.energy = 43;
    st.stats.xp = 44; st.stats.trust = 45; st.stats.mischief = 46;
    st.stats.health = 47;
    const PetStats before = st.stats;
    const uint32_t masteryBefore = st.masteryXP;

    for (uint8_t s = 0; s < DEN_SLOT_COUNT; s++) {
        st.denSlots[s] = denNextForSlot(st, s);
    }
    ASSERT_TRUE(denPlacedCount(st, DEN_SLOT_COUNT) > 0);   // it really did work

    ASSERT_EQ(st.stats.hunger,   before.hunger);
    ASSERT_EQ(st.stats.mood,     before.mood);
    ASSERT_EQ(st.stats.energy,   before.energy);
    ASSERT_EQ((int)st.stats.xp,  (int)before.xp);
    ASSERT_EQ(st.stats.trust,    before.trust);
    ASSERT_EQ(st.stats.mischief, before.mischief);
    ASSERT_EQ(st.stats.health,   before.health);
    ASSERT_EQ((int)st.masteryXP, (int)masteryBefore);
    ASSERT_EQ(st.stage, STAGE_SENTINEL);   // and no evolution either
}

int main() {
    printf("\n=== Den Placement Tests ===\n");

    RUN_TEST(test_item_zero_is_not_a_den_item);
    RUN_TEST(test_every_den_item_has_a_nonzero_id);
    RUN_TEST(test_only_den_cosmetics_are_placeable);
    RUN_TEST(test_a_slot_holding_a_non_den_item_reads_as_empty);
    RUN_TEST(test_slot_index_out_of_range_is_empty_not_a_read_off_the_array);

    RUN_TEST(test_a_new_pet_has_an_empty_den_and_owns_nothing_for_it);
    RUN_TEST(test_placing_with_nothing_owned_leaves_the_slot_empty);
    RUN_TEST(test_owning_a_den_item_without_placing_it_changes_no_slot);

    RUN_TEST(test_an_empty_slot_takes_the_first_owned_item);
    RUN_TEST(test_the_cycle_walks_every_owned_item_then_empties);
    RUN_TEST(test_the_cycle_skips_an_item_standing_in_another_slot);
    RUN_TEST(test_a_slot_can_always_step_off_its_own_item);
    RUN_TEST(test_every_owned_item_out_leaves_a_spare_slot_empty);
    RUN_TEST(test_a_slot_holding_something_no_longer_owned_clears_itself);
    RUN_TEST(test_the_full_array_can_be_filled);

    RUN_TEST(test_tier_decides_how_many_slots_are_shown);
    RUN_TEST(test_the_room_grows_with_the_collection_and_never_shows_dead_slots);
    RUN_TEST(test_the_room_never_shrinks_over_something_standing_in_it);
    RUN_TEST(test_a_core_board_hides_the_extra_slots_and_never_clears_them);
    RUN_TEST(test_placed_count_never_reads_past_the_array);

    RUN_TEST(test_placing_never_touches_a_stat);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
