// This translation unit is the only one that reads the item and recipe tables
// in item_defs.h. Everything else goes through Inventory's accessors, which is
// what keeps those tables (and their string pool) out of every other object
// file: unreferenced, they are dropped. See the include rule in item_defs.h.
#include "pet_inventory.h"
#include "pet_core.h"

#include <ArduinoJson.h>

#include "../content/content_cbor.h"
#include "../content/content_pack.h"

// ── HexHound - Inventory and Crafting Implementation ─────────────
//
// ── P3-W1: readRecipePack() is gone ───────────────────────────────────────
//
// This file used to carry a near-copy of the file reader in content_store.cpp.
// The duplication was accepted because two workstreams were running in
// parallel and neither owned the other's file. They are not running in
// parallel now, so both copies were deleted and replaced by one call to
// ContentPack::load(), which reads, bounds and signature-verifies every pack
// in the firmware.
//
// It unified cleanly, and the reason it did is worth recording: the two
// readers only ever differed in their size cap and their log prefix. The cap
// is now a parameter and the log line moved to the caller, so nothing was
// lost. Recipes still get 4 KB where dialogue gets 6 KB.

// ── Singleton ─────────────────────────────────────────────────────────────

Inventory& Inventory::instance() {
    static Inventory inv;
    return inv;
}

bool Inventory::begin() {
    loadBaseline();
    _fromPack = loadRecipePack();

    Serial.printf("[Items] %u items, %u recipes (%s)\n",
                  (unsigned)ITEM_DEF_COUNT, (unsigned)_recipeCount,
                  _fromPack ? "pack" : "baseline");
    return _fromPack;
}

// ── Item table ────────────────────────────────────────────────────────────

uint8_t Inventory::itemCount() const {
    return ITEM_DEF_COUNT;
}

const ItemDef* Inventory::definition(uint8_t itemId) const {
    return itemDefAt(itemId);
}

uint8_t Inventory::idFor(const char* strId) const {
    return itemIdFor(strId);
}

uint8_t Inventory::applyGrants(const MaterialGrant* grants, uint8_t n) {
    if (!grants) return 0;
    uint8_t landed = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!grants[i].item || grants[i].qty == 0) continue;
        const uint8_t id = idFor(grants[i].item);
        if (id == ITEM_ID_NONE) continue;
        if (add(id, grants[i].qty) > 0) landed++;
    }
    return landed;
}

const char* Inventory::nameFor(uint8_t itemId) const {
    const ItemDef* d = itemDefAt(itemId);
    // Never null: a save from a newer firmware may hold an item this build does
    // not know, and a row that cannot be drawn is worse than a row that admits
    // it does not recognise what it is holding.
    return d ? d->name : "?";
}

// ── Counts ────────────────────────────────────────────────────────────────

uint16_t Inventory::stackMax(uint8_t itemId) {
    const ItemDef* d = itemDefAt(itemId);
    if (!d) return 0;
    return (d->kind == ITEM_COSMETIC) ? 1 : (uint16_t)ITEM_STACK_MAX;
}

uint16_t Inventory::count(uint8_t itemId) const {
    if (itemId >= ITEM_TYPE_COUNT) return 0;
    return PetCore::instance().state().items[itemId];
}

bool Inventory::has(uint8_t itemId, uint16_t amount) const {
    return count(itemId) >= amount;
}

uint16_t Inventory::add(uint8_t itemId, uint16_t amount) {
    const uint16_t cap = stackMax(itemId);
    // cap == 0 means "this build has no such item". Refusing is the only safe
    // answer: writing into items[] past the known table would hand a future
    // firmware a count for an item the player never earned.
    if (cap == 0 || amount == 0 || itemId >= ITEM_TYPE_COUNT) return 0;

    PetState& st = PetCore::instance().state();
    const uint16_t before = st.items[itemId];
    if (before >= cap) return 0;

    // Widen before adding: `before + amount` in uint16 wraps at 65536 and a
    // saturating add that wraps first is not saturating at all.
    uint32_t next = (uint32_t)before + amount;
    if (next > cap) next = cap;

    st.items[itemId] = (uint16_t)next;
    st.dirty = true;
    return (uint16_t)(next - before);
}

