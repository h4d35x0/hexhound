// ── HexHound - Content Engine Unit Tests ─────────────────────────
//
// Covers the three W2 engines: the store and its baseline fallback, the
// dialogue picker, and the daily quest roll.

#include "../test_stubs.h"

#include "../../src/content/content_crypto.cpp"
#include "../../src/content/content_cbor.cpp"
#include "../../src/content/content_pack.cpp"
#include "../../src/content/content_store.h"
#include "../../src/content/content_store.cpp"
#include "../../src/content/dialogue_engine.h"
#include "../../src/content/dialogue_engine.cpp"
#include "../../src/content/quest_engine.h"
#include "../../src/content/quest_engine.cpp"

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

// Stands in for pet memory. The engine must take its numbers from here and
// never from the content itself.
static uint32_t s_memoryValues[MEM_SLOT_COUNT] = {};
static int s_memoryCalls = 0;

static uint32_t testMemory(uint8_t slot, void* user) {
    (void)user;
    s_memoryCalls++;
    return slot < MEM_SLOT_COUNT ? s_memoryValues[slot] : 0;
}

// Every test starts from the shipped content, since the store is a singleton.
static void resetContent() {
    ContentStore::instance().resetToBaseline();
    DialogueEngine::instance().begin(12345);
    DialogueEngine::instance().setMemoryAccessor(nullptr, nullptr);
    s_memoryCalls = 0;
    for (int i = 0; i < MEM_SLOT_COUNT; i++) s_memoryValues[i] = 0;
}

// ── ContentStore ──────────────────────────────────────────────────────────

TEST(test_baseline_is_always_available) {
    // The whole point of the compiled-in pack: no filesystem, still content.
    resetContent();
    ContentStore& store = ContentStore::instance();
    ASSERT_GT(store.dialogueCount(), 0);
    ASSERT_GT(store.questDefCount(), 0);
    ASSERT_FALSE(store.dialogueFromPack());
    ASSERT_FALSE(store.questsFromPack());
}

TEST(test_baseline_covers_every_quest_kind) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    bool seen[QUEST_KIND_COUNT] = {};
    for (uint8_t i = 0; i < store.questDefCount(); i++) {
        seen[store.questDef(i)->kind] = true;
    }
    for (int k = 0; k < QUEST_KIND_COUNT; k++) ASSERT_TRUE(seen[k]);
}

TEST(test_quest_lookup_by_id) {
    resetContent();
    ContentStore& store = ContentStore::instance();
    const QuestDef* def = store.questDefById("care.feed");
    ASSERT_NOT_NULL(def);
    ASSERT_EQ(def->kind, QUEST_CARE);
    ASSERT_NULL(store.questDefById("does.not.exist"));
    ASSERT_NULL(store.questDefById(nullptr));
}

TEST(test_out_of_range_accessors_return_null) {
    resetContent();
    ContentStore& store = ContentStore::instance();
    ASSERT_NULL(store.dialogue(store.dialogueCount()));
    ASSERT_NULL(store.questDef(store.questDefCount()));
    ASSERT_NULL(store.dialogue(255));
}

TEST(test_pack_replaces_baseline) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    const char* pack =
        "{\"lines\":[{\"text\":\"Only line.\",\"context\":\"greeting\"}]}";
    ASSERT_TRUE(store.applyDialogueJson(pack));
    ASSERT_EQ(store.dialogueCount(), 1);
    ASSERT_STREQ(store.dialogue(0)->text, "Only line.");
    ASSERT_EQ(store.dialogue(0)->context, DLG_GREETING);

    resetContent();
    ASSERT_GT(store.dialogueCount(), 1);
}

