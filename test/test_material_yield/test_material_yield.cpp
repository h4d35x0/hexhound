// ── HexHound - Material Yield Unit Tests ────────────────────────
//
// The economy is a pure function of plain counters, deliberately free of the
// roam module, the radios and the display, so all of it is exercisable here.
//
// The test that matters most is coverage: every craftable material has to be
// reachable on a board that CANNOT count steps, because the T-RGB cannot and
// four of the nine recipes would otherwise be impossible on it forever.

#include "../test_stubs.h"

#include "../../src/content/material_yield.h"

#include <string.h>

namespace {

bool grants(const MaterialGrant* g, uint8_t n, const char* item) {
    for (uint8_t i = 0; i < n; i++) {
        if (g[i].item && strcmp(g[i].item, item) == 0) return true;
    }
    return false;
}

uint32_t qtyOf(const MaterialGrant* g, uint8_t n, const char* item) {
    uint32_t q = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (g[i].item && strcmp(g[i].item, item) == 0) q += g[i].qty;
    }
    return q;
}

// The eight craftable materials, exactly as item_defs.h names them.
const char* const ALL_MATERIALS[] = {
    "mat.scrap", "mat.signal", "mat.circuit", "mat.shard",
    "mat.glitch", "mat.copper", "mat.lens",   "mat.ferrite"
};
constexpr int MATERIAL_N = (int)(sizeof(ALL_MATERIALS) / sizeof(ALL_MATERIALS[0]));

}  // namespace

TEST(test_nothing_from_nothing) {
    // A patrol that saw no networks, a roam that went nowhere and an abandoned
    // game must all pay zero. Rounding up here would mint free materials for
    // standing still.
    MaterialGrant g[MATERIAL_GRANT_MAX];
    ASSERT_EQ(matyield::patrolYield(0, 0, 0, g), 0);
    ASSERT_EQ(matyield::roamYield(0, 0, 0, 0, false, g), 0);
    ASSERT_EQ(matyield::gameYield(0, true, g), 0);
}

TEST(test_below_a_divisor_yields_nothing) {
    MaterialGrant g[MATERIAL_GRANT_MAX];
    // Five networks against a divisor of six.
    ASSERT_EQ(matyield::patrolYield(5, 0, 0, g), 0);
    // One more crosses it.
    ASSERT_EQ(matyield::patrolYield(6, 0, 0, g), 1);
    ASSERT_EQ((int)qtyOf(g, 1, "mat.scrap"), 1);
}

TEST(test_abandoned_round_pays_nothing) {
    // Same rule the quest progress already follows: entering and leaving a game
    // must not farm anything, however high the score got.
    MaterialGrant g[MATERIAL_GRANT_MAX];
    ASSERT_EQ(matyield::gameYield(100000, false, g), 0);
    ASSERT_TRUE(matyield::gameYield(100000, true, g) > 0);
}

TEST(test_every_material_is_reachable_without_an_imu) {
    // THE load-bearing test. A board that cannot count steps must still be able
    // to obtain all eight, or its Beacon Beast recipes are dead content.
    bool seen[MATERIAL_N] = { false };
    MaterialGrant g[MATERIAL_GRANT_MAX];

    uint8_t n = matyield::patrolYield(600, 600, 600, g);
    for (int m = 0; m < MATERIAL_N; m++) if (grants(g, n, ALL_MATERIALS[m])) seen[m] = true;

    // stepsMeasured = false: the case the T-RGB is actually in.
    n = matyield::roamYield(100000, 60000, 60000, 0, false, g);
    for (int m = 0; m < MATERIAL_N; m++) if (grants(g, n, ALL_MATERIALS[m])) seen[m] = true;

    n = matyield::gameYield(100000, true, g);
    for (int m = 0; m < MATERIAL_N; m++) if (grants(g, n, ALL_MATERIALS[m])) seen[m] = true;

    for (int m = 0; m < MATERIAL_N; m++) {
        if (!seen[m]) {
            ::printf("  unreachable without an IMU: %s\n", ALL_MATERIALS[m]);
        }
        ASSERT_TRUE(seen[m]);
    }
}