bool Inventory::remove(uint8_t itemId, uint16_t amount) {
    if (itemId >= ITEM_TYPE_COUNT) return false;
    if (amount == 0) return true;

    PetState& st = PetCore::instance().state();
    if (st.items[itemId] < amount) return false;   // all-or-nothing

    st.items[itemId] = (uint16_t)(st.items[itemId] - amount);
    st.dirty = true;
    return true;
}

void Inventory::clearAll() {
    PetState& st = PetCore::instance().state();
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) st.items[i] = 0;
    st.dirty = true;
}

bool Inventory::isEmpty() const {
    const PetState& st = PetCore::instance().state();
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) {
        if (st.items[i] > 0) return false;
    }
    return true;
}

uint8_t Inventory::distinctMaterials() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < ITEM_DEF_COUNT; i++) {
        if (ITEM_DEFS[i].kind == ITEM_MATERIAL && count(i) > 0) n++;
    }
    return n;
}

uint8_t Inventory::ownedCosmetics() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < ITEM_DEF_COUNT; i++) {
        if (ITEM_DEFS[i].kind == ITEM_COSMETIC && count(i) > 0) n++;
    }
    return n;
}

// ── Recipes ───────────────────────────────────────────────────────────────

const Recipe* Inventory::recipe(uint8_t index) const {
    if (index >= _recipeCount) return nullptr;
    return &_recipes[index];
}

int Inventory::recipeIndexById(const char* id) const {
    if (!id) return -1;
    for (uint8_t i = 0; i < _recipeCount; i++) {
        if (strcmp(_recipes[i].id, id) == 0) return (int)i;
    }
    return -1;
}

bool Inventory::isUnlocked(uint8_t index) const {
    if (index >= _recipeCount) return false;
    const uint8_t gate = _recipes[index].minStage;
    if (gate == 0) return true;
    return (uint8_t)PetCore::instance().state().stage >= gate;
}

CraftResult Inventory::check(uint8_t index) const {
    if (index >= _recipeCount) return CRAFT_NO_SUCH_RECIPE;
    const Recipe& r = _recipes[index];

    if (!isUnlocked(index)) return CRAFT_LOCKED;

    // Owned before short: telling someone they are two Copper Wire short of a
    // hat they are already wearing is nonsense.
    if (count(r.output) >= stackMax(r.output)) return CRAFT_ALREADY_OWNED;

    for (uint8_t i = 0; i < r.inputCount; i++) {
        if (count(r.input[i]) < r.qty[i]) return CRAFT_SHORT;
    }
    return CRAFT_OK;
}

CraftResult Inventory::craft(uint8_t index) {
    const CraftResult res = check(index);
    if (res != CRAFT_OK) return res;

    // check() proved every input is affordable, so no remove() below can fail
    // and there is no half-spent state to unwind.
    const Recipe& r = _recipes[index];
    for (uint8_t i = 0; i < r.inputCount; i++) {
        remove(r.input[i], r.qty[i]);
    }
    add(r.output, 1);

    // Deliberately no addXP(), no changeMood(), no feedHunger(). Making a
    // cosmetic is its own reward; the moment crafting pays a stat the pet is a
    // build to optimise. See src/content/item_defs.h.
    return CRAFT_OK;
}

// ── Validation ────────────────────────────────────────────────────────────

bool Inventory::buildRecipe(Recipe& out, const char* id, const char* outputId,
                            const char* const* inputIds, const uint16_t* qtys,
                            uint8_t inputCount, uint8_t minStage) {
    if (!id || !id[0]) return false;
    // A truncated id would alias two recipes onto one name, and the reroll and
    // lookup paths both key on it.
    if (strlen(id) >= CONTENT_MAX_ID_LEN) return false;

    if (inputCount == 0 || inputCount > RECIPE_MAX_INPUTS) return false;

    const uint8_t output = itemIdFor(outputId);
    if (output == ITEM_ID_NONE) return false;              // unknown output
    if (!itemIsCosmetic(output)) return false;             // must be cosmetic

    uint8_t   resolved[RECIPE_MAX_INPUTS];
    uint16_t  amounts[RECIPE_MAX_INPUTS];

    for (uint8_t i = 0; i < inputCount; i++) {
        const uint8_t item = itemIdFor(inputIds[i]);
        if (item == ITEM_ID_NONE) return false;            // unknown input
        if (item == output) return false;                  // eats its own output
        if (qtys[i] == 0 || qtys[i] > ITEM_STACK_MAX) return false;

        for (uint8_t j = 0; j < i; j++) {
            if (resolved[j] == item) return false;         // duplicate input
        }
        resolved[i] = item;
        amounts[i]  = qtys[i];
    }

    // Only now is anything written, so a rejected recipe leaves the slot alone.
    out = Recipe();
    strlcpy(out.id, id, sizeof(out.id));
    out.output     = output;
    out.inputCount = inputCount;
    for (uint8_t i = 0; i < inputCount; i++) {
        out.input[i] = resolved[i];
        out.qty[i]   = amounts[i];
    }
    out.minStage = minStage;
    return true;
}