TEST(test_bad_pack_keeps_baseline) {
    resetContent();
    ContentStore& store = ContentStore::instance();
    uint8_t before = store.dialogueCount();

    // Unparseable, then parseable but empty of usable rows. Neither may blank
    // the pet out.
    ASSERT_FALSE(store.applyDialogueJson("{ this is not json"));
    ASSERT_EQ(store.dialogueCount(), before);

    ASSERT_FALSE(store.applyDialogueJson("{\"lines\":[{\"context\":\"idle\"}]}"));
    ASSERT_EQ(store.dialogueCount(), before);

    uint8_t quests = store.questDefCount();
    ASSERT_FALSE(store.applyQuestJson("[]"));
    ASSERT_EQ(store.questDefCount(), quests);
}

TEST(test_pack_parses_named_and_numeric_fields) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    const char* pack =
        "{\"quests\":["
        "{\"id\":\"a\",\"text\":\"Named.\",\"kind\":\"cyber\",\"target\":3,"
        "\"xp\":40,\"bond\":5,\"requires\":[\"imu\",\"touch\"]},"
        "{\"id\":\"b\",\"text\":\"Numeric.\",\"kind\":1,\"target\":0}"
        "]}";
    ASSERT_TRUE(store.applyQuestJson(pack));
    ASSERT_EQ(store.questDefCount(), 2);

    const QuestDef* a = store.questDefById("a");
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->kind, QUEST_CYBER);
    ASSERT_EQ(a->target, 3);
    ASSERT_EQ(a->rewardXP, 40);
    ASSERT_EQ(a->rewardBond, 5);
    ASSERT_EQ(a->requiresCaps, (uint16_t)(CAP_IMU | CAP_TOUCH));

    const QuestDef* b = store.questDefById("b");
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(b->kind, QUEST_EXPLORE);
    // A target of zero would complete the moment it was offered.
    ASSERT_EQ(b->target, 1);

    resetContent();
}

TEST(test_pack_truncates_at_the_cap) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    String pack = "{\"quests\":[";
    for (int i = 0; i < CONTENT_MAX_QUEST_DEFS + 8; i++) {
        char row[96];
        snprintf(row, sizeof(row),
                 "%s{\"id\":\"q%d\",\"text\":\"Filler.\",\"kind\":\"care\"}",
                 i ? "," : "", i);
        pack += row;
    }
    pack += "]}";

    ASSERT_TRUE(store.applyQuestJson(pack.c_str()));
    ASSERT_EQ(store.questDefCount(), CONTENT_MAX_QUEST_DEFS);

    resetContent();
}

// ── DialogueEngine ────────────────────────────────────────────────────────

TEST(test_dialogue_picks_a_line) {
    resetContent();
    const char* line = DialogueEngine::instance().pick(
        DLG_GREETING, TRAIT_CURIOUS, TRAIT_BRAVE, STAGE_PACKET_PUP);
    ASSERT_NOT_NULL(line);
    ASSERT_GT((int)strlen(line), 0);
}

TEST(test_dialogue_returns_null_when_nothing_matches) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    // A pack that only speaks in one context. Every other context must come
    // back empty-handed instead of crashing or inventing filler.
    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":[{\"text\":\"Hi.\",\"context\":\"greeting\"}]}"));

    ASSERT_NULL(DialogueEngine::instance().pick(DLG_GAME_LOSE, 0xFF, 0xFF, 0xFF));
    ASSERT_FALSE(DialogueEngine::instance().hasLineFor(DLG_GAME_LOSE, 0xFF, 0xFF, 0xFF));
    ASSERT_NOT_NULL(DialogueEngine::instance().pick(DLG_GREETING, 0xFF, 0xFF, 0xFF));

    resetContent();
}

TEST(test_dialogue_respects_trait_and_stage_filters) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"Sleepy only.\",\"context\":\"idle\",\"trait\":\"sleepy\"},"
        "{\"text\":\"Egg only.\",\"context\":\"idle\",\"stage\":1}"
        "]}"));

    // Neither filter satisfied: no line at all.
    ASSERT_NULL(DialogueEngine::instance().pick(DLG_IDLE, TRAIT_BRAVE, 0xFF,
                                                STAGE_GREMLIN));
    // The trait line, matched on the pet's SECOND trait.
    const char* line = DialogueEngine::instance().pick(DLG_IDLE, TRAIT_BRAVE,
                                                       TRAIT_SLEEPY, STAGE_GREMLIN);
    ASSERT_NOT_NULL(line);
    ASSERT_STREQ(line, "Sleepy only.");
    // The stage line.
    line = DialogueEngine::instance().pick(DLG_IDLE, TRAIT_BRAVE, 0xFF, STAGE_EGG);
    ASSERT_NOT_NULL(line);
    ASSERT_STREQ(line, "Egg only.");

    resetContent();
}

