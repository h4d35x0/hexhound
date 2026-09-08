#pragma once

#include <stdint.h>
#include <string.h>

#include "content_types.h"
#include "../config.h"

// ── HexHound - Items and Recipes (baseline content) ──────────────
//
// This file is DATA. Every row below maps one-to-one onto a JSON object, the
// same arrangement content_baseline.h uses for dialogue and quests, and the
// same rule applies: anything that cannot be expressed as a row belongs in an
// engine (src/pet/pet_inventory.cpp), not here.
//
// ── THE ONE RULE THAT IS NOT NEGOTIABLE ───────────────────────────────────
//
// An item is COSMETIC. It changes how the pet and its room LOOK, and nothing
// else. There is deliberately no stat field, no XP field, no multiplier field
// and no "bonus" field on ItemDef, because the moment one exists somebody sets
// it and the pet stops being a companion and becomes a build to optimise.
//
// The absence is enforced two ways:
//   1. ItemDef carries no number an engine could read as a modifier. Its only
//      numbers are a display colour and a stage gate.
//   2. ItemDefShapeProbe below fails the build if the struct grows. That is a
//      tripwire, not a proof - it exists to make whoever adds a field read this
//      comment before widening it.
//
// What an item MAY unlock is presentation: a den decoration, something worn on
// the pet, an idle animation, a flourish on a results screen. All of those are
// selected by the item's `slot`, which the den and home screens read; none of
// them reach PetStats, and no engine in this workstream calls a stat mutator.
//
// ── Item ids are save indices, so the table is compiled-in only ────────────
//
// PetState::items[] is indexed by item id, and that array is persisted. If a
// SPIFFS pack could add, remove or reorder items, a content update would
// silently turn a player's Signal Scarf into a Cracked Lens. So the item table
// itself is NOT pack-replaceable: the numeric id is the identity and the
// baseline owns it. Only the RECIPE table is overridable (see pet_inventory.h),
// and it refers to items by string id, which is exactly why a recipe naming an
// unknown item can be REJECTED at load instead of pointing at a stranger's
// inventory slot.
//
// ── Include rule ──────────────────────────────────────────────────────────
//
// The tables below are `static const` and the two lookups that read them are
// `inline`, so a translation unit that includes this header without calling
// either emits NEITHER: the inline functions are not instantiated, the tables
// are left unreferenced, and the compiler drops them. Only src/pet/
// pet_inventory.cpp actually reads them today, so only pet_inventory.o carries
// the ~600 bytes of rows and string pool.
//
// UI code goes through Inventory's accessors rather than reading the table, so
// that stays true as screens are added.

// Max ingredients in one recipe. Three is the widest a 160x80 row can explain
// without a second screen, which is the real constraint.
#define RECIPE_MAX_INPUTS   3

// Recipe table capacity in RAM. A pack larger than this is truncated at load,
// never allowed to allocate past it.
#define RECIPE_MAX_DEFS     16

// Where an override pack lives. Same convention as the dialogue and quest
// packs: a SIGNED CBOR pack, not a plain JSON file. The .json path is
// deliberately no longer read, because a file anyone can drop on SPIFFS must
// not be able to change what the pet can craft without a signature behind it.
#define CONTENT_PATH_RECIPES  "/content/recipes.hcp"

// Largest recipe pack the loader will read. Anything bigger is refused whole,
// because a half-parsed pack is worse than the baseline it replaced. Smaller
// than CONTENT_MAX_PACK_BYTES on purpose: RECIPE_MAX_DEFS rows of three
// ingredients is about 2 KB of JSON, so 4 KB is generous and still bounded.
#define RECIPE_MAX_PACK_BYTES  4096

// Sentinel for "no such item". 0xFF, not 0, because 0 is a real item id and a
// failed lookup that returns 0 silently means "scrap".
#define ITEM_ID_NONE  0xFF

// What an item IS. The split is load-bearing: a material is spent, a cosmetic
// is the thing you were spending it on, and only a cosmetic may ever be the
// output of a recipe.
enum ItemKind : uint8_t {
    ITEM_MATERIAL = 0,   // gathered on patrol and roam, consumed by recipes
    ITEM_COSMETIC,       // crafted, then worn, placed or played
    ITEM_KIND_COUNT
};

