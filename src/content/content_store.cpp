#include "content_store.h"
#include "content_baseline.h"
#include "content_cbor.h"
#include "content_pack.h"

#include <ArduinoJson.h>

// ── HexHound - Content Store Implementation ──────────────────────
//
// There is no file reader in this file any more. Reading, bounding and
// verifying a pack is ContentPack::load()'s job, and it is the same code the
// recipe pack goes through. What is left here is what was always specific to
// dialogue and quests: turning a validated document into table rows.

// Names accepted in the JSON for each enum. Content authored by hand with bare
// numbers is a bug farm, so the loader takes either: a name from these tables,
// or the raw ordinal for a generator that has the enum already. The tables are
// also the authoritative spelling of the format, which is why they live next to
// the parser rather than in a document that can drift from it.
static const char* const DIALOGUE_CONTEXT_NAMES[DLG_CONTEXT_COUNT] = {
    "greeting", "idle", "patrol_done", "threat_found", "quest_done",
    "game_win", "game_lose", "return_after_absence", "hibernate_wake", "evolved"
};

static const char* const QUEST_KIND_NAMES[QUEST_KIND_COUNT] = {
    "care", "explore", "cyber", "life"
};

static const char* const TRAIT_NAMES[TRAIT_COUNT] = {
    "curious", "protective", "chaotic", "sleepy", "greedy", "brave"
};

// Behavioural forms, in PetForm order INCLUDING the leading FORM_UNSET, so a
// name lookup returns the enum value directly. "unset" is spellable but means
// the same as omitting the field: see the fallback in the parser below.
static const char* const FORM_NAMES[FORM_COUNT] = {
    "unset", "pathfinder", "guardian", "archivist", "cipher", "gremlin",
    "packmaster"
};

static const char* const MEMORY_SLOT_NAMES[MEM_SLOT_COUNT] = {
    "visits", "patrols", "new_networks", "threats", "quests_done",
    "days_away", "best_score"
};

// Capability names, in Capability bit order. Index i is bit (1 << i).
static const char* const CAPABILITY_NAMES[] = {
    "imu", "touch", "battery", "sd", "usb_hid", "psram", "round_panel"
};
static const uint8_t CAPABILITY_NAME_COUNT =
    (uint8_t)(sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]));

static uint8_t lookupName(const char* const* names, uint8_t count,
                          const char* value, uint8_t fallback) {
    if (!value) return fallback;
    for (uint8_t i = 0; i < count; i++) {
        if (strcmp(names[i], value) == 0) return i;
    }
    return fallback;
}

// Read an enum field that may be spelled as a name or an ordinal. Out-of-range
// ordinals fall back rather than indexing past a table, because a bad content
// file must degrade, never corrupt.
static uint8_t enumField(JsonVariantConst v, const char* const* names,
                         uint8_t count, uint8_t fallback) {
    if (v.isNull()) return fallback;
    if (v.is<const char*>()) {
        return lookupName(names, count, v.as<const char*>(), fallback);
    }
    if (v.is<unsigned int>() || v.is<int>()) {
        int raw = v.as<int>();
        if (raw >= 0 && raw < (int)count) return (uint8_t)raw;
    }
    return fallback;
}

// Capability gate: an array of names, a single name, or a raw bitmask.
static uint16_t capsField(JsonVariantConst v) {
    if (v.isNull()) return 0;

    if (v.is<JsonArrayConst>()) {
        uint16_t mask = 0;
        for (JsonVariantConst entry : v.as<JsonArrayConst>()) {
            uint8_t bit = lookupName(CAPABILITY_NAMES, CAPABILITY_NAME_COUNT,
                                     entry.as<const char*>(), 0xFF);
            if (bit != 0xFF) mask |= (uint16_t)(1u << bit);
        }
        return mask;
    }

    if (v.is<const char*>()) {
        uint8_t bit = lookupName(CAPABILITY_NAMES, CAPABILITY_NAME_COUNT,
                                 v.as<const char*>(), 0xFF);
        return bit == 0xFF ? 0 : (uint16_t)(1u << bit);
    }

    return (uint16_t)(v.as<unsigned int>());
}

// ── CBOR field readers ────────────────────────────────────────────────────
//
// The CBOR twins of enumField() and capsField() above. They deliberately share
// lookupName() and the SAME name tables, and they fall back the SAME way,
// because the moment the two formats resolve a name differently, a pack that
// was reviewed as JSON stops meaning what it said.