TEST(test_dialogue_prefers_the_specific_line) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"Generic.\",\"context\":\"idle\"},"
        "{\"text\":\"Curious.\",\"context\":\"idle\",\"trait\":\"curious\"}"
        "]}"));

    // Repeated: a scoring bug that only sometimes prefers the generic line
    // would otherwise pass on a lucky draw.
    for (int i = 0; i < 20; i++) {
        const char* line = DialogueEngine::instance().pick(DLG_IDLE, TRAIT_CURIOUS,
                                                           0xFF, STAGE_GREMLIN);
        ASSERT_NOT_NULL(line);
        ASSERT_STREQ(line, "Curious.");
    }

    resetContent();
}

// ── The form filter (P1-W1) ───────────────────────────────────────────────

TEST(test_dialogue_form_filter_needs_the_matching_form) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"Walker.\",\"context\":\"idle\",\"form\":\"pathfinder\"},"
        "{\"text\":\"Watcher.\",\"context\":\"idle\",\"form\":\"guardian\"}"
        "]}"));

    DialogueEngine& dlg = DialogueEngine::instance();
    ASSERT_STREQ(dlg.pick(DLG_IDLE, 0xFF, 0xFF, 0xFF, FORM_PATHFINDER),
                 "Walker.");
    ASSERT_STREQ(dlg.pick(DLG_IDLE, 0xFF, 0xFF, 0xFF, FORM_GUARDIAN),
                 "Watcher.");

    resetContent();
}

TEST(test_undecided_pet_never_sees_a_form_line) {
    // The discoverability rule in one assertion. A pet with no form must not be
    // handed writing that belongs to a form it has not earned, and a caller
    // that has not been wired for forms at all must behave the same way.
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":[{\"text\":\"Walker.\",\"context\":\"idle\","
        "\"form\":\"pathfinder\"}]}"));

    DialogueEngine& dlg = DialogueEngine::instance();
    ASSERT_NULL(dlg.pick(DLG_IDLE, 0xFF, 0xFF, 0xFF, DIALOGUE_FORM_UNSET));
    ASSERT_NULL(dlg.pick(DLG_IDLE, 0xFF, 0xFF, 0xFF));   // default argument
    ASSERT_NULL(dlg.pick(DLG_IDLE, 0xFF, 0xFF));         // pre-form overload
    ASSERT_FALSE(dlg.hasLineFor(DLG_IDLE, 0xFF, 0xFF, 0xFF));

    resetContent();
}

TEST(test_form_line_outranks_generic_but_not_trait) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"Generic.\",\"context\":\"idle\"},"
        "{\"text\":\"Walker.\",\"context\":\"idle\",\"form\":\"pathfinder\"},"
        "{\"text\":\"Curious.\",\"context\":\"idle\",\"trait\":\"curious\"}"
        "]}"));

    DialogueEngine& dlg = DialogueEngine::instance();
    for (int i = 0; i < 20; i++) {
        // Form beats untagged.
        ASSERT_STREQ(dlg.pick(DLG_IDLE, TRAIT_BRAVE, 0xFF, 0xFF,
                              FORM_PATHFINDER), "Walker.");
        // Trait still beats form: the trait is the pet's voice.
        ASSERT_STREQ(dlg.pick(DLG_IDLE, TRAIT_CURIOUS, 0xFF, 0xFF,
                              FORM_PATHFINDER), "Curious.");
    }

    resetContent();
}

