#pragma once

#include <stdint.h>
#include "board/board_profile.h"

// ── HexHound - Global Configuration ──────────────────────────────

// LED
#define NUM_LEDS          1
#define LED_BRIGHTNESS    40

// Pet defaults
#define PET_NAME_DEFAULT  "HexHound"
#define STAT_MAX          100
#define STAT_MIN          0

// Timing (ms)
#define TICK_INTERVAL_MS       30000   // 30s per hunger/energy tick

// Idle ticks before the pet hibernates and stops decaying entirely.
// 120 ticks x 30s = 1 hour of powered, untouched time. Long enough that it
// never triggers mid-use, short enough that a device left on a desk overnight
// wakes up the same as you left it rather than starved.
#define HIBERNATE_AFTER_TICKS  120

// Powered ticks before the quest day rolls over. 2880 x 30s = 24h of powered
// time. There is no RTC, so a real calendar day is not measurable; the day
// also rolls on power-on, which covers the carried-and-recharged pattern the
// tick counter alone would miss, and the tick counter covers the left-on-a-desk
// pattern that power-on alone would miss. Rebooting to reroll the day is
// therefore possible; on a single-player pet with no leaderboard that is a
// curiosity, not an exploit. Replace both with a real date when NTP lands.
#define TICKS_PER_QUEST_DAY    2880
#define WIFI_SCAN_TIMEOUT_MS   8000
#define BLE_SCAN_DURATION_S    5
#define BUTTON_LONG_PRESS_MS   1000
#define DEBOUNCE_MS            50

// Hold BOTH buttons this long for soft power off (deep sleep). Only meaningful
// on a board that defines PIN_BUTTON_2. Deliberately longer than a long press so
// holding A and brushing B cannot put the device to sleep mid-patrol.
#define BUTTON_BOTH_HOLD_MS    2000
#define UI_REFRESH_MS          250
#define SAVE_DELAY_MS          2000    // debounce saves

// How long the expedition report holds the screen before returning to the menu
// on its own. A long press still dismisses it immediately; this is only so a
// device put back in a pocket at the end of a roam does not sit on a static
// report forever. Everything the report shows was already committed to the pet
// by RoamModule::end() before it was drawn, so leaving it costs nothing.
#define ROAM_REPORT_HOLD_MS    10000

// A longer hold to END an expedition was TRIED AND REVERTED on 2026-08-04, and
// the reason is worth keeping so nobody re-adds it.
//
// It was aimed at roams that appeared to end themselves in a pocket. They were
// not ending on the button at all: shake was mapped to a long press, and gating
// gestures off during a roam had ALREADY fixed it. The journal's "button held"
// on those walks was the owner deliberately ending them, which is the expected
// reason and was misread as evidence of a fault.
//
// So it fixed nothing, and it cost real usability: at 3000 ms the exit felt
// like six seconds on hardware, because a human press on a small tactile switch
// chatters and every bounce restarts the timer, while steady fabric pressure
// does not. It penalised the person and not the failure mode. If accidental
// endings ever come back, read the btn edge count now in the roam journal entry
// FIRST - one press is a person, fifteen is a pocket working the switch.

// Evolution thresholds
#define EGG_INTERACTIONS       10
#define EGG_WIFI_SCANS         1
#define EGG_USB_CONNECTS       1
#define STAGE2_XP              0       // Packet Pup (post-hatch)
#define STAGE3_XP              300     // Beacon Beast
#define STAGE4_XP              900     // Gremlin Mode
#define STAGE5_XP              2400    // Sentinel

// Patrol progression tuning
#define XP_FOOD_PER_POINT      25
#define XP_NEW_WIFI_CAPTURE    2
#define XP_NEW_BLE_CAPTURE     3
#define WIFI_FOOD_PER_NETWORK  4
#define BLE_FOOD_PER_DEVICE    3

// Persistent capture memory
#define MAX_SEEN_WIFI_CAPTURES 64
#define MAX_SEEN_BLE_CAPTURES  64

// Decay rates per tick
#define HUNGER_DECAY           2
#define ENERGY_DECAY           1
#define MOOD_DECAY             1

// SD card paths
// PATH_PET_STATE is the LEGACY single-file save. It is still read so units
// flashed before the A/B change keep their pet, but it is never written to
// again - saves alternate between the _A and _B slots below so that a power
// loss mid-write can never destroy the only copy.
#define PATH_PET_STATE    "/sd/pet_state.json"
#define PATH_PET_STATE_A  "/sd/pet_state_a.json"
#define PATH_PET_STATE_B  "/sd/pet_state_b.json"
#define PATH_JOURNAL      "/sd/journal.log"
#define PATH_CONFIG       "/sd/config.json"