// Where a cosmetic shows up. This is the ONLY thing an item unlocks, and every
// value here is a presentation surface owned by a screen, not by the stat
// layer. Materials are COSMETIC_NONE.
enum CosmeticSlot : uint8_t {
    COSMETIC_NONE = 0,
    COSMETIC_WORN,       // on the pet itself (home screen, evolution hero shot)
    COSMETIC_DEN,        // placed in the den (SCREEN_DEN, PetState::denSlots)
    COSMETIC_FLOURISH,   // an idle animation or a results-screen embellishment
    COSMETIC_SLOT_COUNT
};

// WHERE ON THE PET a COSMETIC_WORN item sits.
//
// Two anchors rather than one worn slot, because the content was authored that
// way and has been since the item table was written: ANTENNA HAT and RECON
// GOGGLES both go on the head, SIGNAL SCARF and LED COLLAR both go round the
// neck. One slot would have made those pairs mutually exclusive for no reason a
// wearer could see, and would have meant a Beacon Beast choosing between its
// goggles and its collar.
//
// Anything that is not COSMETIC_WORN is WEAR_NONE, including every material, so
// the value doubles as "is this wearable at all".
enum WearAnchor : uint8_t {
    WEAR_NONE = 0,
    WEAR_HEAD,
    WEAR_NECK,
    WEAR_ANCHOR_COUNT
};

// WEAR_SLOT_COUNT is the width of PetState::wornSlots[] and is defined in
// config.h, because pet_core.h needs it and cannot include this file (this file
// includes config.h). Adding an anchor here without widening the array there
// would silently write past the end of a persisted struct, so the build refuses
// to let the two drift.
static_assert(WEAR_ANCHOR_COUNT - 1 == WEAR_SLOT_COUNT,
              "a WearAnchor was added or removed without changing "
              "WEAR_SLOT_COUNT in config.h; PetState::wornSlots[] is that wide");

struct ItemDef {
    const char*  id;         // stable string id; recipes and packs use this
    const char*  name;       // display name, short enough for a 160x80 row
    uint8_t      kind;       // ItemKind
    uint8_t      slot;       // CosmeticSlot; COSMETIC_NONE for materials
    uint8_t      minStage;   // PetStage before which it is not offered; 0 = any
    uint16_t     color;      // RGB565 accent, matched to the baked icon
    uint8_t      anchor;     // WearAnchor; WEAR_NONE unless COSMETIC_WORN
    // NO stat, xp, bonus, multiplier or duration field. See the header comment.
};

// One recipe as authored. `input[i] == nullptr` ends the ingredient list, so a
// two-ingredient recipe costs nothing extra to declare.
struct BaselineRecipe {
    const char*  id;
    const char*  output;                    // item string id, must be COSMETIC
    const char*  input[RECIPE_MAX_INPUTS];
    uint16_t     qty[RECIPE_MAX_INPUTS];
    uint8_t      minStage;                  // PetStage gate; 0 = any
};

// One recipe as the engine holds it: string ids resolved to numeric item ids,
// so a craft never does a string compare and an unresolvable recipe cannot get
// this far. Built only by Inventory's validator.
struct Recipe {
    char     id[CONTENT_MAX_ID_LEN] = {};
    uint8_t  output     = ITEM_ID_NONE;
    uint8_t  inputCount = 0;
    uint8_t  input[RECIPE_MAX_INPUTS] = { ITEM_ID_NONE, ITEM_ID_NONE,
                                          ITEM_ID_NONE };
    uint16_t qty[RECIPE_MAX_INPUTS]   = { 0, 0, 0 };
    uint8_t  minStage   = 0;
};