// Longest enum name in any table above, plus room for a typo to be recognised
// as a typo rather than silently truncated into a different name.
#define CONTENT_ENUM_NAME_MAX 24

// Accepts a name or a bare ordinal, exactly like enumField(). An out-of-range
// ordinal, a null, or a value of an unexpected type all fall back rather than
// indexing past a table.
static uint8_t cborEnumField(CborReader& r, const char* const* names,
                             uint8_t count, uint8_t fallback) {
    switch (Cbor::peekType(r)) {
        case CBOR_TYPE_TEXT: {
            char buf[CONTENT_ENUM_NAME_MAX];
            if (!Cbor::readText(r, buf, sizeof(buf))) return fallback;
            return lookupName(names, count, buf, fallback);
        }
        case CBOR_TYPE_UINT: {
            uint32_t raw = 0;
            if (!Cbor::readUint(r, raw)) return fallback;
            return (raw < (uint32_t)count) ? (uint8_t)raw : fallback;
        }
        default:
            // Wrong type for this field. Skipping keeps the ROW usable, which
            // is what `row["x"] | fallback` does on the JSON path. A shape the
            // decoder cannot even skip fails the whole pack, inside skipValue.
            Cbor::skipValue(r);
            return fallback;
    }
}

// Capability gate: an array of names, a single name, or a raw bitmask. Same
// three spellings capsField() accepts.
static uint16_t cborCapsField(CborReader& r) {
    switch (Cbor::peekType(r)) {
        case CBOR_TYPE_ARRAY: {
            uint32_t n = 0;
            if (!Cbor::readArray(r, n)) return 0;
            uint16_t mask = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                    if (!Cbor::skipValue(r)) return 0;
                    continue;
                }
                char buf[CONTENT_ENUM_NAME_MAX];
                if (!Cbor::readText(r, buf, sizeof(buf))) return 0;
                const uint8_t bit = lookupName(CAPABILITY_NAMES,
                                               CAPABILITY_NAME_COUNT, buf, 0xFF);
                if (bit != 0xFF) mask |= (uint16_t)(1u << bit);
            }
            return mask;
        }
        case CBOR_TYPE_TEXT: {
            char buf[CONTENT_ENUM_NAME_MAX];
            if (!Cbor::readText(r, buf, sizeof(buf))) return 0;
            const uint8_t bit = lookupName(CAPABILITY_NAMES,
                                           CAPABILITY_NAME_COUNT, buf, 0xFF);
            return (bit == 0xFF) ? 0 : (uint16_t)(1u << bit);
        }
        case CBOR_TYPE_UINT: {
            uint32_t raw = 0;
            if (!Cbor::readUint(r, raw)) return 0;
            return (uint16_t)raw;
        }
        default:
            Cbor::skipValue(r);
            return 0;
    }
}

// Reads a uint field with a floor, a ceiling and a default, the CBOR twin of
// `row["target"] | 1u` plus its clamps.
static uint32_t cborUintField(CborReader& r, uint32_t fallback) {
    if (Cbor::peekType(r) != CBOR_TYPE_UINT) {
        Cbor::skipValue(r);
        return fallback;
    }
    uint32_t v = 0;
    if (!Cbor::readUint(r, v)) return fallback;
    return v;
}

// Opens the one-key wrapper map and positions the reader on the row array.
// Exactly one top-level key is allowed: the signing tool emits exactly one,
// and "some other keys were also in there" is not something a loader should
// be deciding what to do with.
static bool cborOpenRows(CborReader& r, const uint8_t* body, size_t len,
                         const char* wantKey, uint32_t& rowCount) {
    rowCount = 0;
    Cbor::init(r, body, len);

    uint32_t pairs = 0;
    if (!Cbor::readMap(r, pairs) || pairs != 1) return false;

    char key[CONTENT_ENUM_NAME_MAX];
    if (!Cbor::readText(r, key, sizeof(key))) return false;
    if (strcmp(key, wantKey) != 0) return false;

    return Cbor::readArray(r, rowCount);
}

// ── Store ─────────────────────────────────────────────────────────────────

ContentStore& ContentStore::instance() {
    static ContentStore store;
    return store;
}

