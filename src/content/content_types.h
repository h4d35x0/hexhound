#pragma once

#include <stdint.h>

// ── HexHound - Content Data Types ────────────────────────────────
//
// Shared shapes for data-driven content. Firmware provides the engines; the
// content itself is data, so that quests, dialogue and items can ship without
// rebuilding every hardware target.
//
// Format today is plain JSON parsed with ArduinoJson (already a dependency).
// Format later is CBOR plus a signature. ONLY the loader in content_store
// changes for that; nothing in this header does, and no content authored
// against these structs gets rewritten. That is the whole reason these types
// exist before any content does.
//
// Sizing rule for everything here: the T-Dongle S3 has no PSRAM and about
// 250 KB of usable RAM. Fixed-size fields and small caps, never open-ended
// growth. If something only fits on a PSRAM board it belongs behind
// Caps::tier() == TIER_RICH, not in these core structs.

#define CONTENT_MAX_ID_LEN      24
#define CONTENT_MAX_TEXT_LEN    96
#define QUEST_MAX_ACTIVE         4    // daily quests offered at once
#define DIALOGUE_MAX_TAGS        4

// What a quest asks for. The engine tracks progress; the rules layer reports
// events. Deliberately coarse: a quest is something a person can finish in a
// day without being told to grind.
enum QuestKind : uint8_t {
    QUEST_CARE = 0,     // interact with the pet
    QUEST_EXPLORE,      // go somewhere, move, encounter something new
    QUEST_CYBER,        // run a patrol, review a tip, answer a challenge
    QUEST_LIFE,         // user-defined: focus session, break, go outside
    QUEST_KIND_COUNT
};

enum QuestStatus : uint8_t {
    QUEST_OFFERED = 0,
    QUEST_ACTIVE,
    QUEST_COMPLETE,
    QUEST_REROLLED
};

struct QuestDef {
    char     id[CONTENT_MAX_ID_LEN]   = {};
    char     text[CONTENT_MAX_TEXT_LEN] = {};
    QuestKind kind      = QUEST_CARE;
    uint16_t  target    = 1;      // how many times the tracked event must fire
    uint16_t  rewardXP  = 0;
    uint16_t  rewardBond = 0;
    // Capability gate. A quest requiring motion must not be offered on a board
    // that cannot measure it; offering an impossible quest is worse than
    // offering none. 0 means "any board".
    uint16_t  requiresCaps = 0;
};

struct QuestInstance {
    char        id[CONTENT_MAX_ID_LEN] = {};
    uint16_t    progress = 0;
    uint16_t    target   = 1;
    QuestKind   kind     = QUEST_CARE;
    QuestStatus status   = QUEST_OFFERED;
};

// When a dialogue line is eligible. The engine picks among matching lines; it
// does not evaluate arbitrary expressions, because content must never be able
// to express logic the firmware cannot bound.
enum DialogueContext : uint8_t {
    DLG_GREETING = 0,       // waking the screen
    DLG_IDLE,               // ambient chatter
    DLG_PATROL_DONE,
    DLG_THREAT_FOUND,
    DLG_QUEST_DONE,
    DLG_GAME_WIN,
    DLG_GAME_LOSE,
    DLG_RETURN_AFTER_ABSENCE,
    DLG_HIBERNATE_WAKE,
    DLG_EVOLVED,
    DLG_CONTEXT_COUNT
};

// The value a pet reports for "I have not earned a behavioural form yet". It
// is PetForm's FORM_UNSET, spelled here as a plain integer so this header stays
// free of config.h: content types must not depend on board configuration.
//
// Note the asymmetry with the 0xFF "any" used by the line's own filters. 0xFF
// describes a LINE that suits every pet; 0 describes a PET that suits only
// untagged lines. An undecided pet must never be handed a line written for a
// Guardian, so its default is the strict value, not the permissive one.
#define DIALOGUE_FORM_UNSET  0

struct DialogueLine {
    char            text[CONTENT_MAX_TEXT_LEN] = {};
    DialogueContext context = DLG_IDLE;
    // Optional filters. 0xFF means "any".
    uint8_t         trait   = 0xFF;   // Trait value this line suits
    uint8_t         stage   = 0xFF;   // PetStage this line suits
    // PetForm this line suits. The third filter, and the only place form is
    // allowed to affect the game at all: a form-tagged line lets the writing
    // lean towards where a pet is heading without a screen ever announcing it,
    // and without a progress bar to grind. Form gives no stats, no unlocks and
    // no rewards - just a different sentence now and then.
    uint8_t         form    = 0xFF;
    // Substitution slots the engine may fill from pet memory, e.g. a visit
    // count. Content declares intent; the engine owns the numbers, so content
    // can never fabricate a statistic.
    uint8_t         memorySlot = 0xFF;
};

// Result of one minigame round. Kept tiny and uniform so the reward path is
// identical for every game and a new game needs no reward plumbing.
struct MinigameResult {
    uint32_t score      = 0;
    uint16_t durationMs = 0;
    bool     completed  = false;   // finished vs abandoned
    bool     newBest    = false;
};