void Inventory::loadBaseline() {
    _recipeCount = 0;
    for (uint8_t i = 0; i < BASELINE_RECIPE_COUNT &&
                        _recipeCount < RECIPE_MAX_DEFS; i++) {
        const BaselineRecipe& row = BASELINE_RECIPES[i];

        uint8_t inputCount = 0;
        while (inputCount < RECIPE_MAX_INPUTS && row.input[inputCount]) {
            inputCount++;
        }

        // The baseline goes through exactly the same gate as a pack. A shipped
        // recipe with a typo in an item id is dropped here rather than shipped
        // broken, and the unit tests assert the whole baseline survives it.
        if (buildRecipe(_recipes[_recipeCount], row.id, row.output,
                        row.input, row.qty, inputCount, row.minStage)) {
            _recipeCount++;
        } else {
            Serial.printf("[Items] baseline recipe %s rejected\n", row.id);
        }
    }
}

void Inventory::resetToBaseline() {
    loadBaseline();
    _fromPack = false;
    _packStatus = PACK_ABSENT;
}

bool Inventory::loadRecipePack() {
    ContentPackFile pack;
    _packStatus = ContentPack::load(CONTENT_PATH_RECIPES, PACK_KIND_RECIPES,
                                    RECIPE_MAX_PACK_BYTES, pack);
    if (_packStatus != PACK_OK) return false;

    const bool ok = applyRecipeCbor(pack.body, pack.bodyLen);
    ContentPack::release(pack);
    if (!ok) {
        Serial.println("[Items] recipe pack verified but did not decode");
    }
    return ok;
}

// Pulls one JSON object into `out`, applying the SAME validation gate the
// baseline goes through. Returns false, and leaves `out` untouched, for any
// row the engine cannot honour exactly as written.
bool Inventory::parseRecipeRow(const void* rowVariant, Recipe& out) {
    JsonVariantConst row = *(const JsonVariantConst*)rowVariant;

    const char* id     = row["id"];
    const char* output = row["output"];

    const char* inputIds[RECIPE_MAX_INPUTS] = { nullptr, nullptr, nullptr };
    uint16_t    qtys[RECIPE_MAX_INPUTS]     = { 0, 0, 0 };
    uint8_t     inputCount = 0;
    bool        tooMany    = false;

    JsonArrayConst inputs = row["inputs"].as<JsonArrayConst>();
    if (!inputs.isNull()) {
        for (JsonVariantConst in : inputs) {
            if (inputCount >= RECIPE_MAX_INPUTS) {
                // More ingredients than the engine can hold. Rejecting the whole
                // row is deliberate: silently dropping the fourth would make the
                // recipe cheaper than the author wrote it.
                tooMany = true;
                break;
            }
            inputIds[inputCount] = in["item"];
            uint32_t q = in["qty"] | 1u;
            qtys[inputCount] = (q > 0xFFFFu) ? 0xFFFFu : (uint16_t)q;
            inputCount++;
        }
    }
    if (tooMany) return false;

    // PetStage is 1-based and reads naturally as a number in content, the same
    // convention the dialogue pack uses for its stage filter. Anything outside
    // the enum means "no gate" rather than an unreachable recipe.
    int stage = row["stage"] | 0;
    if (stage < STAGE_EGG || stage > STAGE_SENTINEL) stage = 0;

    return buildRecipe(out, id, output, inputIds, qtys, inputCount,
                       (uint8_t)stage);
}

