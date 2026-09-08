// ── HexHound - Inventory and Crafting Unit Tests ─────────────────
//
// Covers the three things that decide whether this subsystem is safe to wire:
//
//   1. Stack saturation. items[] is uint16_t and a saturating add that wraps
//      before it clamps is not saturating at all.
//   2. Crafting with insufficient materials. A failed craft must spend NOTHING,
//      because a half-spent craft eats materials and hands back no cosmetic.
//   3. Recipe validation. A recipe naming an unknown item must be REJECTED at
//      load, so a bad content pack degrades to the baseline instead of
//      crashing or crafting nothing.
//
// It also holds the line the whole workstream exists to hold: crafting must not
// move a single stat. test_crafting_never_touches_a_stat snapshots all seven
// and asserts they are byte-identical afterwards.

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

#include <ArduinoJson.h>

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != nullptr)
#define ASSERT_NULL(p) ASSERT_TRUE((p) == nullptr)

// Both the pet and the inventory are singletons, so every test starts from a
// known state rather than from whatever the previous one left behind.
static void resetInventory(PetStage stage = STAGE_SENTINEL) {
    Inventory::instance().resetToBaseline();
    Inventory::instance().clearAll();
    PetCore::instance().state().stage = stage;
    PetCore::instance().state().dirty = false;
}

static uint8_t id(const char* s) {
    return Inventory::instance().idFor(s);
}

// ── The item table ────────────────────────────────────────────────────────

TEST(test_item_table_is_addressable_by_the_save) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    ASSERT_GT(inv.itemCount(), 0);
    // Every item id must be a legal index into PetState::items[], or a craft
    // would write past the persisted array.
    ASSERT_TRUE(inv.itemCount() <= ITEM_TYPE_COUNT);
}

TEST(test_item_lookup_by_string_id) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    ASSERT_EQ(inv.idFor("mat.scrap"), 0);
    ASSERT_NOT_NULL(inv.definition(inv.idFor("mat.scrap")));
    ASSERT_STREQ(inv.definition(inv.idFor("mat.scrap"))->id, "mat.scrap");
    ASSERT_EQ(inv.idFor("does.not.exist"), ITEM_ID_NONE);
    ASSERT_EQ(inv.idFor(nullptr), ITEM_ID_NONE);
    ASSERT_EQ(inv.idFor(""), ITEM_ID_NONE);
}

TEST(test_unknown_item_id_never_indexes_off_the_table) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    ASSERT_NULL(inv.definition(ITEM_ID_NONE));
    ASSERT_NULL(inv.definition((uint8_t)(inv.itemCount())));
    // Still drawable: a save from a newer firmware must not blank a row.
    ASSERT_STREQ(inv.nameFor(ITEM_ID_NONE), "?");
    ASSERT_EQ(inv.count(ITEM_ID_NONE), 0);
    ASSERT_EQ(inv.add(ITEM_ID_NONE, 5), 0);
    ASSERT_FALSE(inv.remove(ITEM_ID_NONE, 1));
}

TEST(test_every_item_is_material_or_cosmetic) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    bool sawMaterial = false, sawCosmetic = false;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        ASSERT_NOT_NULL(d);
        ASSERT_TRUE(d->kind < ITEM_KIND_COUNT);
        ASSERT_TRUE(d->slot < COSMETIC_SLOT_COUNT);
        if (d->kind == ITEM_MATERIAL) {
            sawMaterial = true;
            // A material has no cosmetic slot: it is spent, never displayed.
            ASSERT_EQ(d->slot, COSMETIC_NONE);
        } else {
            sawCosmetic = true;
            // A cosmetic that lands in no slot could never be seen, which
            // would make it a collectible with no purpose at all.
            ASSERT_TRUE(d->slot != COSMETIC_NONE);
        }
    }
    ASSERT_TRUE(sawMaterial);
    ASSERT_TRUE(sawCosmetic);
}

// ── Stack saturation ──────────────────────────────────────────────────────

