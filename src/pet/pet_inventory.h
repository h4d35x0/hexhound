#pragma once

#if defined(UNIT_TEST)
// Unit tests provide their own stubs
#include <cstdint>
#include <cstring>
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

#include "../config.h"
#include "../content/item_defs.h"
#include "../content/material_yield.h"

// First word of an item name: SCRAP, SIGNAL, CIRCUIT, DATA, GLITCH, COPPER,
// CRACKED, FERRITE, LAMP... All eight materials are distinct in their first
// word, which is what makes this safe where a footer has room for one word per
// item and not two. Shared so the Kit and the Den cannot drift apart on how
// they shorten a name.
inline void itemFirstWord(const char* name, char* out, size_t n) {
    if (!out || n == 0) return;
    size_t i = 0;
    if (name) {
        while (name[i] && name[i] != ' ' && i + 1 < n) { out[i] = name[i]; i++; }
    }
    out[i] = '\0';
}
#include "../content/content_pack.h"

// ── HexHound - Inventory and Crafting ────────────────────────────
//
// Owns the rules for what the pet is carrying and what it can make from it.
// The counts themselves are NOT owned here: they live in PetState::items[],
// because that is what gets persisted and there must be exactly one copy of a
// number. This engine is the only thing that writes that array.
//
// Shaped after QuestEngine: a singleton, fixed storage, no allocation, a
// baseline table that is always loaded first and an optional SPIFFS pack that
// may replace it only if it parses AND validates.
//
// ── Nothing here touches a stat ───────────────────────────────────────────
//
// There is no call to feedHunger(), changeMood(), changeEnergy() or addXP() in
// pet_inventory.cpp and there must never be one. Items are cosmetic; see the
// rule at the top of src/content/item_defs.h. `dirty` is the only PetState
// field this engine sets besides items[].
//
// ── Recipe validation is the load-bearing part ────────────────────────────
//
// A recipe is a set of string item ids. Those are resolved to numeric ids ONCE,
// at load, and a recipe that fails any check is dropped before it ever reaches
// the table. A bad content pack therefore degrades to the baseline instead of
// crashing, crafting nothing, or crafting the wrong thing. Rejected:
//
//   * no id, or an id longer than the field (truncation would alias two recipes)
//   * an output that is not a known item
//   * an output that is a MATERIAL (a recipe minting materials is a laundry,
//     and the point of the whole subsystem is cosmetics)
//   * zero inputs, or more than RECIPE_MAX_INPUTS
//   * an input that is not a known item
//   * an input quantity of 0, or above ITEM_STACK_MAX (unpayable by design)
//   * an input that is also the output (a recipe that eats what it makes)
//   * the same input item listed twice (ambiguous; merging it would be a guess)
//
// If a pack yields zero valid recipes the baseline table is left completely
// untouched, exactly as ContentStore does for dialogue and quests.

// Why a craft did not happen. The screen needs the reason, not just a false:
// "you are short two Copper Wire" and "that unlocks at Beacon Beast" are
// different sentences and a single boolean cannot tell them apart.
enum CraftResult : uint8_t {
    CRAFT_OK = 0,
    CRAFT_NO_SUCH_RECIPE,   // index out of range
    CRAFT_LOCKED,           // pet has not reached the recipe's stage
    CRAFT_SHORT,            // not enough materials
    CRAFT_ALREADY_OWNED     // cosmetics are one-of; you already have it
};

class Inventory {
public:
    static Inventory& instance();

    // Fills the recipe table. Always leaves a usable table behind, so this
    // cannot fail in a way a caller must handle; the return value reports only
    // whether a pack was picked up, for logging.
    bool begin();

    // ── Item table ────────────────────────────────────────────────────────
    // Compiled-in and read-only. See item_defs.h for why a pack may not
    // replace it: item ids are indices into a persisted array.

    uint8_t        itemCount() const;
    const ItemDef* definition(uint8_t itemId) const;
    // Numeric id for a string id, or ITEM_ID_NONE.
    uint8_t        idFor(const char* strId) const;

    // Apply a yield table from src/content/material_yield.h. Returns how many
    // entries actually landed; an entry naming an item this build does not know
    // is SKIPPED rather than trusted, because a content pack from a newer
    // firmware may reference one. Stack limits still apply per item, so a full
    // stack silently absorbs less than it was offered - which is the existing
    // contract of add().
    uint8_t        applyGrants(const struct MaterialGrant* grants, uint8_t n);
    // Display name, or "?" for an id this firmware does not know. Never null,
    // so a row can always be drawn even if a save carries a future item.
    const char*    nameFor(uint8_t itemId) const;

    // ── Counts ────────────────────────────────────────────────────────────
    // All backed by PetState::items[].

    uint16_t count(uint8_t itemId) const;
    bool     has(uint8_t itemId, uint16_t amount = 1) const;