// ── Behavioural form (Phase 1) ────────────────────────────────────────────
// A SECOND axis, independent of PetStage. Stage is age and is earned with XP;
// form is identity and is earned by what the owner actually does with the pet.
// A Legendary Pathfinder and a Legendary Cipher are the same age and nothing
// alike, which is the point: two people should not end up with the same pet.
//
// FORM_UNSET means not enough behaviour to call it yet. It is deliberately 0 so
// an older save, which has no form field at all, defaults to "undecided"
// rather than silently claiming to be a Pathfinder.
enum PetForm : uint8_t {
    FORM_UNSET = 0,
    FORM_PATHFINDER,     // walking, travel, new environments
    FORM_GUARDIAN,       // trusted-device watching, defensive quests
    FORM_ARCHIVIST,      // collections, journal, discoveries
    FORM_CIPHER,         // puzzles and educational challenges
    FORM_GREMLIN,        // minigames, playful missions, mischief
    FORM_PACKMASTER,     // social encounters and co-op
    FORM_COUNT
};

// Behaviour counters feeding form selection, in the same order as PetForm - 1.
#define FORM_BEHAVIOUR_COUNT   6
// Behaviour needed before a form is claimed at all. Below this the pet is
// FORM_UNSET and says nothing about direction, because declaring an identity
// off three data points would be noise dressed as insight.
#define FORM_MIN_EVIDENCE      25
// How far ahead the leading behaviour must be before it takes the form, as a
// percentage of the runner-up. A near-tie should stay undecided rather than
// flip-flopping every patrol.
#define FORM_LEAD_PERCENT      140

// ── Inventory (Phase 1) ───────────────────────────────────────────────────
// Fixed-size counts indexed by item id. Fixed rather than dynamic because the
// T-Dongle S3 has no PSRAM and a growable inventory on a fragmenting heap is
// how you get an allocation failure in the renderer three screens away.
#define ITEM_TYPE_COUNT        24
#define ITEM_STACK_MAX         999

// ── Den (Phase 1) ─────────────────────────────────────────────────────────
// Placement slots. TIER_CORE uses the first DEN_SLOTS_CORE of them; TIER_RICH
// uses all of them. One array either way, so the save is identical across
// boards and a pet can move between them without losing its room.
#define DEN_SLOT_COUNT         8
#define DEN_SLOTS_CORE         3

// ── Worn cosmetics ────────────────────────────────────────────────────────
// Width of PetState::wornSlots[]: one per body position a cosmetic can occupy.
// The positions themselves are the WearAnchor enum in content/item_defs.h,
// which pet_core.h cannot include (item_defs.h includes this file), so the
// number lives here and item_defs.h static_asserts that the two agree.
#define WEAR_SLOT_COUNT        2

// Personality traits
enum Trait : uint8_t {
    TRAIT_CURIOUS = 0,
    TRAIT_PROTECTIVE,
    TRAIT_CHAOTIC,
    TRAIT_SLEEPY,
    TRAIT_GREEDY,
    TRAIT_BRAVE,
    TRAIT_COUNT
};

// -- Stage list: the single source of truth for every per-stage table -----
//
// Before this there were FIVE parallel per-stage structures across three
// files: STAGE_NAMES (upper), PetCore::stageName() (mixed), STAGE_TAG (three
// glyphs, in ui_inventory.cpp), STAGE_ABILITY and STAGE_COLOR. A rename had to
// find all five, and one did not: the 2026-08-30 company scrub fixed
// STAGE_NAMES and left a company-initialled name on the home screen for two days, because
// it searched for the full name and the other table said something shorter.
//
// Every spelling of a stage now sits on ONE line. A partial rename shows up in
// the diff instead of on the glass, and a stage cannot be added to the enum
// without adding its row here, because the enum is generated from this list.
//
// Columns: enumerator, UPPER name, Mixed name, 3-glyph tag, ability, color.
//
// Expansions must not reorder this list. Stages are 1-based and every table is
// indexed [stage - 1]; saved pet state stores the numeric value.
#define HH_STAGE_LIST(X) \
    X(STAGE_EGG,          "EGG",          "Egg",          "EGG", "",                       0x8410) \
    X(STAGE_PACKET_PUP,   "PACKET PUP",   "Packet Pup",   "PUP", "WIFI RECONNAISSANCE",    0x07FF) \
    X(STAGE_BEACON_BEAST, "BEACON BEAST", "Beacon Beast", "BST", "BLE BEACON DETECTION",   0xFD20) \
    X(STAGE_GREMLIN,      "GREMLIN MODE", "Gremlin Mode", "GRM", "USB AWARENESS MISSIONS", 0x07E0) \
    X(STAGE_SENTINEL,     "SENTINEL",     "Sentinel",     "SNT", "ANOMALY DETECTION",      0xF800)

// Evolution stages. STAGE__BASE is not a stage: it exists so the generated
// enumerators start at 1, which is the value saved pet state already holds.
enum PetStage : uint8_t {
    STAGE__BASE = 0,
#define HH_STAGE_ENUMERATOR(e, u, m, t, a, c) e,
    HH_STAGE_LIST(HH_STAGE_ENUMERATOR)
#undef HH_STAGE_ENUMERATOR
};