TEST(test_add_saturates_at_the_stack_max) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t scrap = id("mat.scrap");

    ASSERT_EQ(inv.add(scrap, 10), 10);
    ASSERT_EQ(inv.count(scrap), 10);

    // Ask for far more than the cap in one call.
    const uint16_t added = inv.add(scrap, 5000);
    ASSERT_EQ(added, ITEM_STACK_MAX - 10);
    ASSERT_EQ(inv.count(scrap), ITEM_STACK_MAX);

    // Adding to a full stack reports that nothing landed, rather than lying.
    ASSERT_EQ(inv.add(scrap, 1), 0);
    ASSERT_EQ(inv.count(scrap), ITEM_STACK_MAX);
}

TEST(test_add_never_wraps_the_uint16) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t scrap = id("mat.scrap");

    // 0xFFFF would wrap a 16-bit accumulator to nearly zero if the add were
    // done at the stored width. It must clamp instead.
    ASSERT_EQ(inv.add(scrap, 0xFFFF), ITEM_STACK_MAX);
    ASSERT_EQ(inv.count(scrap), ITEM_STACK_MAX);

    inv.clearAll();
    inv.add(scrap, ITEM_STACK_MAX - 1);
    ASSERT_EQ(inv.add(scrap, 0xFFFF), 1);
    ASSERT_EQ(inv.count(scrap), ITEM_STACK_MAX);
}

TEST(test_cosmetics_do_not_stack) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t hat = id("cos.antenna");

    ASSERT_EQ(Inventory::stackMax(hat), 1);
    ASSERT_EQ(inv.add(hat, 7), 1);
    ASSERT_EQ(inv.count(hat), 1);
    ASSERT_EQ(inv.add(hat, 7), 0);
    ASSERT_EQ(inv.count(hat), 1);
}

TEST(test_add_of_zero_changes_nothing) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t scrap = id("mat.scrap");
    inv.add(scrap, 3);
    PetCore::instance().state().dirty = false;

    ASSERT_EQ(inv.add(scrap, 0), 0);
    ASSERT_EQ(inv.count(scrap), 3);
    // A no-op must not schedule a save; a save per no-op is flash wear.
    ASSERT_FALSE(PetCore::instance().state().dirty);
}

TEST(test_remove_is_all_or_nothing) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t scrap = id("mat.scrap");
    inv.add(scrap, 5);

    ASSERT_FALSE(inv.remove(scrap, 6));
    ASSERT_EQ(inv.count(scrap), 5);   // untouched, not partially spent

    ASSERT_TRUE(inv.remove(scrap, 5));
    ASSERT_EQ(inv.count(scrap), 0);
    ASSERT_FALSE(inv.remove(scrap, 1));
}

TEST(test_mutations_mark_the_save_dirty) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t scrap = id("mat.scrap");

    PetCore::instance().state().dirty = false;
    inv.add(scrap, 1);
    ASSERT_TRUE(PetCore::instance().state().dirty);

    PetCore::instance().state().dirty = false;
    inv.remove(scrap, 1);
    ASSERT_TRUE(PetCore::instance().state().dirty);
}

// ── Empty is a state ──────────────────────────────────────────────────────

TEST(test_a_new_pet_owns_nothing_and_says_so) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    ASSERT_TRUE(inv.isEmpty());
    ASSERT_EQ(inv.distinctMaterials(), 0);
    ASSERT_EQ(inv.ownedCosmetics(), 0);
    // Recipes still exist to work towards, which is what stops the screen from
    // being blank on the state every player sees first.
    ASSERT_GT(inv.recipeCount(), 0);
}

TEST(test_distinct_counts_track_kinds_not_quantities) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    inv.add(id("mat.scrap"), 40);
    ASSERT_EQ(inv.distinctMaterials(), 1);
    inv.add(id("mat.scrap"), 40);
    ASSERT_EQ(inv.distinctMaterials(), 1);
    inv.add(id("mat.copper"), 1);
    ASSERT_EQ(inv.distinctMaterials(), 2);
    ASSERT_FALSE(inv.isEmpty());

    inv.add(id("cos.antenna"), 1);
    ASSERT_EQ(inv.ownedCosmetics(), 1);
    ASSERT_EQ(inv.distinctMaterials(), 2);   // a cosmetic is not a material
}

// ── The baseline recipe table ─────────────────────────────────────────────