bool ContentStore::begin() {
    loadBaseline();

    // Each table is replaced independently. A device may ship a new quest pack
    // without a dialogue pack, and a broken quest pack must not cost it the
    // dialogue it already had.
    //
    // The baseline is loaded FIRST and unconditionally, so every rejection
    // path below is a no-op on already-good tables rather than a recovery.
    // There is no ordering of these calls that can leave the pet mute.
    _dialogueFromPack = loadDialoguePack();
    _questsFromPack   = loadQuestPack();

    Serial.printf("[Content] %u lines (%s: %s), %u quests (%s: %s)\n",
                  (unsigned)_dialogueCount,
                  _dialogueFromPack ? "pack" : "baseline",
                  ContentPack::statusName(_dialogueStatus),
                  (unsigned)_questDefCount,
                  _questsFromPack ? "pack" : "baseline",
                  ContentPack::statusName(_questStatus));

    return _dialogueFromPack || _questsFromPack;
}

void ContentStore::loadBaseline() {
    _dialogueCount = 0;
    for (uint8_t i = 0; i < BASELINE_DIALOGUE_COUNT &&
                        _dialogueCount < CONTENT_MAX_DIALOGUE_LINES; i++) {
        const BaselineDialogue& row = BASELINE_DIALOGUE[i];
        DialogueLine& line = _dialogue[_dialogueCount];
        strlcpy(line.text, row.text, sizeof(line.text));
        line.context    = (DialogueContext)row.context;
        line.trait      = row.trait;
        line.stage      = row.stage;
        line.form       = row.form;
        line.memorySlot = row.memorySlot;
        _dialogueCount++;
    }

    _questDefCount = 0;
    for (uint8_t i = 0; i < BASELINE_QUEST_COUNT &&
                        _questDefCount < CONTENT_MAX_QUEST_DEFS; i++) {
        const BaselineQuest& row = BASELINE_QUESTS[i];
        QuestDef& def = _questDefs[_questDefCount];
        strlcpy(def.id, row.id, sizeof(def.id));
        strlcpy(def.text, row.text, sizeof(def.text));
        def.kind         = (QuestKind)row.kind;
        def.target       = row.target;
        def.rewardXP     = row.rewardXP;
        def.rewardBond   = row.rewardBond;
        def.requiresCaps = row.requiresCaps;
        _questDefCount++;
    }
}

void ContentStore::resetToBaseline() {
    loadBaseline();
    _dialogueFromPack = false;
    _questsFromPack = false;
    _dialogueStatus = PACK_ABSENT;
    _questStatus    = PACK_ABSENT;
}

const DialogueLine* ContentStore::dialogue(uint8_t index) const {
    if (index >= _dialogueCount) return nullptr;
    return &_dialogue[index];
}

const QuestDef* ContentStore::questDef(uint8_t index) const {
    if (index >= _questDefCount) return nullptr;
    return &_questDefs[index];
}

const QuestDef* ContentStore::questDefById(const char* id) const {
    if (!id) return nullptr;
    for (uint8_t i = 0; i < _questDefCount; i++) {
        if (strcmp(_questDefs[i].id, id) == 0) return &_questDefs[i];
    }
    return nullptr;
}

// Both loaders are the same four steps: read and verify, decode, release,
// report. The verify happens entirely inside ContentPack::load(), so the CBOR
// decoder below is never handed bytes that failed a signature check.

bool ContentStore::loadDialoguePack() {
    ContentPackFile pack;
    _dialogueStatus = ContentPack::load(CONTENT_PATH_DIALOGUE,
                                        PACK_KIND_DIALOGUE,
                                        CONTENT_MAX_PACK_BYTES, pack);
    if (_dialogueStatus != PACK_OK) return false;

    const bool ok = applyDialogueCbor(pack.body, pack.bodyLen);
    ContentPack::release(pack);
    if (!ok) {
        Serial.println("[Content] dialogue pack verified but did not decode");
    }
    return ok;
}

bool ContentStore::loadQuestPack() {
    ContentPackFile pack;
    _questStatus = ContentPack::load(CONTENT_PATH_QUESTS,
                                     PACK_KIND_QUESTS,
                                     CONTENT_MAX_PACK_BYTES, pack);
    if (_questStatus != PACK_OK) return false;

    const bool ok = applyQuestCbor(pack.body, pack.bodyLen);
    ContentPack::release(pack);
    if (!ok) {
        Serial.println("[Content] quest pack verified but did not decode");
    }
    return ok;
}

// Both parsers make two passes over the parsed document: one to count valid
// rows, one to write them. That is what lets a pack with zero usable rows leave
// the baseline untouched, instead of clearing the table and then discovering
// there was nothing to put in it.

