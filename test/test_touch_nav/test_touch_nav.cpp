// ── HexHound - Touch Gesture Recognizer Unit Tests ──────────────
//
// The recognizer is the only part of touch navigation that can be tested off
// hardware: the desktop simulator has no touch panel and the FT3267 lives on a
// board. That is exactly why the gesture logic was kept free of both.
//
// Everything below feeds the recognizer the sample stream a real poll loop
// would produce (25 ms cadence, coordinates in panel pixels) and checks what
// comes out.

#include "../test_stubs.h"

#include "../../src/ui/touch_nav.h"

using namespace touchnav;

// The T-RGB tuning: 480x480 panel, thresholds as main.cpp derives them.
static GestureConfig trgbConfig() {
    GestureConfig cfg;
    cfg.tapSlop  = 480 / 16;   // 30
    cfg.swipeMin = 480 / 6;    // 80
    cfg.holdMs   = 1000;       // BUTTON_LONG_PRESS_MS
    return cfg;
}

// Feed `ms` of contact at a fixed point, 25 ms per poll, and return the first
// non-NONE gesture seen (or NONE). Mirrors the real poll cadence.
static GestureEvent holdAt(GestureRecognizer& g, int x, int y, uint32_t ms,
                           uint32_t& clock) {
    GestureEvent out;
    const uint32_t end = clock + ms;
    while (clock < end) {
        GestureEvent ev = g.update(true, x, y, clock);
        if (ev.kind != GESTURE_NONE && out.kind == GESTURE_NONE) {
            out = ev;
        }
        clock += 25;
    }
    return out;
}

// Release and settle past the grace window.
static GestureEvent releaseAt(GestureRecognizer& g, int x, int y,
                              uint32_t& clock) {
    GestureEvent out;
    for (int i = 0; i < 8; i++) {
        GestureEvent ev = g.update(false, x, y, clock);
        if (ev.kind != GESTURE_NONE && out.kind == GESTURE_NONE) {
            out = ev;
        }
        clock += 25;
    }
    return out;
}

// ── Tests ─────────────────────────────────────────────────────────────────

TEST(test_quick_contact_is_a_tap) {
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    ASSERT_EQ(holdAt(g, 240, 300, 150, t).kind, GESTURE_NONE);
    GestureEvent ev = releaseAt(g, 240, 300, t);
    ASSERT_EQ(ev.kind, GESTURE_TAP);
    ASSERT_EQ(ev.x, 240);
    ASSERT_EQ(ev.y, 300);
}

TEST(test_contact_bounce_is_not_a_tap) {
    // A single stray sample is contact noise, not a press.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    g.update(true, 240, 300, t);
    t += 25;
    ASSERT_EQ(releaseAt(g, 240, 300, t).kind, GESTURE_NONE);
}

TEST(test_long_contact_fires_hold_while_still_down) {
    // The point of firing on the way down rather than on release: "touch and
    // hold the item I want" has to do something while the finger is still on it.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    GestureEvent ev = holdAt(g, 240, 300, 1400, t);
    ASSERT_EQ(ev.kind, GESTURE_HOLD);
    ASSERT_EQ(ev.y, 300);
    // And the release that follows must NOT also produce a tap.
    ASSERT_EQ(releaseAt(g, 240, 300, t).kind, GESTURE_NONE);
}

TEST(test_hold_fires_once_only) {
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    int holds = 0;
    for (int i = 0; i < 200; i++) {          // 5 s of contact
        if (g.update(true, 240, 300, t).kind == GESTURE_HOLD) holds++;
        t += 25;
    }
    ASSERT_EQ(holds, 1);
}

TEST(test_swipe_up_and_down) {
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;

    g.update(true, 240, 400, t); t += 25;
    for (int y = 380; y >= 200; y -= 20) { g.update(true, 240, y, t); t += 25; }
    ASSERT_EQ(releaseAt(g, 240, 200, t).kind, GESTURE_SWIPE_UP);

    g.update(true, 240, 200, t); t += 25;
    for (int y = 220; y <= 400; y += 20) { g.update(true, 240, y, t); t += 25; }
    ASSERT_EQ(releaseAt(g, 240, 400, t).kind, GESTURE_SWIPE_DOWN);
}

TEST(test_swipe_reports_where_the_finger_started) {
    // A hit test wants the row the finger landed on, not the row it ended on.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    g.update(true, 240, 400, t); t += 25;
    for (int y = 380; y >= 200; y -= 20) { g.update(true, 240, y, t); t += 25; }
    ASSERT_EQ(releaseAt(g, 240, 200, t).y, 400);
}