TEST(test_every_baseline_recipe_survives_validation) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    // The baseline goes through the same gate as a pack. If a shipped recipe
    // has a typo in an item id it is dropped, and this is where that shows up.
    ASSERT_EQ(inv.recipeCount(), BASELINE_RECIPE_COUNT);
    ASSERT_FALSE(inv.recipesFromPack());
}

TEST(test_every_recipe_makes_a_cosmetic_from_known_materials) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.recipeCount(); i++) {
        const Recipe* r = inv.recipe(i);
        ASSERT_NOT_NULL(r);
        ASSERT_TRUE(r->id[0] != '\0');
        ASSERT_NOT_NULL(inv.definition(r->output));
        ASSERT_EQ(inv.definition(r->output)->kind, ITEM_COSMETIC);
        ASSERT_GT(r->inputCount, 0);
        ASSERT_TRUE(r->inputCount <= RECIPE_MAX_INPUTS);
        for (uint8_t j = 0; j < r->inputCount; j++) {
            ASSERT_NOT_NULL(inv.definition(r->input[j]));
            ASSERT_GT(r->qty[j], 0);
            ASSERT_TRUE(r->input[j] != r->output);
        }
    }
}

TEST(test_recipe_lookup_by_id) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const int idx = inv.recipeIndexById("craft.antenna");
    ASSERT_GT(idx, -1);
    ASSERT_STREQ(inv.recipe((uint8_t)idx)->id, "craft.antenna");
    ASSERT_EQ(inv.recipeIndexById("craft.nope"), -1);
    ASSERT_EQ(inv.recipeIndexById(nullptr), -1);
}

TEST(test_out_of_range_recipe_accessors) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    ASSERT_NULL(inv.recipe(inv.recipeCount()));
    ASSERT_NULL(inv.recipe(200));
    ASSERT_EQ(inv.check(200), CRAFT_NO_SUCH_RECIPE);
    ASSERT_EQ(inv.craft(200), CRAFT_NO_SUCH_RECIPE);
    ASSERT_FALSE(inv.isUnlocked(200));
}

// ── Crafting ──────────────────────────────────────────────────────────────

// Gives the pet exactly the ingredients one recipe asks for, nothing more.
static void stockFor(uint8_t recipeIndex) {
    Inventory& inv = Inventory::instance();
    const Recipe* r = inv.recipe(recipeIndex);
    for (uint8_t i = 0; i < r->inputCount; i++) {
        inv.add(r->input[i], r->qty[i]);
    }
}

TEST(test_craft_spends_the_inputs_and_grants_the_output) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t idx = (uint8_t)inv.recipeIndexById("craft.antenna");
    stockFor(idx);

    const Recipe* r = inv.recipe(idx);
    ASSERT_EQ(inv.check(idx), CRAFT_OK);
    ASSERT_TRUE(inv.canCraft(idx));
    ASSERT_EQ(inv.craft(idx), CRAFT_OK);

    ASSERT_EQ(inv.count(r->output), 1);
    for (uint8_t i = 0; i < r->inputCount; i++) {
        ASSERT_EQ(inv.count(r->input[i]), 0);
    }
}

TEST(test_craft_with_insufficient_materials_spends_nothing) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t idx = (uint8_t)inv.recipeIndexById("craft.antenna");
    const Recipe* r = inv.recipe(idx);

    // Every ingredient present but ONE short by a single unit. This is the case
    // a naive implementation gets wrong: it spends the first ingredients, hits
    // the shortfall on the last, and the player is down materials with nothing
    // to show for it.
    stockFor(idx);
    ASSERT_TRUE(inv.remove(r->input[r->inputCount - 1], 1));

    uint16_t before[RECIPE_MAX_INPUTS];
    for (uint8_t i = 0; i < r->inputCount; i++) before[i] = inv.count(r->input[i]);

    ASSERT_FALSE(inv.canCraft(idx));
    ASSERT_EQ(inv.check(idx), CRAFT_SHORT);
    ASSERT_EQ(inv.craft(idx), CRAFT_SHORT);

    for (uint8_t i = 0; i < r->inputCount; i++) {
        ASSERT_EQ(inv.count(r->input[i]), before[i]);
    }
    ASSERT_EQ(inv.count(r->output), 0);
}