// Notification levels
enum NotifLevel : uint8_t {
    NOTIF_INFO = 0,
    NOTIF_WARN,
    NOTIF_CRIT
};

// UI screens
enum Screen : uint8_t {
    SCREEN_HOME = 0,
    SCREEN_PATROL_HUD,  // live scanning HUD (portrait mode)
    SCREEN_PATROL,       // final patrol results
    SCREEN_ALERT,
    SCREEN_JOURNAL,
    SCREEN_EVOLVE,       // evolution cutscene (full takeover)
    SCREEN_MENU,         // main menu
    SCREEN_CONFIG,       // trusted SSIDs/hosts config
    SCREEN_STATS,        // full pet stats
    SCREEN_MISSIONS,     // mission select (Gremlin Mode)
    SCREEN_MISSION_BRIEF,// mission briefing / confirm before execute
    SCREEN_HWTEST,
    // Phase 0 companion screens. Appended, never inserted: main.cpp switches on
    // these values and saved config may reference them.
    SCREEN_QUESTS,       // daily quest list
    SCREEN_GAMES,        // minigame select
    SCREEN_GAME_PLAY,    // a minigame has the screen
    // Phase 1. Reserved centrally so no feature branch edits this enum.
    SCREEN_INVENTORY,    // materials and craftable cosmetics
    SCREEN_DEN,          // the pet's room
    SCREEN_ROAM,         // roam expedition in progress
    SCREEN_ROAM_REPORT,  // what the expedition brought back
    SCREEN_REPORT,       // field report since last time
    // Phase 3. Appended, same rule as above.
    SCREEN_UPDATE,       // firmware version, OTA status, and the arm gate
    // Phase 4. Appended, same rule as above.
    SCREEN_CLOSET        // what the pet is wearing
        // hardware diagnostics
};

// ── Stage metadata (names, abilities, colors) ────────────────────────────

// All three generated from HH_STAGE_LIST above, in list order, and all three
// indexed by PetStage - 1.

// Stage display names, upper case (indexed by PetStage - 1)
#define HH_STAGE_NAME(e, u, m, t, a, c) u,
static const char* const STAGE_NAMES[] = { HH_STAGE_LIST(HH_STAGE_NAME) };
#undef HH_STAGE_NAME

// Ability unlocked per stage
#define HH_STAGE_ABILITY(e, u, m, t, a, c) a,
static const char* const STAGE_ABILITY[] = { HH_STAGE_LIST(HH_STAGE_ABILITY) };
#undef HH_STAGE_ABILITY

// RGB565 accent color per stage
#define HH_STAGE_COLOR(e, u, m, t, a, c) c,
static const uint16_t STAGE_COLOR[] = { HH_STAGE_LIST(HH_STAGE_COLOR) };
#undef HH_STAGE_COLOR

// A table one row short is an out-of-bounds read at a dozen unguarded
// [stage - 1] call sites, so bind every table's length to the last enumerator.
// STAGE_SENTINEL is both the highest stage value and the number of stages,
// because the enumerators start at 1.
static_assert(sizeof(STAGE_NAMES)   / sizeof(STAGE_NAMES[0])   == STAGE_SENTINEL,
              "STAGE_NAMES must have exactly one entry per stage");
static_assert(sizeof(STAGE_ABILITY) / sizeof(STAGE_ABILITY[0]) == STAGE_SENTINEL,
              "STAGE_ABILITY must have exactly one entry per stage");
static_assert(sizeof(STAGE_COLOR)   / sizeof(STAGE_COLOR[0])   == STAGE_SENTINEL,
              "STAGE_COLOR must have exactly one entry per stage");

// ui_evolve.cpp sizes the cutscene title from the longest stage name and says
// so only in a comment: "The longest name at size 2 is 144px (BEACON BEAST)".
// A comment is not a control. Twelve characters is what that layout was
// measured against on a 160px panel, so exceeding it is a compile error rather
// than a title that runs off both edges.
#define HH_STAGE_NAME_FITS(e, u, m, t, a, c) sizeof(u) - 1 <= 12 &&
static_assert(HH_STAGE_LIST(HH_STAGE_NAME_FITS) true,
              "a stage name is longer than 12 chars; re-measure the evolution "
              "cutscene title on the 160px panel before raising this");
#undef HH_STAGE_NAME_FITS

// ui_inventory.cpp pads its locked-recipe column to a fixed width on the
// assumption that every tag is three glyphs.
#define HH_STAGE_TAG_FITS(e, u, m, t, a, c) sizeof(t) == 4 &&
static_assert(HH_STAGE_LIST(HH_STAGE_TAG_FITS) true,
              "a stage tag is not exactly 3 characters; ui_inventory.cpp's "
              "right-hand column assumes a fixed width");
#undef HH_STAGE_TAG_FITS