bool ContentStore::applyDialogueJson(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[Content] dialogue pack is not valid JSON");
        return false;
    }

    // Accept { "lines": [...] } or a bare [...].
    JsonArrayConst rows = doc["lines"].as<JsonArrayConst>();
    if (rows.isNull()) rows = doc.as<JsonArrayConst>();
    if (rows.isNull()) return false;

    uint8_t valid = 0;
    for (JsonVariantConst row : rows) {
        const char* text = row["text"];
        if (text && text[0]) valid++;
    }
    if (valid == 0) {
        Serial.println("[Content] dialogue pack has no usable lines");
        return false;
    }

    _dialogueCount = 0;
    for (JsonVariantConst row : rows) {
        if (_dialogueCount >= CONTENT_MAX_DIALOGUE_LINES) break;

        const char* text = row["text"];
        if (!text || !text[0]) continue;

        DialogueLine& line = _dialogue[_dialogueCount];
        strlcpy(line.text, text, sizeof(line.text));
        line.context = (DialogueContext)enumField(row["context"],
                                                  DIALOGUE_CONTEXT_NAMES,
                                                  DLG_CONTEXT_COUNT, DLG_IDLE);
        line.trait      = enumField(row["trait"], TRAIT_NAMES,
                                    TRAIT_COUNT, 0xFF);
        line.memorySlot = enumField(row["memory"], MEMORY_SLOT_NAMES,
                                    MEM_SLOT_COUNT, DIALOGUE_MEMORY_NONE);

        // Form is the third filter. An absent, misspelled or explicitly "unset"
        // form all collapse to 0xFF, "suits any pet". Tagging a line for
        // FORM_UNSET would mean "only for a pet with no identity yet", which no
        // writer means and which would make a typo silently narrow a line down
        // to the pets least likely to be looked at.
        uint8_t formTag = enumField(row["form"], FORM_NAMES, FORM_COUNT, 0xFF);
        line.form = (formTag == FORM_UNSET) ? 0xFF : formTag;

        // Stage has no name table: PetStage is 1-based and reads naturally as a
        // number in content. Anything outside the enum becomes "any".
        int stage = row["stage"] | -1;
        line.stage = (stage >= STAGE_EGG && stage <= STAGE_SENTINEL)
                         ? (uint8_t)stage : 0xFF;

        _dialogueCount++;
    }

    return _dialogueCount > 0;
}

bool ContentStore::applyQuestJson(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        Serial.println("[Content] quest pack is not valid JSON");
        return false;
    }

    JsonArrayConst rows = doc["quests"].as<JsonArrayConst>();
    if (rows.isNull()) rows = doc.as<JsonArrayConst>();
    if (rows.isNull()) return false;

    uint8_t valid = 0;
    for (JsonVariantConst row : rows) {
        const char* id = row["id"];
        const char* text = row["text"];
        if (id && id[0] && text && text[0]) valid++;
    }
    if (valid == 0) {
        Serial.println("[Content] quest pack has no usable quests");
        return false;
    }

    _questDefCount = 0;
    for (JsonVariantConst row : rows) {
        if (_questDefCount >= CONTENT_MAX_QUEST_DEFS) break;

        const char* id = row["id"];
        const char* text = row["text"];
        if (!id || !id[0] || !text || !text[0]) continue;

        QuestDef& def = _questDefs[_questDefCount];
        strlcpy(def.id, id, sizeof(def.id));
        strlcpy(def.text, text, sizeof(def.text));
        def.kind = (QuestKind)enumField(row["kind"], QUEST_KIND_NAMES,
                                        QUEST_KIND_COUNT, QUEST_CARE);

        // A target of zero would complete the instant it is offered, so the
        // floor is one. The ceiling keeps a typo from asking for a lifetime.
        uint32_t target = row["target"] | 1u;
        if (target < 1) target = 1;
        if (target > 9999) target = 9999;
        def.target = (uint16_t)target;

        def.rewardXP     = (uint16_t)(row["xp"] | 0u);
        def.rewardBond   = (uint16_t)(row["bond"] | 0u);
        def.requiresCaps = capsField(row["requires"]);

        _questDefCount++;
    }

    return _questDefCount > 0;
}

// ── CBOR parsers ──────────────────────────────────────────────────────────
//
// The signed path. Same two passes as the JSON parsers above, same validity
// rules, same name tables, same fallbacks. The difference is that pass one
// here also proves the ENTIRE document decodes before pass two writes a single
// row, so a pack that turns to garbage halfway through cannot leave a table
// half replaced.
//
// Both parsers restore the baseline if the second pass ever disagrees with the
// first. That cannot happen, because the two passes are the same code over the
// same immutable bytes. It is written down anyway: "cannot happen" is not a
// property this device gets to rely on when the alternative is a pet with
// nothing to say.