TEST(test_form_parses_as_name_or_ordinal_and_degrades_safely) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"ByName.\",\"context\":\"idle\",\"form\":\"cipher\"},"
        "{\"text\":\"ByOrdinal.\",\"context\":\"greeting\",\"form\":4},"
        "{\"text\":\"Nonsense.\",\"context\":\"game_win\",\"form\":\"wizard\"},"
        "{\"text\":\"Explicit.\",\"context\":\"game_lose\",\"form\":\"unset\"}"
        "]}"));

    ASSERT_EQ(store.dialogue(0)->form, FORM_CIPHER);
    ASSERT_EQ(store.dialogue(1)->form, FORM_CIPHER);   // ordinal 4 == cipher
    // A misspelled form and an explicit "unset" both mean "any pet", never
    // "only a pet with no identity", which would silently hide the line.
    ASSERT_EQ(store.dialogue(2)->form, 0xFF);
    ASSERT_EQ(store.dialogue(3)->form, 0xFF);

    resetContent();
}

TEST(test_baseline_form_lines_reach_a_formed_pet) {
    // The shipped pack must actually contain form-flavoured writing, and an
    // undecided pet must still have plenty to say without it.
    resetContent();
    DialogueEngine& dlg = DialogueEngine::instance();

    int tagged = 0;
    ContentStore& store = ContentStore::instance();
    for (uint8_t i = 0; i < store.dialogueCount(); i++) {
        if (store.dialogue(i)->form != 0xFF) tagged++;
    }
    ASSERT_GT(tagged, 0);

    // Every form-tagged baseline line is reachable by the pet it was written
    // for, so no shipped line is dead content.
    for (uint8_t i = 0; i < store.dialogueCount(); i++) {
        const DialogueLine* line = store.dialogue(i);
        if (line->form == 0xFF) continue;
        ASSERT_TRUE(dlg.hasLineFor(line->context, 0xFF, 0xFF, 0xFF,
                                   line->form));
    }

    // And the pet is never mute just because it has not become anything.
    ASSERT_NOT_NULL(dlg.pick(DLG_IDLE, 0xFF, 0xFF, 0xFF, DIALOGUE_FORM_UNSET));
    ASSERT_NOT_NULL(dlg.pick(DLG_GREETING, 0xFF, 0xFF, 0xFF,
                             DIALOGUE_FORM_UNSET));
}

TEST(test_dialogue_memory_line_needs_an_accessor) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":[{\"text\":\"We ran {n} patrols.\",\"context\":\"idle\","
        "\"memory\":\"patrols\"}]}"));

    // No accessor installed: the line is not eligible, because content must
    // never print a statistic the firmware did not supply.
    ASSERT_NULL(DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF));
    ASSERT_EQ(s_memoryCalls, 0);

    s_memoryValues[MEM_SLOT_PATROLS] = 14;
    DialogueEngine::instance().setMemoryAccessor(testMemory, nullptr);

    const char* line = DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF);
    ASSERT_NOT_NULL(line);
    ASSERT_STREQ(line, "We ran 14 patrols.");
    ASSERT_EQ(s_memoryCalls, 1);

    resetContent();
}

TEST(test_dialogue_substitution_stays_in_bounds) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    // A line longer than the text field, with the token at the front so it
    // survives the clip and the expansion runs against a full buffer.
    String text = "{n}";
    for (int i = 0; i < CONTENT_MAX_TEXT_LEN + 40; i++) text += "x";

    String pack = "{\"lines\":[{\"text\":\"";
    pack += text;
    pack += "\",\"context\":\"idle\",\"memory\":\"visits\"}]}";

    ASSERT_TRUE(store.applyDialogueJson(pack.c_str()));
    s_memoryValues[MEM_SLOT_VISITS] = 4294967295u;
    DialogueEngine::instance().setMemoryAccessor(testMemory, nullptr);

    const char* line = DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF);
    ASSERT_NOT_NULL(line);
    // Text is clipped to the field on load, the ten-digit number goes in, and
    // the whole thing still fits the expansion buffer.
    ASSERT_EQ(strncmp(line, "4294967295", 10), 0);
    ASSERT_TRUE((int)strlen(line) < CONTENT_MAX_TEXT_LEN + 12);

    resetContent();
}