TEST(test_craft_with_nothing_at_all) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    for (uint8_t i = 0; i < inv.recipeCount(); i++) {
        ASSERT_FALSE(inv.canCraft(i));
        ASSERT_EQ(inv.craft(i), CRAFT_SHORT);
    }
    ASSERT_TRUE(inv.isEmpty());
}

TEST(test_craft_is_blocked_by_the_stage_gate) {
    // An egg cannot craft. The gate is checked before the materials, so the
    // screen can say "locked" rather than "you are short two Copper Wire" for
    // something the pet could not make with a full pack anyway.
    resetInventory(STAGE_EGG);
    Inventory& inv = Inventory::instance();
    const uint8_t idx = (uint8_t)inv.recipeIndexById("craft.antenna");
    stockFor(idx);

    ASSERT_FALSE(inv.isUnlocked(idx));
    ASSERT_EQ(inv.check(idx), CRAFT_LOCKED);
    ASSERT_EQ(inv.craft(idx), CRAFT_LOCKED);
    ASSERT_EQ(inv.count(inv.recipe(idx)->output), 0);

    PetCore::instance().state().stage = STAGE_PACKET_PUP;
    ASSERT_TRUE(inv.isUnlocked(idx));
    ASSERT_EQ(inv.check(idx), CRAFT_OK);
}

TEST(test_a_cosmetic_cannot_be_crafted_twice) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const uint8_t idx = (uint8_t)inv.recipeIndexById("craft.antenna");

    stockFor(idx);
    ASSERT_EQ(inv.craft(idx), CRAFT_OK);

    // Materials for a second one, which must NOT be spent: the pet already
    // owns the hat and a second is not a thing.
    stockFor(idx);
    const Recipe* r = inv.recipe(idx);
    ASSERT_EQ(inv.check(idx), CRAFT_ALREADY_OWNED);
    ASSERT_EQ(inv.craft(idx), CRAFT_ALREADY_OWNED);
    ASSERT_EQ(inv.count(r->input[0]), r->qty[0]);
    ASSERT_EQ(inv.count(r->output), 1);
}

TEST(test_crafting_never_touches_a_stat) {
    // The line this whole workstream exists to hold. If a future change makes
    // an item grant XP or lift mood, this is what fails.
    resetInventory();
    Inventory& inv = Inventory::instance();
    PetState& st = PetCore::instance().state();
    st.stats.hunger = 41; st.stats.mood = 42; st.stats.energy = 43;
    st.stats.xp = 44; st.stats.trust = 45; st.stats.mischief = 46;
    st.stats.health = 47;
    const PetStats before = st.stats;
    const uint32_t masteryBefore = st.masteryXP;

    const uint8_t idx = (uint8_t)inv.recipeIndexById("craft.antenna");
    stockFor(idx);
    ASSERT_EQ(inv.craft(idx), CRAFT_OK);

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

// ── Recipe pack validation ────────────────────────────────────────────────

TEST(test_pack_replaces_the_baseline) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const char* json =
        "{\"recipes\":[{\"id\":\"pack.hat\",\"output\":\"cos.antenna\","
        "\"stage\":2,\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":1}]}]}";

    ASSERT_TRUE(inv.applyRecipeJson(json));
    ASSERT_EQ(inv.recipeCount(), 1);
    ASSERT_STREQ(inv.recipe(0)->id, "pack.hat");
    ASSERT_EQ(inv.recipe(0)->output, id("cos.antenna"));
    ASSERT_EQ(inv.recipe(0)->inputCount, 1);
    ASSERT_EQ(inv.recipe(0)->input[0], id("mat.scrap"));
    ASSERT_EQ(inv.recipe(0)->qty[0], 1);
    ASSERT_EQ(inv.recipe(0)->minStage, STAGE_PACKET_PUP);

    inv.resetToBaseline();
    ASSERT_EQ(inv.recipeCount(), BASELINE_RECIPE_COUNT);
}