// ── Shape tripwire ────────────────────────────────────────────────────────
// If the static_assert in the table section below fails you probably just
// added a field to ItemDef. Before widening it, re-read the rule at the top of
// this file: if the field is a stat, an XP value, a multiplier or a timer, the
// fix is to delete it, not to update this struct.
//
// A mirror struct rather than a hardcoded byte count, so the check is correct
// on a 32-bit Xtensa target and a 64-bit test host alike.
struct ItemDefShapeProbe {
    const char* id;
    const char* name;
    uint8_t     kind;
    uint8_t     slot;
    uint8_t     minStage;
    uint16_t    color;
};

// PROGMEM is a no-op on Xtensa (const data is already memory-mapped from
// flash); the marker states the intent, which is that these rows are read in
// place and never copied into RAM wholesale.

// ── Materials ─────────────────────────────────────────────────────────────
// Junk with a story. Every one of these is something a recon pet would
// plausibly drag home from a patrol, and none of them does anything on its own.
//
// ── Cosmetics ─────────────────────────────────────────────────────────────
// The reason the materials exist. Worn, placed or played; never equipped for an
// effect, because there are no effects.
//
// Item ids ARE indices into this table and into PetState::items[]. APPEND ONLY.
// Never insert, never reorder, never delete a row: a save written by an older
// firmware indexes this array positionally.
static const ItemDef ITEM_DEFS[] PROGMEM = {
    // ── Materials (ids 0..7) ──────────────────────────────────────────────
    { "mat.scrap",   "SCRAP",          ITEM_MATERIAL, COSMETIC_NONE, 0, 0x8410 },
    { "mat.signal",  "SIGNAL FRAG",    ITEM_MATERIAL, COSMETIC_NONE, 0, 0x07FF },
    { "mat.circuit", "CIRCUIT BIT",    ITEM_MATERIAL, COSMETIC_NONE, 0, 0x07E0 },
    { "mat.shard",   "DATA SHARD",     ITEM_MATERIAL, COSMETIC_NONE, 0, 0x881F },
    { "mat.glitch",  "GLITCH FRAG",    ITEM_MATERIAL, COSMETIC_NONE, 0, 0xF81E },
    { "mat.copper",  "COPPER WIRE",    ITEM_MATERIAL, COSMETIC_NONE, 0, 0xFB60 },
    { "mat.lens",    "CRACKED LENS",   ITEM_MATERIAL, COSMETIC_NONE, 0, 0xAEDF },
    { "mat.ferrite", "FERRITE BEAD",   ITEM_MATERIAL, COSMETIC_NONE, 0, 0x5AEB },

    // ── Worn cosmetics (ids 8..11) ────────────────────────────────────────
    { "cos.antenna", "ANTENNA HAT",    ITEM_COSMETIC, COSMETIC_WORN,
      STAGE_PACKET_PUP,   0x07FF, WEAR_HEAD },
    { "cos.scarf",   "SIGNAL SCARF",   ITEM_COSMETIC, COSMETIC_WORN,
      STAGE_PACKET_PUP,   0xF81E, WEAR_NECK },
    { "cos.goggles", "RECON GOGGLES",  ITEM_COSMETIC, COSMETIC_WORN,
      STAGE_BEACON_BEAST, 0xFD20, WEAR_HEAD },
    { "cos.collar",  "LED COLLAR",     ITEM_COSMETIC, COSMETIC_WORN,
      STAGE_BEACON_BEAST, 0x07E0, WEAR_NECK },

    // ── Den cosmetics (ids 12..16) ────────────────────────────────────────
    { "cos.lamp",    "DESK LAMP",      ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_PACKET_PUP,   0xFFE0 },
    { "cos.rug",     "STATIC RUG",     ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_PACKET_PUP,   0x8410 },
    { "cos.poster",  "FARADAY POSTER", ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_PACKET_PUP,   0x07FF },
    { "cos.fern",    "SERVER FERN",    ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_BEACON_BEAST, 0x07E0 },
    { "cos.lantern", "BEACON LAMP",    ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_BEACON_BEAST, 0xFD20 },

    // ── Flourishes (ids 17..19) ───────────────────────────────────────────
    { "cos.confetti","PACKET FLURRY",  ITEM_COSMETIC, COSMETIC_FLOURISH,
      STAGE_GREMLIN,      0xF81E },
    { "cos.sparks",  "SOLDER SPARKS",  ITEM_COSMETIC, COSMETIC_FLOURISH,
      STAGE_GREMLIN,      0xFB60 },
    { "cos.aurora",  "NOISE AURORA",   ITEM_COSMETIC, COSMETIC_FLOURISH,
      STAGE_SENTINEL,     0x881F },

    // ── Den cosmetics, second set (ids 20..22) ────────────────────────────
    // DEN_SLOT_COUNT is 8 and the first set filled only five of them, so a pet
    // that had crafted every den piece in the game still stood in a room with
    // three empty brackets in it. That is the room telling its keeper they are
    // not finished when in fact they were. These three close the gap, and they
    // are gated so the room fills as the pet GROWS rather than all at once:
    // four pieces reachable at Packet Pup, two more at Beacon Beast, one at
    // Gremlin, one at Sentinel. Eight pieces for eight brackets.
    //
    // APPENDED, never inserted. See the rule above the table: an id is a save
    // index, so cos.crate has to stay 20 for as long as any save exists.
    // The three accents are chosen against the five den pieces already in the
    // room rather than in isolation, because the den draws them side by side:
    // crate brown (not the copper amber, which would sit next to BEACON LAMP's
    // orange and read as the same object twice), rack silver (not the sage
    // 0x5AEB the first draft used - at 8 px on a 160x80 panel that reduced to
    // a grey smear beside the fern), dish pale steel.
    { "cos.crate",   "CABLE CRATE",    ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_PACKET_PUP,   0xA448 },
    { "cos.rack",    "PATCH RACK",     ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_GREMLIN,      0xC618 },
    { "cos.dish",    "ROOFTOP DISH",   ITEM_COSMETIC, COSMETIC_DEN,
      STAGE_SENTINEL,     0xAEDF }
};