TEST(test_steps_only_accelerate_they_do_not_gate) {
    // Whatever a step count adds must ALSO be obtainable without one, which is
    // what stops a material hiding behind hardware half the fleet lacks.
    MaterialGrant with[MATERIAL_GRANT_MAX];
    MaterialGrant without[MATERIAL_GRANT_MAX];
    const uint8_t nw = matyield::roamYield(100000, 60000, 60000, 50000, true, with);
    const uint8_t no = matyield::roamYield(100000, 60000, 60000, 50000, false, without);

    for (uint8_t i = 0; i < nw; i++) {
        ASSERT_TRUE(grants(without, no, with[i].item));
    }
    // And it really is an accelerator: more copper with steps than without.
    ASSERT_TRUE(qtyOf(with, nw, "mat.copper") > qtyOf(without, no, "mat.copper"));
}

TEST(test_a_roam_outpays_a_patrol_on_the_same_discoveries) {
    // The expedition has to be worth the walk, or nobody would ever roam.
    MaterialGrant r[MATERIAL_GRANT_MAX];
    MaterialGrant p[MATERIAL_GRANT_MAX];
    const uint8_t rn = matyield::roamYield(1000, 40, 24, 0, false, r);
    const uint8_t pn = matyield::patrolYield(40, 40, 24, p);
    ASSERT_TRUE(rn > pn);
}

TEST(test_never_overruns_the_grant_table) {
    // Every caller passes a MATERIAL_GRANT_MAX buffer; the widest table is the
    // roam with its step bonus, and it must still fit.
    MaterialGrant g[MATERIAL_GRANT_MAX];
    const uint8_t n = matyield::roamYield(0xFFFFFFFFu, 0xFFFF, 0xFFFF,
                                          0xFFFFFFFFu, true, g);
    ASSERT_TRUE(n <= MATERIAL_GRANT_MAX);
    // A count that saturates must clamp, not wrap into a small number.
    for (uint8_t i = 0; i < n; i++) ASSERT_TRUE(g[i].qty > 0);
}

TEST(test_every_activity_pays_something_a_young_pet_can_spend) {
    // Found by playing, not by reading: the first cut paid a minigame only
    // circuit and glitch, and BOTH are Beacon Beast tier - circuit is in
    // goggles, collar, fern and lantern, glitch only in collar. So a Packet Pup
    // grinding games banked a currency it could not spend on anything it owned,
    // which is worse than earning nothing because it looks like progress.
    //
    // These are the materials the five Packet Pup recipes actually ask for.
    const char* const EARLY[] = {
        "mat.scrap", "mat.copper", "mat.signal", "mat.ferrite", "mat.shard"
    };
    const int EARLY_N = (int)(sizeof(EARLY) / sizeof(EARLY[0]));

    MaterialGrant g[MATERIAL_GRANT_MAX];
    uint8_t n;
    bool ok;

    n = matyield::patrolYield(600, 600, 600, g);
    ok = false;
    for (int e = 0; e < EARLY_N; e++) if (grants(g, n, EARLY[e])) ok = true;
    ASSERT_TRUE(ok);

    n = matyield::roamYield(100000, 60000, 60000, 0, false, g);
    ok = false;
    for (int e = 0; e < EARLY_N; e++) if (grants(g, n, EARLY[e])) ok = true;
    ASSERT_TRUE(ok);

    // The one that was broken.
    n = matyield::gameYield(500, true, g);
    ok = false;
    for (int e = 0; e < EARLY_N; e++) if (grants(g, n, EARLY[e])) ok = true;
    ASSERT_TRUE(ok);
}

int main() {
    RUN_TEST(test_nothing_from_nothing);
    RUN_TEST(test_below_a_divisor_yields_nothing);
    RUN_TEST(test_abandoned_round_pays_nothing);
    RUN_TEST(test_every_material_is_reachable_without_an_imu);
    RUN_TEST(test_steps_only_accelerate_they_do_not_gate);
    RUN_TEST(test_a_roam_outpays_a_patrol_on_the_same_discoveries);
    RUN_TEST(test_never_overruns_the_grant_table);
    RUN_TEST(test_every_activity_pays_something_a_young_pet_can_spend);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