TEST(test_recipe_with_an_unknown_input_is_rejected) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    // One good recipe and one naming an item this firmware has never heard of.
    // The good one must survive and the bad one must be gone; a pack is not
    // all-or-nothing, but an unresolvable row never reaches the table.
    const char* json =
        "{\"recipes\":["
        "{\"id\":\"ok\",\"output\":\"cos.scarf\","
        "\"inputs\":[{\"item\":\"mat.signal\",\"qty\":2}]},"
        "{\"id\":\"bad\",\"output\":\"cos.antenna\","
        "\"inputs\":[{\"item\":\"mat.unobtanium\",\"qty\":1}]}"
        "]}";

    ASSERT_TRUE(inv.applyRecipeJson(json));
    ASSERT_EQ(inv.recipeCount(), 1);
    ASSERT_STREQ(inv.recipe(0)->id, "ok");
    ASSERT_EQ(inv.recipeIndexById("bad"), -1);
}

TEST(test_recipe_with_an_unknown_output_is_rejected) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    const char* json =
        "[{\"id\":\"bad\",\"output\":\"cos.jetpack\","
        "\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":1}]}]";

    // Zero valid rows: the baseline is left completely alone rather than being
    // cleared and then found to have nothing to replace it with.
    ASSERT_FALSE(inv.applyRecipeJson(json));
    ASSERT_EQ(inv.recipeCount(), BASELINE_RECIPE_COUNT);
    ASSERT_EQ(inv.recipeIndexById("bad"), -1);
}

TEST(test_recipe_that_outputs_a_material_is_rejected) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    // A recipe minting materials is a laundry, and the subsystem exists to
    // produce cosmetics. Rejected at load, not at craft time.
    const char* json =
        "[{\"id\":\"launder\",\"output\":\"mat.scrap\","
        "\"inputs\":[{\"item\":\"mat.glitch\",\"qty\":1}]}]";
    ASSERT_FALSE(inv.applyRecipeJson(json));
    ASSERT_EQ(inv.recipeCount(), BASELINE_RECIPE_COUNT);
}

TEST(test_malformed_recipes_are_rejected_one_by_one) {
    resetInventory();
    Inventory& inv = Inventory::instance();

    struct { const char* why; const char* json; } cases[] = {
        { "no id",
          "[{\"output\":\"cos.scarf\",\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":1}]}]" },
        { "no inputs",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":[]}]" },
        { "inputs missing entirely",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\"}]" },
        { "zero quantity",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":0}]}]" },
        { "quantity above the stack cap",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":5000}]}]" },
        { "more inputs than the engine holds",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":["
          "{\"item\":\"mat.scrap\",\"qty\":1},{\"item\":\"mat.copper\",\"qty\":1},"
          "{\"item\":\"mat.signal\",\"qty\":1},{\"item\":\"mat.shard\",\"qty\":1}]}]" },
        { "duplicate input",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":["
          "{\"item\":\"mat.scrap\",\"qty\":1},{\"item\":\"mat.scrap\",\"qty\":2}]}]" },
        { "input is the output",
          "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"inputs\":["
          "{\"item\":\"cos.scarf\",\"qty\":1}]}]" },
        { "not JSON at all", "{not json" },
        { "empty array", "[]" },
        { "an object with no recipes", "{\"quests\":[]}" }
    };

    for (auto& c : cases) {
        inv.resetToBaseline();
        if (inv.applyRecipeJson(c.json)) {
            printf("FAIL\n    %s:%d: accepted a pack with %s\n",
                   __FILE__, __LINE__, c.why);
            s_testsFailed++;
            return;
        }
        // And in every one of those cases the baseline is still intact.
        ASSERT_EQ(inv.recipeCount(), BASELINE_RECIPE_COUNT);
    }
}

TEST(test_pack_truncates_at_the_cap) {
    resetInventory();
    Inventory& inv = Inventory::instance();

    // More rows than RECIPE_MAX_DEFS. The table must fill and stop, never grow.
    std::string json = "[";
    for (int i = 0; i < RECIPE_MAX_DEFS + 6; i++) {
        char row[160];
        snprintf(row, sizeof(row),
                 "%s{\"id\":\"r%d\",\"output\":\"cos.scarf\","
                 "\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":1}]}",
                 i ? "," : "", i);
        json += row;
    }
    json += "]";

    ASSERT_TRUE(inv.applyRecipeJson(json.c_str()));
    ASSERT_EQ(inv.recipeCount(), RECIPE_MAX_DEFS);
}