static const uint8_t ITEM_DEF_COUNT =
    (uint8_t)(sizeof(ITEM_DEFS) / sizeof(ITEM_DEFS[0]));

// The table must fit the persisted array. ONE spare slot is left, down from
// four before the den's second set was appended. That is deliberate rather
// than comfortable: raising ITEM_TYPE_COUNT widens PetState::items[] on a
// board with no PSRAM, so it is a cost to pay when an item actually needs the
// room and not in advance. Appending a 24th item is still free of a schema
// bump, because the loader reads items[] by index with a zero default; a 25th
// means raising ITEM_TYPE_COUNT in src/config.h first.
static_assert(sizeof(ITEM_DEFS) / sizeof(ITEM_DEFS[0]) <= ITEM_TYPE_COUNT,
              "ITEM_DEFS has more rows than PetState::items[] can hold; "
              "raise ITEM_TYPE_COUNT in src/config.h and bump the schema");

static_assert(sizeof(ItemDef) == sizeof(ItemDefShapeProbe),
              "ItemDef gained a field. Items are cosmetic: they must carry no "
              "stat, XP, multiplier or duration. See src/content/item_defs.h.");

// ── Baseline recipes ──────────────────────────────────────────────────────
// Costs are chosen so the cheapest cosmetic is reachable from a handful of
// patrols and the most expensive is a project. Nothing here is a power curve,
// because none of the outputs do anything.

