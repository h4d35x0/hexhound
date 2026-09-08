// ── HexHound - Closet Row Unit Tests ────────────────────────────
//
// The closet's PIXELS are verified by rendering it; its ROW MAPPING is
// verified here, and it did not exist until the FX row was added.
//
// Until then the closet had exactly one kind of row and the mapping was
// obvious: cursor 1 is HEAD, cursor 2 is NECK. Adding a third row of a
// DIFFERENT kind made it an off-by-one waiting to happen, in code that reaches
// into two different arrays - PetState::wornSlots[] indexed by anchor, and
// PetState::flourishSlot. Point the flourish row one place left and the screen
// offers to put a hat on the pet's flourish; point it one right and the FX row
// silently reads past the end of a row list the draw loop will still paint.
//
// The rules in pet_wear.h and pet_flourish.h are covered by test_wear and
// test_flourish. This file covers the layer above them: which row is which,
// what each row is showing, and where the cursor is allowed to be. That layer
// was written by hand in one sitting and had no test at all, which is the gap
// this file closes.
//
// ui_closet.h is included and ui_closet.cpp is NOT: the row rules are free
// inline functions on purpose, the same shape ui_den.h uses, so they can be
// exercised with no display attached.

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
#include "../../src/ui/ui_closet.h"

static PetState& reset(PetStage stage = STAGE_SENTINEL) {
    Inventory::instance().resetToBaseline();
    Inventory::instance().clearAll();
    PetState& st = PetCore::instance().state();
    st.stage = stage;
    st.dirty = false;
    memset(st.wornSlots, 0, sizeof(st.wornSlots));
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

// ── The row list ──────────────────────────────────────────────────────────

TEST(test_there_is_one_row_per_anchor_plus_one_for_the_flourish) {
    reset();
    ASSERT_EQ((int)CLOSET_ROWS, (int)WEAR_SLOT_COUNT + 1);
    // The draw loops iterate CLOSET_ROWS and the cursor walks the same range.
    // If these ever disagree the screen paints a row nothing can select, or
    // offers a row it does not paint.
    ASSERT_TRUE(CLOSET_ROWS > WEAR_SLOT_COUNT);
}

TEST(test_every_row_has_a_name_and_the_flourish_row_is_last) {
    reset();
    for (uint8_t r = 0; r < WEAR_SLOT_COUNT; r++) {
        // An anchor row is named after its body position, never "FX".
        ASSERT_TRUE(strcmp(closetRowName(r), "FX") != 0);
        ASSERT_TRUE(strcmp(closetRowName(r),
                           wearAnchorName((WearAnchor)(r + 1))) == 0);
    }
    ASSERT_TRUE(strcmp(closetRowName(WEAR_SLOT_COUNT), "FX") == 0);
    // Named cases, so a reordering of WearAnchor cannot leave the loop above
    // agreeing with itself while both halves moved.
    ASSERT_TRUE(strcmp(closetRowName(0), "HEAD") == 0);
    ASSERT_TRUE(strcmp(closetRowName(1), "NECK") == 0);
}

TEST(test_a_row_index_past_the_end_is_the_flourish_not_a_read_off_the_array) {
    // closetRowName and closetRowItem both use >= for the flourish, so an
    // index past the end answers FX rather than indexing wornSlots[] out of
    // range. That is the safe direction, and it is pinned so nobody "tidies"
    // it into an exact comparison.
    PetState& st = reset();
    ASSERT_TRUE(strcmp(closetRowName(200), "FX") == 0);
    ASSERT_EQ(closetRowItem(st, 200), 0);
}

// ── What a row is showing ─────────────────────────────────────────────────

TEST(test_an_empty_closet_shows_nothing_on_any_row) {
    PetState& st = reset();
    for (uint8_t r = 0; r < CLOSET_ROWS; r++) {
        ASSERT_EQ(closetRowItem(st, r), 0);
    }
}

TEST(test_each_row_reports_its_own_item_and_not_another_rows) {
    PetState& st = reset();
    const uint8_t hat   = own("cos.antenna");
    const uint8_t scarf = own("cos.scarf");
    const uint8_t conf  = own("cos.confetti");

    wearSet(st, WEAR_HEAD, hat);
    wearSet(st, WEAR_NECK, scarf);
    flourishSet(st, conf);

    ASSERT_EQ(closetRowItem(st, 0), hat);
    ASSERT_EQ(closetRowItem(st, 1), scarf);
    ASSERT_EQ(closetRowItem(st, WEAR_SLOT_COUNT), conf);

    // The three are genuinely different items, so a row returning a neighbour's
    // value would be caught rather than passing by coincidence.
    ASSERT_TRUE(hat != scarf && scarf != conf && hat != conf);
}

TEST(test_the_flourish_row_never_shows_a_worn_item) {
    PetState& st = reset();
    const uint8_t hat = own("cos.antenna");
    wearSet(st, WEAR_HEAD, hat);
    // Nothing is in the flourish slot, so the FX row is empty even though the
    // pet is wearing something. The two arrays must not bleed into each other.
    ASSERT_EQ(closetRowItem(st, WEAR_SLOT_COUNT), 0);
}

TEST(test_an_anchor_row_never_shows_the_flourish) {
    PetState& st = reset();
    const uint8_t conf = own("cos.confetti");
    flourishSet(st, conf);
    for (uint8_t r = 0; r < WEAR_SLOT_COUNT; r++) {
        ASSERT_EQ(closetRowItem(st, r), 0);
    }
    ASSERT_EQ(closetRowItem(st, WEAR_SLOT_COUNT), conf);
}

TEST(test_a_row_reads_through_the_rules_not_the_raw_byte) {
    PetState& st = reset();
    const uint8_t lamp = own("cos.lamp");     // a DEN cosmetic
    // The shape a corrupt save or a build with a different item table would
    // have. Neither row may hand the renderer an item it cannot place.
    st.wornSlots[wearSlotOf(WEAR_HEAD)] = lamp;
    st.flourishSlot = lamp;
    ASSERT_EQ(closetRowItem(st, 0), 0);
    ASSERT_EQ(closetRowItem(st, WEAR_SLOT_COUNT), 0);
}

// ── Where the cursor is allowed to be ─────────────────────────────────────

TEST(test_cursor_zero_is_back) {
    reset();
    ASSERT_EQ(closetKindForCursor(0, nullptr), CLOSET_ROW_BACK);
}

TEST(test_a_negative_cursor_is_back_not_an_anchor) {
    reset();
    // Defensive: nothing sets a negative cursor today, but this maps straight
    // into an array index and a wrap is one arithmetic slip away.
    ASSERT_EQ(closetKindForCursor(-1, nullptr), CLOSET_ROW_BACK);
    ASSERT_EQ(closetKindForCursor(-99, nullptr), CLOSET_ROW_BACK);
}

TEST(test_the_anchor_cursors_map_to_the_anchors_in_order) {
    reset();
    for (uint8_t r = 0; r < WEAR_SLOT_COUNT; r++) {
        WearAnchor a = WEAR_NONE;
        ASSERT_EQ(closetKindForCursor((int)r + 1, &a), CLOSET_ROW_ANCHOR);
        // The anchor is the enum value, which is the row index PLUS ONE
        // because WEAR_NONE occupies 0. Getting this wrong by one is the
        // whole reason this file exists.
        ASSERT_EQ(a, (WearAnchor)(r + 1));
        ASSERT_TRUE(a != WEAR_NONE);
        ASSERT_TRUE(wearSlotOf(a) < WEAR_SLOT_COUNT);
    }
}

TEST(test_the_cursor_after_the_last_anchor_is_the_flourish) {
    reset();
    ASSERT_EQ(closetKindForCursor((int)WEAR_SLOT_COUNT + 1, nullptr),
              CLOSET_ROW_FLOURISH);
}

TEST(test_a_cursor_past_the_last_row_is_back_and_not_an_anchor) {
    reset();
    // scrollDown() wraps with a modulo so this should be unreachable, but if
    // entryCount() and the draw loop ever disagree this is the branch that
    // stops a long press acting on a row that was never drawn.
    for (int c = (int)CLOSET_ROWS + 1; c < (int)CLOSET_ROWS + 6; c++) {
        WearAnchor a = WEAR_HEAD;   // deliberately not WEAR_NONE
        ASSERT_EQ(closetKindForCursor(c, &a), CLOSET_ROW_BACK);
        // And it must not have written an anchor out on the way.
        ASSERT_EQ(a, WEAR_HEAD);
    }
}

TEST(test_every_drawn_row_is_reachable_by_the_cursor) {
    reset();
    // The draw loops paint rows 0..CLOSET_ROWS-1. Every one of them must have
    // a cursor position that selects it, or the screen shows a row the owner
    // cannot act on - which is the exact shape of the bug this whole feature
    // was built to fix, one level up.
    for (uint8_t r = 0; r < CLOSET_ROWS; r++) {
        const int cursor = (int)r + 1;
        WearAnchor a = WEAR_NONE;
        const ClosetRowKind k = closetKindForCursor(cursor, &a);
        ASSERT_TRUE(k == CLOSET_ROW_ANCHOR || k == CLOSET_ROW_FLOURISH);
        if (r < WEAR_SLOT_COUNT) {
            ASSERT_EQ(k, CLOSET_ROW_ANCHOR);
        } else {
            ASSERT_EQ(k, CLOSET_ROW_FLOURISH);
        }
    }
}

TEST(test_no_cursor_position_selects_two_rows) {
    reset();
    // One position, one row. Counted rather than asserted per case, so adding
    // a row without extending the cursor range fails here.
    int anchors = 0, flourishes = 0;
    for (int c = 1; c <= (int)CLOSET_ROWS; c++) {
        WearAnchor a = WEAR_NONE;
        switch (closetKindForCursor(c, &a)) {
            case CLOSET_ROW_ANCHOR:   anchors++;   break;
            case CLOSET_ROW_FLOURISH: flourishes++; break;
            default: break;
        }
    }
    ASSERT_EQ(anchors, (int)WEAR_SLOT_COUNT);
    ASSERT_EQ(flourishes, 1);
}

// ── The counts the footer prints ──────────────────────────────────────────

TEST(test_the_footer_counts_the_flourish_as_something_you_are_wearing) {
    PetState& st = reset();
    const uint8_t hat  = own("cos.antenna");
    const uint8_t conf = own("cos.confetti");

    // This is the arithmetic UICloset::footerLeft() does. It counted only the
    // worn items at first and printed "all 2 on" with three things equipped,
    // which is the same class of wrong number the den's footer was corrected
    // for: a count that does not match what the screen is showing.
    ASSERT_EQ(wornOwnedTotal() + flourishOwnedCount(), 2);
    ASSERT_EQ(wornCount(st) + (flourishActive(st) != FLOURISH_NONE ? 1 : 0), 0);

    wearSet(st, WEAR_HEAD, hat);
    ASSERT_EQ(wornCount(st) + (flourishActive(st) != FLOURISH_NONE ? 1 : 0), 1);

    flourishSet(st, conf);
    ASSERT_EQ(wornCount(st) + (flourishActive(st) != FLOURISH_NONE ? 1 : 0), 2);
    // Everything owned is on, which is what makes the footer say "all N on".
    ASSERT_EQ(wornCount(st) + (flourishActive(st) != FLOURISH_NONE ? 1 : 0),
              wornOwnedTotal() + flourishOwnedCount());
}

int main() {
    printf("\n=== Closet Row Tests ===\n");

    RUN_TEST(test_there_is_one_row_per_anchor_plus_one_for_the_flourish);
    RUN_TEST(test_every_row_has_a_name_and_the_flourish_row_is_last);
    RUN_TEST(test_a_row_index_past_the_end_is_the_flourish_not_a_read_off_the_array);

    RUN_TEST(test_an_empty_closet_shows_nothing_on_any_row);
    RUN_TEST(test_each_row_reports_its_own_item_and_not_another_rows);
    RUN_TEST(test_the_flourish_row_never_shows_a_worn_item);
    RUN_TEST(test_an_anchor_row_never_shows_the_flourish);
    RUN_TEST(test_a_row_reads_through_the_rules_not_the_raw_byte);

    RUN_TEST(test_cursor_zero_is_back);
    RUN_TEST(test_a_negative_cursor_is_back_not_an_anchor);
    RUN_TEST(test_the_anchor_cursors_map_to_the_anchors_in_order);
    RUN_TEST(test_the_cursor_after_the_last_anchor_is_the_flourish);
    RUN_TEST(test_a_cursor_past_the_last_row_is_back_and_not_an_anchor);
    RUN_TEST(test_every_drawn_row_is_reachable_by_the_cursor);
    RUN_TEST(test_no_cursor_position_selects_two_rows);

    RUN_TEST(test_the_footer_counts_the_flourish_as_something_you_are_wearing);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