TEST(test_pack_stage_field_is_bounded) {
    resetInventory();
    Inventory& inv = Inventory::instance();
    // A stage outside the enum means "no gate", never an unreachable recipe.
    const char* json =
        "[{\"id\":\"a\",\"output\":\"cos.scarf\",\"stage\":99,"
        "\"inputs\":[{\"item\":\"mat.scrap\",\"qty\":1}]}]";
    ASSERT_TRUE(inv.applyRecipeJson(json));
    ASSERT_EQ(inv.recipe(0)->minStage, 0);

    PetCore::instance().state().stage = STAGE_EGG;
    ASSERT_TRUE(inv.isUnlocked(0));
}

TEST(test_a_pack_can_be_crafted_from) {
    // End to end through the pack path: load, stock, craft, and confirm the
    // cosmetic landed and the materials went.
    resetInventory();
    Inventory& inv = Inventory::instance();
    const char* json =
        "[{\"id\":\"p\",\"output\":\"cos.lamp\",\"stage\":2,\"inputs\":["
        "{\"item\":\"mat.scrap\",\"qty\":2},{\"item\":\"mat.lens\",\"qty\":1}]}]";
    ASSERT_TRUE(inv.applyRecipeJson(json));

    ASSERT_EQ(inv.check(0), CRAFT_SHORT);
    inv.add(id("mat.scrap"), 2);
    inv.add(id("mat.lens"), 1);
    ASSERT_EQ(inv.craft(0), CRAFT_OK);
    ASSERT_EQ(inv.count(id("cos.lamp")), 1);
    ASSERT_EQ(inv.count(id("mat.scrap")), 0);
    ASSERT_EQ(inv.count(id("mat.lens")), 0);
}

int main() {
    printf("\n=== Inventory & Crafting Tests ===\n");

    RUN_TEST(test_item_table_is_addressable_by_the_save);
    RUN_TEST(test_item_lookup_by_string_id);
    RUN_TEST(test_unknown_item_id_never_indexes_off_the_table);
    RUN_TEST(test_every_item_is_material_or_cosmetic);

    RUN_TEST(test_add_saturates_at_the_stack_max);
    RUN_TEST(test_add_never_wraps_the_uint16);
    RUN_TEST(test_cosmetics_do_not_stack);
    RUN_TEST(test_add_of_zero_changes_nothing);
    RUN_TEST(test_remove_is_all_or_nothing);
    RUN_TEST(test_mutations_mark_the_save_dirty);

    RUN_TEST(test_a_new_pet_owns_nothing_and_says_so);
    RUN_TEST(test_distinct_counts_track_kinds_not_quantities);

    RUN_TEST(test_every_baseline_recipe_survives_validation);
    RUN_TEST(test_every_recipe_makes_a_cosmetic_from_known_materials);
    RUN_TEST(test_recipe_lookup_by_id);
    RUN_TEST(test_out_of_range_recipe_accessors);

    RUN_TEST(test_craft_spends_the_inputs_and_grants_the_output);
    RUN_TEST(test_craft_with_insufficient_materials_spends_nothing);
    RUN_TEST(test_craft_with_nothing_at_all);
    RUN_TEST(test_craft_is_blocked_by_the_stage_gate);
    RUN_TEST(test_a_cosmetic_cannot_be_crafted_twice);
    RUN_TEST(test_crafting_never_touches_a_stat);

    RUN_TEST(test_pack_replaces_the_baseline);
    RUN_TEST(test_recipe_with_an_unknown_input_is_rejected);
    RUN_TEST(test_recipe_with_an_unknown_output_is_rejected);
    RUN_TEST(test_recipe_that_outputs_a_material_is_rejected);
    RUN_TEST(test_malformed_recipes_are_rejected_one_by_one);
    RUN_TEST(test_pack_truncates_at_the_cap);
    RUN_TEST(test_pack_stage_field_is_bounded);
    RUN_TEST(test_a_pack_can_be_crafted_from);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