bool Inventory::applyRecipeJson(const char* json) {
    if (!json) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[Items] recipe pack is not valid JSON");
        return false;
    }

    // Accept { "recipes": [...] } or a bare [...].
    JsonArrayConst rows = doc["recipes"].as<JsonArrayConst>();
    if (rows.isNull()) rows = doc.as<JsonArrayConst>();
    if (rows.isNull()) return false;

    // Two passes over the parsed document, exactly as ContentStore does: one to
    // count valid rows, one to write them. That is what lets a pack with zero
    // usable recipes leave the baseline table untouched, instead of clearing it
    // and then discovering there was nothing to put in it. One 40-byte scratch
    // Recipe, never a staging array, so this stays off the boot stack.
    Recipe  scratch;
    uint8_t valid = 0;
    for (JsonVariantConst row : rows) {
        if (valid >= RECIPE_MAX_DEFS) break;
        if (parseRecipeRow(&row, scratch)) {
            valid++;
        } else {
            const char* id = row["id"];
            Serial.printf("[Items] recipe %s rejected\n", id ? id : "(no id)");
        }
    }

    if (valid == 0) {
        Serial.println("[Items] recipe pack has no valid recipes");
        return false;
    }

    _recipeCount = 0;
    for (JsonVariantConst row : rows) {
        if (_recipeCount >= RECIPE_MAX_DEFS) break;
        if (parseRecipeRow(&row, _recipes[_recipeCount])) _recipeCount++;
    }

    return _recipeCount > 0;
}

// ── CBOR parser ───────────────────────────────────────────────────────────
//
// The signed path. Same buildRecipe() gate, same two passes, same rules. The
// difference is that pass one proves the ENTIRE document decodes before pass
// two writes a row, so a pack that turns to garbage halfway cannot leave the
// recipe table half replaced.

// Longest key or enum name in a recipe row, with headroom.
#define RECIPE_CBOR_NAME_MAX 24

bool Inventory::parseRecipeRowCbor(void* reader, Recipe& out, bool& malformed) {
    CborReader& r = *(CborReader*)reader;
    malformed = false;

    char id[CONTENT_MAX_ID_LEN]     = {};
    char output[CONTENT_MAX_ID_LEN] = {};

    // Storage for the ingredient ids has to outlive the loop, because
    // buildRecipe() takes an array of pointers into it.
    char     inputStore[RECIPE_MAX_INPUTS][CONTENT_MAX_ID_LEN] = {};
    const char* inputIds[RECIPE_MAX_INPUTS] = { nullptr, nullptr, nullptr };
    uint16_t qtys[RECIPE_MAX_INPUTS]        = { 0, 0, 0 };
    uint8_t  inputCount = 0;
    bool     tooMany    = false;
    uint32_t stage      = 0;

    uint32_t pairs = 0;
    if (!Cbor::readMap(r, pairs)) { malformed = true; return false; }

    for (uint32_t i = 0; i < pairs; i++) {
        char key[RECIPE_CBOR_NAME_MAX];
        if (!Cbor::readText(r, key, sizeof(key))) { malformed = true; return false; }

        if (strcmp(key, "id") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                if (!Cbor::skipValue(r)) { malformed = true; return false; }
            } else if (!Cbor::readText(r, id, sizeof(id))) {
                malformed = true;
                return false;
            }
        } else if (strcmp(key, "output") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                if (!Cbor::skipValue(r)) { malformed = true; return false; }
            } else if (!Cbor::readText(r, output, sizeof(output))) {
                malformed = true;
                return false;
            }
        } else if (strcmp(key, "stage") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_UINT) {
                if (!Cbor::skipValue(r)) { malformed = true; return false; }
            } else if (!Cbor::readUint(r, stage)) {
                malformed = true;
                return false;
            }
        } else if (strcmp(key, "inputs") == 0) {
            uint32_t n = 0;
            if (!Cbor::readArray(r, n)) { malformed = true; return false; }
            for (uint32_t k = 0; k < n; k++) {
                uint32_t innerPairs = 0;
                if (!Cbor::readMap(r, innerPairs)) { malformed = true; return false; }

                char     itemId[CONTENT_MAX_ID_LEN] = {};
                uint32_t qty = 1;
                for (uint32_t m = 0; m < innerPairs; m++) {
                    char innerKey[RECIPE_CBOR_NAME_MAX];
                    if (!Cbor::readText(r, innerKey, sizeof(innerKey))) {
                        malformed = true;
                        return false;
                    }
                    if (strcmp(innerKey, "item") == 0) {
                        if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                            if (!Cbor::skipValue(r)) { malformed = true; return false; }
                        } else if (!Cbor::readText(r, itemId, sizeof(itemId))) {
                            malformed = true;
                            return false;
                        }
                    } else if (strcmp(innerKey, "qty") == 0) {
                        if (Cbor::peekType(r) != CBOR_TYPE_UINT) {
                            if (!Cbor::skipValue(r)) { malformed = true; return false; }
                        } else if (!Cbor::readUint(r, qty)) {
                            malformed = true;
                            return false;
                        }
                    } else if (!Cbor::skipValue(r)) {
                        malformed = true;
                        return false;
                    }
                }

                // More ingredients than the engine can hold. The row is
                // rejected WHOLE rather than silently dropping the fourth,
                // which would make the recipe cheaper than the author wrote
                // it. The rest of the array still has to be consumed so the
                // reader stays aligned for the next row.
                if (inputCount >= RECIPE_MAX_INPUTS) {
                    tooMany = true;
                    continue;
                }
                strlcpy(inputStore[inputCount], itemId, CONTENT_MAX_ID_LEN);
                inputIds[inputCount] = inputStore[inputCount];
                qtys[inputCount]     = (qty > 0xFFFFu) ? 0xFFFFu : (uint16_t)qty;
                inputCount++;
            }
        } else if (!Cbor::skipValue(r)) {
            malformed = true;
            return false;
        }

        if (Cbor::failed(r)) { malformed = true; return false; }
    }

    if (tooMany) return false;

    // PetStage is 1-based and reads naturally as a number in content, the same
    // convention the dialogue pack uses. Anything outside the enum means "no
    // gate" rather than an unreachable recipe.
    if (stage < (uint32_t)STAGE_EGG || stage > (uint32_t)STAGE_SENTINEL) stage = 0;

    return buildRecipe(out, id[0] ? id : nullptr, output[0] ? output : nullptr,
                       inputIds, qtys, inputCount, (uint8_t)stage);
}