struct DialogueScratch {
    char    text[CONTENT_MAX_TEXT_LEN];
    uint8_t context;
    uint8_t trait;
    uint8_t stage;
    uint8_t form;
    uint8_t memorySlot;
};

// Returns false only when the CBOR is malformed, which condemns the whole
// pack. A well-formed row that is unusable comes back with an empty text,
// which is the same "skip this row" signal the JSON pass uses.
static bool readDialogueRow(CborReader& r, DialogueScratch& out) {
    out.text[0]    = '\0';
    out.context    = DLG_IDLE;
    out.trait      = 0xFF;
    out.stage      = 0xFF;
    out.form       = 0xFF;
    out.memorySlot = DIALOGUE_MEMORY_NONE;

    uint32_t pairs = 0;
    if (!Cbor::readMap(r, pairs)) return false;

    for (uint32_t i = 0; i < pairs; i++) {
        char key[CONTENT_ENUM_NAME_MAX];
        if (!Cbor::readText(r, key, sizeof(key))) return false;

        if (strcmp(key, "text") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                if (!Cbor::skipValue(r)) return false;
            } else if (!Cbor::readText(r, out.text, sizeof(out.text))) {
                return false;
            }
        } else if (strcmp(key, "context") == 0) {
            out.context = cborEnumField(r, DIALOGUE_CONTEXT_NAMES,
                                        DLG_CONTEXT_COUNT, DLG_IDLE);
        } else if (strcmp(key, "trait") == 0) {
            out.trait = cborEnumField(r, TRAIT_NAMES, TRAIT_COUNT, 0xFF);
        } else if (strcmp(key, "memory") == 0) {
            out.memorySlot = cborEnumField(r, MEMORY_SLOT_NAMES,
                                           MEM_SLOT_COUNT, DIALOGUE_MEMORY_NONE);
        } else if (strcmp(key, "form") == 0) {
            // Same collapse the JSON path applies: absent, misspelled or
            // explicitly "unset" all become "suits any pet".
            const uint8_t formTag = cborEnumField(r, FORM_NAMES, FORM_COUNT, 0xFF);
            out.form = (formTag == FORM_UNSET) ? 0xFF : formTag;
        } else if (strcmp(key, "stage") == 0) {
            const uint32_t stage = cborUintField(r, 0xFFFFFFFFu);
            out.stage = (stage >= (uint32_t)STAGE_EGG &&
                         stage <= (uint32_t)STAGE_SENTINEL) ? (uint8_t)stage : 0xFF;
        } else {
            // Unknown field, ignored exactly as the JSON parser ignores it, so
            // a pack authored against a newer firmware still loads on this one.
            if (!Cbor::skipValue(r)) return false;
        }

        if (Cbor::failed(r)) return false;
    }
    return true;
}

bool ContentStore::applyDialogueCbor(const uint8_t* body, size_t len) {
    CborReader r;
    uint32_t rows = 0;
    if (!cborOpenRows(r, body, len, "lines", rows)) {
        Serial.println("[Content] dialogue pack is not valid CBOR");
        return false;
    }

    DialogueScratch scratch;
    uint8_t valid = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (!readDialogueRow(r, scratch)) {
            Serial.println("[Content] dialogue pack is not valid CBOR");
            return false;
        }
        if (scratch.text[0] && valid < CONTENT_MAX_DIALOGUE_LINES) valid++;
    }
    if (!Cbor::finish(r)) {
        Serial.println("[Content] dialogue pack has trailing bytes");
        return false;
    }
    if (valid == 0) {
        Serial.println("[Content] dialogue pack has no usable lines");
        return false;
    }

    if (!cborOpenRows(r, body, len, "lines", rows)) return false;
    _dialogueCount = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (_dialogueCount >= CONTENT_MAX_DIALOGUE_LINES) break;
        if (!readDialogueRow(r, scratch)) {
            loadBaseline();
            return false;
        }
        if (!scratch.text[0]) continue;

        DialogueLine& line = _dialogue[_dialogueCount];
        strlcpy(line.text, scratch.text, sizeof(line.text));
        line.context    = (DialogueContext)scratch.context;
        line.trait      = scratch.trait;
        line.stage      = scratch.stage;
        line.form       = scratch.form;
        line.memorySlot = scratch.memorySlot;
        _dialogueCount++;
    }

    if (_dialogueCount == 0) {
        loadBaseline();
        return false;
    }
    return true;
}

