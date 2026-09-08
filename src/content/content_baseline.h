#pragma once

#include "content_types.h"
#include "content_store.h"
#include "../config.h"
#include "../board/capabilities.h"

// ── HexHound - Baseline Content Pack ─────────────────────────────
//
// The content a device ships with. This is DATA, not logic: every row here has
// a one-to-one mapping onto a JSON object in /content/*.json, and the loader
// applies both through the same validation. Anything that cannot be expressed
// as a row belongs in an engine, not here.
//
// This pack is why an erased device still has a personality. It is loaded
// first, unconditionally, and a SPIFFS pack only replaces it once that pack has
// parsed successfully.
//
// PROGMEM is a no-op on Xtensa (const data is already memory-mapped from
// flash), so these tables are read directly rather than through pgm_read_*.
// The marker is kept because it states the intent: these bytes must never be
// copied into RAM wholesale, only row by row as the loader needs them.

// Row shapes mirroring the JSON objects. Pointers rather than fixed char
// arrays, so the flash cost is the length of the strings and not 96 bytes per
// line; the loader copies into the bounded fields of DialogueLine / QuestDef.
struct BaselineDialogue {
    const char* text;
    uint8_t     context;
    uint8_t     trait;        // 0xFF = any
    uint8_t     stage;        // 0xFF = any
    uint8_t     form;         // 0xFF = any (PetForm otherwise)
    uint8_t     memorySlot;   // DIALOGUE_MEMORY_NONE = no substitution
};

struct BaselineQuest {
    const char* id;
    const char* text;
    uint8_t     kind;
    uint16_t    target;
    uint16_t    rewardXP;
    uint16_t    rewardBond;
    uint16_t    requiresCaps;   // 0 = any board
};

// {n} is the substitution token. It is only expanded when the row declares a
// memory slot AND the caller has installed an accessor, so a line can never
// print a number the firmware did not supply.
//
// The `form` column is the third filter and the ONLY place behavioural form is
// allowed to show up in the game. Form gives no stats, no unlocks and no
// rewards; it changes a sentence now and then. That is deliberate. A pet whose
// identity is a modifier becomes a build to min-max, and the correct way to
// play it becomes farming the counter rather than living with the thing.
//
// The form-tagged lines below never say the word Pathfinder or Guardian, never
// mention a counter, and never hint at how close anything is. They are simply
// what a pet that has spent its life walking sounds like. Someone who has been
// walking notices their pet talks about distance; nobody is told to.
static const BaselineDialogue BASELINE_DIALOGUE[] PROGMEM = {
    // Greeting
    { "Back already. I kept the antenna warm.",
      DLG_GREETING, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "You again. That is {n} check-ins now.",
      DLG_GREETING, 0xFF, 0xFF, 0xFF, MEM_SLOT_VISITS },
    { "Oh good, you brought hands. I have questions.",
      DLG_GREETING, TRAIT_CURIOUS, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Mm. It is you. Five more minutes.",
      DLG_GREETING, TRAIT_SLEEPY, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Something taps back from inside the shell.",
      DLG_GREETING, 0xFF, STAGE_EGG, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Boots on. Which way are we going?",
      DLG_GREETING, 0xFF, 0xFF, FORM_PATHFINDER, DIALOGUE_MEMORY_NONE },
    { "Perimeter is quiet. It stayed that way.",
      DLG_GREETING, 0xFF, 0xFF, FORM_GUARDIAN, DIALOGUE_MEMORY_NONE },

    // Ambient
    { "Listening to the noise floor.",
      DLG_IDLE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Somewhere out there a router is lying about its name.",
      DLG_IDLE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "I could break something. Purely for research.",
      DLG_IDLE, TRAIT_CHAOTIC, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Nothing gets past me while you are looking away.",
      DLG_IDLE, TRAIT_PROTECTIVE, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "I have logged {n} networks I had never met before.",
      DLG_IDLE, 0xFF, 0xFF, 0xFF, MEM_SLOT_NEW_NETWORKS },
    { "The good corners are the ones nobody points at.",
      DLG_IDLE, 0xFF, 0xFF, FORM_PATHFINDER, DIALOGUE_MEMORY_NONE },
    { "Everything is filed. I could find it in the dark.",
      DLG_IDLE, 0xFF, 0xFF, FORM_ARCHIVIST, DIALOGUE_MEMORY_NONE },
    { "Give me something that does not want to be read.",
      DLG_IDLE, 0xFF, 0xFF, FORM_CIPHER, DIALOGUE_MEMORY_NONE },
    { "I have been extremely well behaved. Ask nobody.",
      DLG_IDLE, 0xFF, 0xFF, FORM_GREMLIN, DIALOGUE_MEMORY_NONE },
    { "It is quieter when it is only the two of us.",
      DLG_IDLE, 0xFF, 0xFF, FORM_PACKMASTER, DIALOGUE_MEMORY_NONE },

    // Patrol
    { "Patrol done. The air is busy today.",
      DLG_PATROL_DONE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "That makes {n} patrols we have walked together.",
      DLG_PATROL_DONE, 0xFF, 0xFF, 0xFF, MEM_SLOT_PATROLS },
    { "Good haul. More packets, more snacks.",
      DLG_PATROL_DONE, TRAIT_GREEDY, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },

    // Findings
    { "That one is pretending to be something it is not.",
      DLG_THREAT_FOUND, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "I see you. Come closer.",
      DLG_THREAT_FOUND, TRAIT_BRAVE, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Stand behind me. I have had my eye on that one.",
      DLG_THREAT_FOUND, 0xFF, 0xFF, FORM_GUARDIAN, DIALOGUE_MEMORY_NONE },

    // Quests
    { "Task cleared. Write it in the log.",
      DLG_QUEST_DONE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "{n} quests finished. I am keeping score.",
      DLG_QUEST_DONE, 0xFF, 0xFF, 0xFF, MEM_SLOT_QUESTS_DONE },

    // Games
    { "Beat that. I dare you.",
      DLG_GAME_WIN, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "Best of {n}. That is my record and I am keeping it.",
      DLG_GAME_WIN, 0xFF, 0xFF, 0xFF, MEM_SLOT_BEST_SCORE },
    { "We do not talk about that round.",
      DLG_GAME_LOSE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },

    // Absence and power states
    { "You were gone {n} days. I counted every one.",
      DLG_RETURN_AFTER_ABSENCE, 0xFF, 0xFF, 0xFF, MEM_SLOT_DAYS_AWAY },
    { "Radios coming back up. Give me a second.",
      DLG_HIBERNATE_WAKE, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },

    // Evolution
    { "Something changed. I can feel the new antenna.",
      DLG_EVOLVED, 0xFF, 0xFF, 0xFF, DIALOGUE_MEMORY_NONE },
    { "I am the whole watch now. Go on, sleep.",
      DLG_EVOLVED, 0xFF, STAGE_SENTINEL, 0xFF, DIALOGUE_MEMORY_NONE }
};