bool Inventory::applyRecipeCbor(const uint8_t* body, size_t len) {
    CborReader r;
    uint32_t rows = 0;

    // Wrapper: a map with exactly one key, "recipes", holding the row array.
    Cbor::init(r, body, len);
    uint32_t pairs = 0;
    char key[RECIPE_CBOR_NAME_MAX];
    if (!Cbor::readMap(r, pairs) || pairs != 1 ||
        !Cbor::readText(r, key, sizeof(key)) || strcmp(key, "recipes") != 0 ||
        !Cbor::readArray(r, rows)) {
        Serial.println("[Items] recipe pack is not valid CBOR");
        return false;
    }

    Recipe  scratch;
    uint8_t valid = 0;
    for (uint32_t i = 0; i < rows; i++) {
        bool malformed = false;
        const bool ok = parseRecipeRowCbor(&r, scratch, malformed);
        if (malformed) {
            Serial.println("[Items] recipe pack is not valid CBOR");
            return false;
        }
        if (ok && valid < RECIPE_MAX_DEFS) valid++;
    }
    if (!Cbor::finish(r)) {
        Serial.println("[Items] recipe pack has trailing bytes");
        return false;
    }
    if (valid == 0) {
        Serial.println("[Items] recipe pack has no valid recipes");
        return false;
    }

    Cbor::init(r, body, len);
    if (!Cbor::readMap(r, pairs) || !Cbor::readText(r, key, sizeof(key)) ||
        !Cbor::readArray(r, rows)) {
        return false;
    }

    _recipeCount = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (_recipeCount >= RECIPE_MAX_DEFS) break;
        bool malformed = false;
        if (parseRecipeRowCbor(&r, _recipes[_recipeCount], malformed)) {
            _recipeCount++;
        }
        if (malformed) {
            // Cannot happen: pass one already decoded these exact bytes. The
            // baseline is restored anyway, because a device with an empty
            // recipe table is not something to leave to an argument.
            loadBaseline();
            return false;
        }
    }

    if (_recipeCount == 0) {
        loadBaseline();
        return false;
    }
    return true;
}