TEST(test_travel_defeats_the_hold) {
    // A finger that wanders is not holding a row, however long it stays down.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    int holds = 0;
    for (int i = 0; i < 120; i++) {          // 3 s, drifting 1 px per poll
        if (g.update(true, 240, 300 + i, t).kind == GESTURE_HOLD) holds++;
        t += 25;
    }
    ASSERT_EQ(holds, 0);
}

TEST(test_horizontal_drag_is_ignored) {
    // Not a tap (travelled too far), not a vertical swipe. Guessing here is how
    // a thumb dragged across the glass opens a menu row.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    g.update(true, 100, 300, t); t += 25;
    for (int x = 140; x <= 380; x += 40) { g.update(true, x, 300, t); t += 25; }
    ASSERT_EQ(releaseAt(g, 380, 300, t).kind, GESTURE_NONE);
}

TEST(test_dropped_poll_does_not_split_one_contact_in_two) {
    // The FT3267 drops the occasional frame mid-contact. Believing the first
    // empty read would turn one press into a phantom tap plus a second gesture
    // out of the remainder of the same contact.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;

    g.update(true, 240, 300, t); t += 25;
    g.update(true, 240, 300, t); t += 25;
    ASSERT_EQ(g.update(false, 240, 300, t).kind, GESTURE_NONE);   // dropped frame
    t += 25;
    ASSERT_EQ(g.update(true, 240, 300, t).kind, GESTURE_NONE);    // contact resumes
    t += 25;
    ASSERT_TRUE(g.contactInProgress());

    ASSERT_EQ(releaseAt(g, 240, 300, t).kind, GESTURE_TAP);
}

TEST(test_reset_drops_a_contact_in_progress) {
    // Suppression during a roam must not park a gesture that fires the moment
    // the expedition ends and dismisses the report before it can be read.
    GestureRecognizer g(trgbConfig());
    uint32_t t = 1000;
    holdAt(g, 240, 300, 200, t);
    g.reset();
    ASSERT_FALSE(g.contactInProgress());
    ASSERT_EQ(releaseAt(g, 240, 300, t).kind, GESTURE_NONE);
}

TEST(test_thresholds_scale_with_the_panel) {
    // The same finger travel that is a swipe on the 320x172 Waveshare Touch is
    // still only a tap on the 480x480 T-RGB. Thresholds are a fraction of the
    // panel for exactly this reason.
    GestureConfig wave;
    wave.tapSlop  = 172 / 16;   // 10
    wave.swipeMin = 172 / 6;    // 28

    GestureRecognizer g(wave);
    uint32_t t = 1000;
    g.update(true, 160, 120, t); t += 25;
    for (int y = 110; y >= 80; y -= 10) { g.update(true, 160, y, t); t += 25; }
    ASSERT_EQ(releaseAt(g, 160, 80, t).kind, GESTURE_SWIPE_UP);

    GestureRecognizer g2(trgbConfig());
    uint32_t t2 = 1000;
    g2.update(true, 240, 300, t2); t2 += 25;
    for (int y = 290; y >= 260; y -= 10) { g2.update(true, 240, y, t2); t2 += 25; }
    ASSERT_EQ(releaseAt(g2, 240, 260, t2).kind, GESTURE_NONE);  // 40 px: neither
}

// ── Row hit test ──────────────────────────────────────────────────────────
// The real round-menu geometry at 480, from ui_round.h: BODY_Y = scaled(58) =
// 116, FOOTER_DIV = scaled(190) = 380, so the body is 264 px, five rows of
// scaled(26) = 52 fit, and the block is centered at y0 = 116 + (264 - 260)/2 =
// 118. Last row therefore ends at 377, three pixels clear of the footer strip
// that main.cpp treats as Back: the two zones must not overlap.

TEST(test_row_hit_test) {
    ASSERT_EQ(uiRowAtY(118, 118, 52, 5), 0);
    ASSERT_EQ(uiRowAtY(169, 118, 52, 5), 0);
    ASSERT_EQ(uiRowAtY(170, 118, 52, 5), 1);
    ASSERT_EQ(uiRowAtY(377, 118, 52, 5), 4);
    ASSERT_EQ(uiRowAtY(117, 118, 52, 5), -1);   // above the block
    ASSERT_EQ(uiRowAtY(378, 118, 52, 5), -1);   // below the last row
    ASSERT_EQ(uiRowAtY(0, 118, 52, 5), -1);
}