static const uint8_t BASELINE_DIALOGUE_COUNT =
    (uint8_t)(sizeof(BASELINE_DIALOGUE) / sizeof(BASELINE_DIALOGUE[0]));

// Targets are deliberately small. A daily quest is something a person finishes
// in a day without being told to grind, so nothing here asks for a number that
// only a session of deliberate farming reaches.
static const BaselineQuest BASELINE_QUESTS[] PROGMEM = {
    // Care
    { "care.feed",     "Feed HexHound twice today.",
      QUEST_CARE,    2,  15, 3, 0 },
    { "care.play",     "Play one round of anything.",
      QUEST_CARE,    1,  10, 4, 0 },
    { "care.checkin",  "Check on the pet three times.",
      QUEST_CARE,    3,  10, 3, 0 },

    // Explore. The two motion quests are gated: a board with no IMU cannot
    // measure movement, and offering a quest that board can never complete is
    // worse than offering nothing.
    { "explore.steps", "Walk 200 steps together.",
      QUEST_EXPLORE, 200, 30, 4, CAP_IMU },
    { "explore.shake", "Wake the pet with a shake.",
      QUEST_EXPLORE, 1,  10, 2, CAP_IMU },
    { "explore.newnet","Meet five networks you have never seen.",
      QUEST_EXPLORE, 5,  25, 3, 0 },
    { "explore.roam",  "Run a patrol somewhere new.",
      QUEST_EXPLORE, 1,  20, 3, 0 },

    // Cyber
    { "cyber.patrol",  "Run two patrols.",
      QUEST_CYBER,   2,  20, 2, 0 },
    { "cyber.triage",  "Review one open-network finding.",
      QUEST_CYBER,   1,  15, 2, 0 },
    { "cyber.tracker", "Identify a tracker beacon.",
      QUEST_CYBER,   1,  25, 3, 0 },

    // Life. User-defined and unverifiable by design: the pet takes your word
    // for it, which is the only honest way to track something off-device.
    { "life.focus",    "Take one 25 minute focus session.",
      QUEST_LIFE,    1,  20, 4, 0 },
    { "life.break",    "Stand up and stretch twice.",
      QUEST_LIFE,    2,  10, 3, 0 },
    { "life.outside",  "Go outside for ten minutes.",
      QUEST_LIFE,    1,  20, 4, 0 }
};

static const uint8_t BASELINE_QUEST_COUNT =
    (uint8_t)(sizeof(BASELINE_QUESTS) / sizeof(BASELINE_QUESTS[0]));