TEST(test_dialogue_avoids_immediate_repeats) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    ASSERT_TRUE(store.applyDialogueJson(
        "{\"lines\":["
        "{\"text\":\"One.\",\"context\":\"idle\"},"
        "{\"text\":\"Two.\",\"context\":\"idle\"}"
        "]}"));

    char previous[CONTENT_MAX_TEXT_LEN] = {};
    strlcpy(previous, DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF),
            sizeof(previous));
    for (int i = 0; i < 10; i++) {
        const char* line = DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF);
        ASSERT_NOT_NULL(line);
        ASSERT_TRUE(strcmp(line, previous) != 0);
        strlcpy(previous, line, sizeof(previous));
    }

    resetContent();
}

TEST(test_empty_pack_leaves_the_pet_speaking) {
    resetContent();
    ContentStore& store = ContentStore::instance();

    // An empty array parses fine and is still refused, because the alternative
    // is a device that boots with nothing to say.
    ASSERT_FALSE(store.applyDialogueJson("{\"lines\":[]}"));
    ASSERT_GT(store.dialogueCount(), 0);
    ASSERT_NOT_NULL(DialogueEngine::instance().pick(DLG_IDLE, 0xFF, 0xFF, 0xFF));
}

// ── QuestEngine ───────────────────────────────────────────────────────────

TEST(test_quest_roll_fills_every_slot) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    ASSERT_EQ(quests.activeCount(), QUEST_MAX_ACTIVE);
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        const QuestInstance* q = quests.quest(i);
        ASSERT_NOT_NULL(q);
        ASSERT_GT((int)strlen(q->id), 0);
        ASSERT_GT(q->target, 0);
        ASSERT_EQ(q->progress, 0);
        ASSERT_EQ(q->status, QUEST_OFFERED);
        ASSERT_NOT_NULL(quests.definitionFor(i));
    }
    ASSERT_NULL(quests.quest(QUEST_MAX_ACTIVE));
}

TEST(test_quest_roll_has_no_duplicates) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();

    for (uint32_t seed = 1; seed <= 50; seed++) {
        quests.rollDaily(seed * 7919u);
        for (uint8_t i = 0; i < quests.activeCount(); i++) {
            for (uint8_t j = (uint8_t)(i + 1); j < quests.activeCount(); j++) {
                ASSERT_TRUE(strcmp(quests.quest(i)->id, quests.quest(j)->id) != 0);
            }
        }
    }
}

TEST(test_quest_roll_never_offers_an_impossible_quest) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();

    // The native test build carries the default board profile, which has no
    // IMU. The two motion quests in the baseline must never appear.
    for (uint32_t seed = 1; seed <= 100; seed++) {
        quests.rollDaily(seed);
        for (uint8_t i = 0; i < quests.activeCount(); i++) {
            const QuestDef* def = quests.definitionFor(i);
            ASSERT_NOT_NULL(def);
            ASSERT_EQ((uint16_t)(def->requiresCaps & ~Caps::mask()), 0);
        }
    }

    // And the gate itself, stated directly.
    QuestDef impossible;
    impossible.requiresCaps = (uint16_t)(CAP_IMU | CAP_TOUCH | CAP_PSRAM);
    ASSERT_FALSE(QuestEngine::isOfferable(impossible));

    QuestDef anyBoard;
    ASSERT_TRUE(QuestEngine::isOfferable(anyBoard));
}

TEST(test_quest_roll_spreads_across_kinds) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0x5EED);

    bool seen[QUEST_KIND_COUNT] = {};
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        seen[quests.quest(i)->kind] = true;
    }
    // Four slots, four kinds, at least one eligible quest in each on this
    // board: the day should never be four of the same thing.
    for (int k = 0; k < QUEST_KIND_COUNT; k++) ASSERT_TRUE(seen[k]);
}