    // How many of this item may be held. ITEM_STACK_MAX for a material; ONE for
    // a cosmetic, because owning nine Antenna Hats is not a thing anybody wants
    // and a cosmetic you already have should read as owned, not as a stack.
    static uint16_t stackMax(uint8_t itemId);

    // Adds up to `amount`, saturating at stackMax(). Returns how many were
    // actually added, which is how a caller reports "3 scrap (2 lost, full)"
    // instead of silently dropping them.
    uint16_t add(uint8_t itemId, uint16_t amount);

    // All-or-nothing. Returns false and changes nothing when the pet does not
    // have `amount`, so a partial spend is impossible; a craft that failed
    // halfway would eat materials and hand back nothing.
    bool remove(uint8_t itemId, uint16_t amount);

    // Zeroes every count. For tests and a future "start over" path only.
    void clearAll();

    // ── Empty is a state, not an edge case ────────────────────────────────
    // A new pet owns nothing. Callers ask these rather than deriving emptiness
    // from a loop, so the screen's empty state and the engine agree.

    bool    isEmpty() const;            // owns nothing at all
    uint8_t distinctMaterials() const;  // material types held, count > 0
    uint8_t ownedCosmetics() const;     // cosmetics crafted so far

    // ── Recipes ───────────────────────────────────────────────────────────

    uint8_t       recipeCount() const { return _recipeCount; }
    const Recipe* recipe(uint8_t index) const;
    // Index of a recipe by its string id, or -1.
    int           recipeIndexById(const char* id) const;
    bool          recipesFromPack() const { return _fromPack; }

    // True when the pet's stage is at or past the recipe's gate. Separate from
    // canCraft() because a locked recipe is still worth SHOWING - it is the
    // only way a player learns that a Beacon Lamp exists to work towards.
    bool isUnlocked(uint8_t index) const;

    // Why this recipe cannot be made right now, or CRAFT_OK.
    CraftResult check(uint8_t index) const;
    bool canCraft(uint8_t index) const { return check(index) == CRAFT_OK; }

    // Spends the inputs and grants the output. Returns CRAFT_OK on success; on
    // anything else NOTHING was consumed. Grants exactly one output, and never
    // touches a stat.
    CraftResult craft(uint8_t index);

    // ── Content loading ───────────────────────────────────────────────────

    // Replace the recipe table from the CBOR body of a pack whose signature
    // has ALREADY been verified. Returns false and leaves the current table
    // untouched when the body is not valid CBOR in the accepted profile or
    // contains no VALID recipe.
    //
    // Takes a body, not a file, so the only way to reach it is through
    // ContentPack::verifyImage(). There is no entry point that parses pack
    // structure without a signature behind it.
    bool applyRecipeCbor(const uint8_t* body, size_t len);

    // Replace the recipe table from a NUL-terminated JSON buffer.
    //
    // KEPT, and no longer reachable from the filesystem. This is the authoring
    // and test entry point; the signing tool consumes the same JSON on a
    // desktop. What changed in P3-W1 is that nothing on SPIFFS can reach it.
    bool applyRecipeJson(const char* json);

    // Why the last pack load did what it did, for diagnostics. PACK_ABSENT is
    // the normal answer on a device that has never been given a pack.
    ContentPackStatus packStatus() const { return _packStatus; }

    // Discards any pack and restores the compiled-in recipes.
    void resetToBaseline();

private:
    Inventory() = default;

    void loadBaseline();
    bool loadRecipePack();

    // Writes one validated recipe into slot `index`. Returns false without
    // touching the slot if any check fails; this is the single validation gate
    // shared by the baseline and the pack, so the two can never diverge.
    static bool buildRecipe(Recipe& out, const char* id, const char* outputId,
                            const char* const* inputIds, const uint16_t* qtys,
                            uint8_t inputCount, uint8_t minStage);

    // Pulls one JSON object into `out` through buildRecipe(). A member rather
    // than a file-static helper only so it can reach buildRecipe(); the two
    // passes in applyRecipeJson() must run identical validation or the count
    // and the write would disagree. Declared with a void* to keep ArduinoJson
    // out of this header - the .cpp casts it back to JsonVariantConst*.
    static bool parseRecipeRow(const void* rowVariant, Recipe& out);

    // CBOR twin of parseRecipeRow(), through the same buildRecipe() gate.
    // `reader` is a CborReader*; the void* keeps content_cbor.h out of this
    // header for the same reason the JSON one keeps ArduinoJson out.
    //
    // Sets `malformed` when the CBOR itself is broken, which condemns the whole
    // pack, as distinct from returning false for a well-formed row the engine
    // will not honour.
    static bool parseRecipeRowCbor(void* reader, Recipe& out, bool& malformed);

    Recipe  _recipes[RECIPE_MAX_DEFS];
    uint8_t _recipeCount = 0;
    bool    _fromPack    = false;
    ContentPackStatus _packStatus = PACK_ABSENT;
};