TEST(test_row_hit_test_rejects_degenerate_geometry) {
    // rowIndexAtY() is answered from the last draw. Before the first one there
    // has been no draw, and a menu that has never been painted must not claim a
    // row was touched.
    ASSERT_EQ(uiRowAtY(200, 0, 0, 0), -1);
    ASSERT_EQ(uiRowAtY(200, 118, 52, 0), -1);
    ASSERT_EQ(uiRowAtY(200, 118, 0, 5), -1);
}

// ── RowHitBox: the geometry a list screen recorded when it drew ───────────

TEST(test_row_hit_box_maps_touch_to_list_index) {
    // A T-RGB missions list as drawn: 5 rows of 60 px starting at y=104,
    // scrolled so the topmost drawn row is list index 3. The box returns a LIST
    // index, not a row number, which is the part a caller cannot get from
    // uiRowAtY() alone and the part that is wrong if _scroll is forgotten.
    RowHitBox rows;
    rows.note(104, 60, 5, 3);

    ASSERT_EQ(rows.indexAtY(104), 3);   // first pixel of the top row
    ASSERT_EQ(rows.indexAtY(163), 3);   // last pixel of the top row
    ASSERT_EQ(rows.indexAtY(164), 4);   // first pixel of the next row
    ASSERT_EQ(rows.indexAtY(403), 7);   // last pixel of the last row

    ASSERT_EQ(rows.indexAtY(103), -1);  // the gap under the header
    ASSERT_EQ(rows.indexAtY(404), -1);  // past the last row
    ASSERT_EQ(rows.indexAtY(0), -1);    // the header itself
}

TEST(test_row_hit_box_is_empty_until_something_is_drawn) {
    // A screen that has never drawn must not claim a row was touched: the
    // cursor would jump to a row the owner cannot see, and a hold would then
    // open it.
    RowHitBox rows;
    ASSERT_EQ(rows.indexAtY(0), -1);
    ASSERT_EQ(rows.indexAtY(240), -1);
}

TEST(test_row_hit_box_refuses_an_empty_list) {
    // The round draw paths clamp their visible count UP to 1 so an empty list
    // still paints an empty-state row. That row is not a list entry, so the hit
    // box is told zero and every touch must miss. Handing it the clamped count
    // instead would let a tap on an empty quest list select quest 0, which a
    // hold would then hand to onLongPress() to reroll.
    RowHitBox rows;
    rows.note(104, 60, 0, 0);
    ASSERT_EQ(rows.indexAtY(104), -1);
    ASSERT_EQ(rows.indexAtY(150), -1);

    // A negative count is floored rather than trusted: `total - _scroll` goes
    // negative if a list shrinks underneath a scrolled cursor.
    rows.note(104, 60, -3, 0);
    ASSERT_EQ(rows.count, 0);
    ASSERT_EQ(rows.indexAtY(104), -1);
}

TEST(test_row_hit_box_follows_a_redraw_at_new_geometry) {
    // Recording on every draw is the whole point. The round lists recompute
    // rowH from how many rows are showing, so the hit box has to follow the
    // list rather than a constant; a stale box is how a tap opens the row above
    // the one touched.
    RowHitBox rows;
    rows.note(104, 60, 5, 0);
    ASSERT_EQ(rows.indexAtY(380), 4);

    rows.note(140, 100, 2, 0);          // two rows left, so they got taller
    ASSERT_EQ(rows.indexAtY(280), 1);
    ASSERT_EQ(rows.indexAtY(380), -1);
}

int main() {
    printf("\n=== Touch Navigation Tests ===\n");

    RUN_TEST(test_quick_contact_is_a_tap);
    RUN_TEST(test_contact_bounce_is_not_a_tap);
    RUN_TEST(test_long_contact_fires_hold_while_still_down);
    RUN_TEST(test_hold_fires_once_only);
    RUN_TEST(test_swipe_up_and_down);
    RUN_TEST(test_swipe_reports_where_the_finger_started);
    RUN_TEST(test_travel_defeats_the_hold);
    RUN_TEST(test_horizontal_drag_is_ignored);
    RUN_TEST(test_dropped_poll_does_not_split_one_contact_in_two);
    RUN_TEST(test_reset_drops_a_contact_in_progress);
    RUN_TEST(test_thresholds_scale_with_the_panel);
    RUN_TEST(test_row_hit_test);
    RUN_TEST(test_row_hit_test_rejects_degenerate_geometry);
    RUN_TEST(test_row_hit_box_maps_touch_to_list_index);
    RUN_TEST(test_row_hit_box_is_empty_until_something_is_drawn);
    RUN_TEST(test_row_hit_box_refuses_an_empty_list);
    RUN_TEST(test_row_hit_box_follows_a_redraw_at_new_geometry);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