TEST(test_quest_progress_and_completion) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    // Find a care quest and drive it to done.
    int slot = -1;
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        if (quests.quest(i)->kind == QUEST_CARE) { slot = i; break; }
    }
    ASSERT_GT(slot, -1);

    uint16_t target = quests.quest(slot)->target;
    for (uint16_t n = 1; n < target; n++) {
        ASSERT_FALSE(quests.reportProgress(QUEST_CARE, 1));
        ASSERT_EQ(quests.quest(slot)->status, QUEST_ACTIVE);
    }
    ASSERT_TRUE(quests.reportProgress(QUEST_CARE, 1));
    ASSERT_EQ(quests.quest(slot)->status, QUEST_COMPLETE);
    ASSERT_EQ(quests.quest(slot)->progress, target);

    // A finished quest does not keep counting, and does not re-fire.
    ASSERT_FALSE(quests.reportProgress(QUEST_CARE, 50));
    ASSERT_EQ(quests.quest(slot)->progress, target);
    ASSERT_GT(quests.completedCount(), 0);
}

TEST(test_quest_progress_saturates_at_target) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    int slot = -1;
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        if (quests.quest(i)->kind == QUEST_EXPLORE) { slot = i; break; }
    }
    ASSERT_GT(slot, -1);

    // A burst far larger than the target must clamp, not wrap back to nearly
    // zero and leave the quest looking untouched.
    ASSERT_TRUE(quests.reportProgress(QUEST_EXPLORE, 65535));
    ASSERT_EQ(quests.quest(slot)->progress, quests.quest(slot)->target);
    ASSERT_EQ(quests.quest(slot)->status, QUEST_COMPLETE);
}

TEST(test_quest_progress_by_id) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    const char* id = quests.quest(0)->id;
    uint16_t target = quests.quest(0)->target;
    ASSERT_TRUE(quests.reportProgressById(id, target));
    ASSERT_EQ(quests.quest(0)->status, QUEST_COMPLETE);

    ASSERT_FALSE(quests.reportProgressById("no.such.quest", 1));
    ASSERT_FALSE(quests.reportProgressById(nullptr, 1));
    ASSERT_FALSE(quests.reportProgress(QUEST_CARE, 0));
}

TEST(test_quest_reroll_is_bounded) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    char before[CONTENT_MAX_ID_LEN];
    strlcpy(before, quests.quest(1)->id, sizeof(before));

    ASSERT_EQ(quests.rerollsLeft(), QUEST_REROLLS_PER_DAY);
    ASSERT_TRUE(quests.reroll(1, 42));
    ASSERT_TRUE(strcmp(quests.quest(1)->id, before) != 0);
    ASSERT_EQ(quests.quest(1)->progress, 0);
    ASSERT_EQ(quests.quest(1)->status, QUEST_OFFERED);
    ASSERT_EQ(quests.rerollsLeft(), 0);

    // Budget spent, and out-of-range slots are refused rather than trusted.
    ASSERT_FALSE(quests.reroll(0, 43));
    ASSERT_FALSE(quests.reroll(QUEST_MAX_ACTIVE, 44));

    // A fresh day restores the budget.
    quests.rollDaily(0xBEEF);
    ASSERT_EQ(quests.rerollsLeft(), QUEST_REROLLS_PER_DAY);
}

TEST(test_quest_reroll_refuses_a_finished_quest) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    ASSERT_TRUE(quests.reportProgressById(quests.quest(0)->id,
                                          quests.quest(0)->target));
    ASSERT_FALSE(quests.reroll(0, 7));
    ASSERT_EQ(quests.rerollsLeft(), QUEST_REROLLS_PER_DAY);
}