static const BaselineRecipe BASELINE_RECIPES[] PROGMEM = {
    { "craft.antenna", "cos.antenna",
      { "mat.copper",  "mat.scrap",   nullptr       }, { 3, 2, 0 },
      STAGE_PACKET_PUP },
    { "craft.scarf",   "cos.scarf",
      { "mat.signal",  "mat.scrap",   nullptr       }, { 4, 2, 0 },
      STAGE_PACKET_PUP },
    { "craft.lamp",    "cos.lamp",
      { "mat.scrap",   "mat.copper",  nullptr       }, { 4, 2, 0 },
      STAGE_PACKET_PUP },
    { "craft.rug",     "cos.rug",
      { "mat.scrap",   "mat.ferrite", nullptr       }, { 6, 2, 0 },
      STAGE_PACKET_PUP },
    { "craft.poster",  "cos.poster",
      { "mat.shard",   "mat.scrap",   nullptr       }, { 3, 3, 0 },
      STAGE_PACKET_PUP },

    { "craft.goggles", "cos.goggles",
      { "mat.lens",    "mat.circuit", "mat.copper"  }, { 2, 2, 1 },
      STAGE_BEACON_BEAST },
    { "craft.collar",  "cos.collar",
      { "mat.circuit", "mat.copper",  "mat.glitch"  }, { 3, 2, 1 },
      STAGE_BEACON_BEAST },
    { "craft.fern",    "cos.fern",
      { "mat.circuit", "mat.shard",   "mat.ferrite" }, { 2, 2, 2 },
      STAGE_BEACON_BEAST },
    { "craft.lantern", "cos.lantern",
      { "mat.signal",  "mat.lens",    "mat.circuit" }, { 5, 1, 2 },
      STAGE_BEACON_BEAST },

    { "craft.confetti","cos.confetti",
      { "mat.shard",   "mat.signal",  nullptr       }, { 4, 4, 0 },
      STAGE_GREMLIN },
    { "craft.sparks",  "cos.sparks",
      { "mat.copper",  "mat.ferrite", nullptr       }, { 4, 3, 0 },
      STAGE_GREMLIN },

    { "craft.aurora",  "cos.aurora",
      { "mat.glitch",  "mat.shard",   "mat.signal"  }, { 4, 4, 4 },
      STAGE_SENTINEL },

    // The den's second set, on the same curve as the first: a two-ingredient
    // piece early, three-ingredient projects later. The Sentinel's dish is the
    // most expensive thing in the room because it is the last piece of it.
    { "craft.crate",   "cos.crate",
      { "mat.scrap",   "mat.copper",  nullptr       }, { 5, 3, 0 },
      STAGE_PACKET_PUP },
    { "craft.rack",    "cos.rack",
      { "mat.circuit", "mat.copper",  "mat.ferrite" }, { 4, 4, 3 },
      STAGE_GREMLIN },
    { "craft.dish",    "cos.dish",
      { "mat.signal",  "mat.lens",    "mat.ferrite" }, { 6, 3, 4 },
      STAGE_SENTINEL }
};

static const uint8_t BASELINE_RECIPE_COUNT =
    (uint8_t)(sizeof(BASELINE_RECIPES) / sizeof(BASELINE_RECIPES[0]));

static_assert(sizeof(BASELINE_RECIPES) / sizeof(BASELINE_RECIPES[0])
                  <= RECIPE_MAX_DEFS,
              "BASELINE_RECIPES exceeds RECIPE_MAX_DEFS");

// ── Table lookups ─────────────────────────────────────────────────────────
// Flash-table scans. Both are cold paths (content load, and one call per drawn
// row); an index would cost RAM to save microseconds nobody can see. Being
// `inline` is what lets a translation unit that never calls them drop the
// tables entirely - see the include rule at the top of this file.

// Numeric id for a string id, or ITEM_ID_NONE. This is what lets the recipe
// validator REJECT a recipe naming an unknown item rather than silently
// resolving it to slot 0.
inline uint8_t itemIdFor(const char* id) {
    if (!id || !id[0]) return ITEM_ID_NONE;
    for (uint8_t i = 0; i < ITEM_DEF_COUNT; i++) {
        if (strcmp(ITEM_DEFS[i].id, id) == 0) return i;
    }
    return ITEM_ID_NONE;
}

// Definition for a numeric item id, or nullptr when out of range. Never indexes
// past the table, so a corrupt save cannot walk off it.
inline const ItemDef* itemDefAt(uint8_t itemId) {
    if (itemId >= ITEM_DEF_COUNT) return nullptr;
    return &ITEM_DEFS[itemId];
}

// An unknown id is neither, which is what makes both safe to ask about a value
// that came off a save file.
inline bool itemIsMaterial(uint8_t itemId) {
    const ItemDef* d = itemDefAt(itemId);
    return d && d->kind == ITEM_MATERIAL;
}

inline bool itemIsCosmetic(uint8_t itemId) {
    const ItemDef* d = itemDefAt(itemId);
    return d && d->kind == ITEM_COSMETIC;
}