struct QuestScratch {
    char     id[CONTENT_MAX_ID_LEN];
    char     text[CONTENT_MAX_TEXT_LEN];
    uint8_t  kind;
    uint16_t target;
    uint16_t rewardXP;
    uint16_t rewardBond;
    uint16_t requiresCaps;
};

static bool readQuestRow(CborReader& r, QuestScratch& out) {
    out.id[0]        = '\0';
    out.text[0]      = '\0';
    out.kind         = QUEST_CARE;
    out.target       = 1;
    out.rewardXP     = 0;
    out.rewardBond   = 0;
    out.requiresCaps = 0;

    uint32_t pairs = 0;
    if (!Cbor::readMap(r, pairs)) return false;

    for (uint32_t i = 0; i < pairs; i++) {
        char key[CONTENT_ENUM_NAME_MAX];
        if (!Cbor::readText(r, key, sizeof(key))) return false;

        if (strcmp(key, "id") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                if (!Cbor::skipValue(r)) return false;
            } else if (!Cbor::readText(r, out.id, sizeof(out.id))) {
                return false;
            }
        } else if (strcmp(key, "text") == 0) {
            if (Cbor::peekType(r) != CBOR_TYPE_TEXT) {
                if (!Cbor::skipValue(r)) return false;
            } else if (!Cbor::readText(r, out.text, sizeof(out.text))) {
                return false;
            }
        } else if (strcmp(key, "kind") == 0) {
            out.kind = cborEnumField(r, QUEST_KIND_NAMES, QUEST_KIND_COUNT,
                                     QUEST_CARE);
        } else if (strcmp(key, "target") == 0) {
            // A target of zero would complete the instant it is offered, so
            // the floor is one; the ceiling keeps a typo from asking for a
            // lifetime. Same bounds as the JSON path.
            uint32_t target = cborUintField(r, 1u);
            if (target < 1)    target = 1;
            if (target > 9999) target = 9999;
            out.target = (uint16_t)target;
        } else if (strcmp(key, "xp") == 0) {
            const uint32_t xp = cborUintField(r, 0u);
            out.rewardXP = (xp > 0xFFFFu) ? 0xFFFFu : (uint16_t)xp;
        } else if (strcmp(key, "bond") == 0) {
            const uint32_t bond = cborUintField(r, 0u);
            out.rewardBond = (bond > 0xFFFFu) ? 0xFFFFu : (uint16_t)bond;
        } else if (strcmp(key, "requires") == 0) {
            out.requiresCaps = cborCapsField(r);
        } else {
            if (!Cbor::skipValue(r)) return false;
        }

        if (Cbor::failed(r)) return false;
    }
    return true;
}

bool ContentStore::applyQuestCbor(const uint8_t* body, size_t len) {
    CborReader r;
    uint32_t rows = 0;
    if (!cborOpenRows(r, body, len, "quests", rows)) {
        Serial.println("[Content] quest pack is not valid CBOR");
        return false;
    }

    QuestScratch scratch;
    uint8_t valid = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (!readQuestRow(r, scratch)) {
            Serial.println("[Content] quest pack is not valid CBOR");
            return false;
        }
        if (scratch.id[0] && scratch.text[0] && valid < CONTENT_MAX_QUEST_DEFS) {
            valid++;
        }
    }
    if (!Cbor::finish(r)) {
        Serial.println("[Content] quest pack has trailing bytes");
        return false;
    }
    if (valid == 0) {
        Serial.println("[Content] quest pack has no usable quests");
        return false;
    }

    if (!cborOpenRows(r, body, len, "quests", rows)) return false;
    _questDefCount = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (_questDefCount >= CONTENT_MAX_QUEST_DEFS) break;
        if (!readQuestRow(r, scratch)) {
            loadBaseline();
            return false;
        }
        if (!scratch.id[0] || !scratch.text[0]) continue;

        QuestDef& def = _questDefs[_questDefCount];
        strlcpy(def.id, scratch.id, sizeof(def.id));
        strlcpy(def.text, scratch.text, sizeof(def.text));
        def.kind         = (QuestKind)scratch.kind;
        def.target       = scratch.target;
        def.rewardXP     = scratch.rewardXP;
        def.rewardBond   = scratch.rewardBond;
        def.requiresCaps = scratch.requiresCaps;
        _questDefCount++;
    }

    if (_questDefCount == 0) {
        loadBaseline();
        return false;
    }
    return true;
}