TEST(test_quest_roll_is_reproducible) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();

    char first[QUEST_MAX_ACTIVE][CONTENT_MAX_ID_LEN] = {};
    quests.rollDaily(0xABCDEF);
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        strlcpy(first[i], quests.quest(i)->id, CONTENT_MAX_ID_LEN);
    }

    quests.rollDaily(0x111111);
    quests.rollDaily(0xABCDEF);
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        ASSERT_STREQ(quests.quest(i)->id, first[i]);
    }
}

TEST(test_quest_roll_with_no_eligible_content) {
    resetContent();
    ContentStore& store = ContentStore::instance();
    QuestEngine& quests = QuestEngine::instance();

    // Every quest gated behind a capability this board does not have.
    ASSERT_TRUE(store.applyQuestJson(
        "{\"quests\":[{\"id\":\"x\",\"text\":\"Impossible.\",\"kind\":\"care\","
        "\"requires\":[\"imu\"]}]}"));

    quests.rollDaily(9);
    ASSERT_EQ(quests.activeCount(), 0);
    ASSERT_NULL(quests.quest(0));
    ASSERT_FALSE(quests.allComplete());
    ASSERT_FALSE(quests.reportProgress(QUEST_CARE, 1));
    ASSERT_FALSE(quests.reroll(0, 1));

    resetContent();
}

TEST(test_quest_all_complete) {
    resetContent();
    QuestEngine& quests = QuestEngine::instance();
    quests.rollDaily(0xC0FFEE);

    ASSERT_FALSE(quests.allComplete());
    for (uint8_t i = 0; i < quests.activeCount(); i++) {
        quests.reportProgressById(quests.quest(i)->id, quests.quest(i)->target);
    }
    ASSERT_TRUE(quests.allComplete());
    ASSERT_EQ(quests.completedCount(), quests.activeCount());
}

int main() {
    printf("\n=== Content Engine Tests ===\n");

    RUN_TEST(test_baseline_is_always_available);
    RUN_TEST(test_baseline_covers_every_quest_kind);
    RUN_TEST(test_quest_lookup_by_id);
    RUN_TEST(test_out_of_range_accessors_return_null);
    RUN_TEST(test_pack_replaces_baseline);
    RUN_TEST(test_bad_pack_keeps_baseline);
    RUN_TEST(test_pack_parses_named_and_numeric_fields);
    RUN_TEST(test_pack_truncates_at_the_cap);

    RUN_TEST(test_dialogue_picks_a_line);
    RUN_TEST(test_dialogue_returns_null_when_nothing_matches);
    RUN_TEST(test_dialogue_respects_trait_and_stage_filters);
    RUN_TEST(test_dialogue_prefers_the_specific_line);
    RUN_TEST(test_dialogue_form_filter_needs_the_matching_form);
    RUN_TEST(test_undecided_pet_never_sees_a_form_line);
    RUN_TEST(test_form_line_outranks_generic_but_not_trait);
    RUN_TEST(test_form_parses_as_name_or_ordinal_and_degrades_safely);
    RUN_TEST(test_baseline_form_lines_reach_a_formed_pet);
    RUN_TEST(test_dialogue_memory_line_needs_an_accessor);
    RUN_TEST(test_dialogue_substitution_stays_in_bounds);
    RUN_TEST(test_dialogue_avoids_immediate_repeats);
    RUN_TEST(test_empty_pack_leaves_the_pet_speaking);

    RUN_TEST(test_quest_roll_fills_every_slot);
    RUN_TEST(test_quest_roll_has_no_duplicates);
    RUN_TEST(test_quest_roll_never_offers_an_impossible_quest);
    RUN_TEST(test_quest_roll_spreads_across_kinds);
    RUN_TEST(test_quest_progress_and_completion);
    RUN_TEST(test_quest_progress_saturates_at_target);
    RUN_TEST(test_quest_progress_by_id);
    RUN_TEST(test_quest_reroll_is_bounded);
    RUN_TEST(test_quest_reroll_refuses_a_finished_quest);
    RUN_TEST(test_quest_roll_is_reproducible);
    RUN_TEST(test_quest_roll_with_no_eligible_content);
    RUN_TEST(test_quest_all_complete);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
