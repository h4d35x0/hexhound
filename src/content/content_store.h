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

#include "content_types.h"
#include "content_pack.h"

// ── HexHound - Content Store ─────────────────────────────────────
//
// Owns the single in-RAM copy of every dialogue line and quest definition, and
// remembers where each table came from.
//
// Load order, and the reason for it:
//   1. The compiled-in baseline pack (content_baseline.h) always fills the
//      tables first. A freshly-erased device has no SPIFFS content at all, and
//      that is the NORMAL first-boot state rather than an error case. The pet
//      has to have something to say on the very first power-up, so the
//      fallback is not optional.
//   2. A SIGNED pack under /content/*.hcp then REPLACES a table, but only if
//      its signature verifies, its CBOR validates and it yields at least one
//      valid entry. A pack that is missing, unsigned, signed by a key this
//      firmware does not carry, truncated, oversized or malformed leaves the
//      baseline in place; a content update can never leave the pet mute.
//
// Sizing: both tables are fixed arrays that are never grown. About 6.6 KB of
// static RAM on the T-Dongle S3, which has no PSRAM. A pack bigger than these
// caps is truncated at load; it is not allowed to allocate its way past them.
//
// ── P3-W1: what moving to signed CBOR actually changed ────────────────────
//
// The claim this file used to make was "moving to signed CBOR later changes
// this file and nothing else". That was ALMOST right and the exception is
// worth stating rather than papering over.
//
// What held: content_types.h did not change, the baseline did not change, the
// engines did not change, and no authored JSON had to be rewritten. The tool
// reads the same JSON fields with the same names and the same name-or-ordinal
// spellings, and the CBOR decoders below resolve them through the SAME name
// tables with the SAME fallbacks as the JSON parsers.
//
// What did not hold: it was not ONE file. Verifying a signature before
// parsing needs crypto (content_crypto), a bounded decoder (content_cbor) and
// an envelope reader (content_pack), and the envelope reader is shared with
// pet_inventory.cpp because recipes are packs too. The honest version of the
// claim is "the DATA MODEL does not change and authored content does not get
// rewritten", which is the part that was actually load bearing.
//
// The plain-JSON parsers below are kept, but they are no longer reachable
// from the filesystem. See applyDialogueJson().

#define CONTENT_MAX_DIALOGUE_LINES  40
#define CONTENT_MAX_QUEST_DEFS      20

// Signed CBOR packs. The .json paths are deliberately NOT read any more: a
// file anyone can drop on SPIFFS must not be able to change what the device
// says without a signature behind it.
#define CONTENT_PATH_DIALOGUE  "/content/dialogue.hcp"
#define CONTENT_PATH_QUESTS    "/content/quests.hcp"

// Largest pack file the loader will read. Anything bigger is refused whole,
// because a half-parsed pack is worse than the baseline it replaced.
#define CONTENT_MAX_PACK_BYTES  6144

// Numbers a dialogue line may ask the engine to substitute. Content declares
// the slot; the engine fetches the value from the caller. Content can name a
// statistic but never invent one.
enum MemorySlot : uint8_t {
    MEM_SLOT_VISITS = 0,      // times the pet has been checked on
    MEM_SLOT_PATROLS,         // patrols run together
    MEM_SLOT_NEW_NETWORKS,    // never-before-seen networks logged
    MEM_SLOT_THREATS,         // findings raised
    MEM_SLOT_QUESTS_DONE,     // quests completed, all time
    MEM_SLOT_DAYS_AWAY,       // days since the last power-on
    MEM_SLOT_BEST_SCORE,      // best minigame score
    MEM_SLOT_COUNT
};

// A line with no substitution. Matches the 0xFF "any" convention used by the
// trait and stage filters in content_types.h.
#define DIALOGUE_MEMORY_NONE  0xFF

namespace Content {

// xorshift32. Shared by both engines so a daily quest roll and a dialogue pick
// can be reproduced from a seed, which is what makes either testable. Never
// used for anything that needs to be unpredictable.
inline uint32_t nextRandom(uint32_t& state) {
    if (state == 0) state = 0x9E3779B9u;   // any non-zero seed will do
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace Content

class ContentStore {
public:
    static ContentStore& instance();

    // Fills both tables. Always leaves usable content behind, so this cannot
    // fail in a way the caller has to handle; the return value reports only
    // whether a pack was picked up, for logging.
    bool begin();

    uint8_t dialogueCount() const { return _dialogueCount; }
    const DialogueLine* dialogue(uint8_t index) const;

    uint8_t questDefCount() const { return _questDefCount; }
    const QuestDef* questDef(uint8_t index) const;
    const QuestDef* questDefById(const char* id) const;

    bool dialogueFromPack() const { return _dialogueFromPack; }
    bool questsFromPack() const   { return _questsFromPack; }

    // Why the last load of each pack did what it did, for the diagnostics
    // screen and the boot log. PACK_ABSENT is the normal answer on a device
    // that has never been given a pack, and is not an error.
    ContentPackStatus dialoguePackStatus() const { return _dialogueStatus; }
    ContentPackStatus questPackStatus() const    { return _questStatus; }

    // Replace a table from the CBOR body of a pack whose signature has ALREADY
    // been verified. Returns false, and leaves the current table untouched,
    // when the body is not valid CBOR in the accepted profile or contains no
    // usable rows.
    //
    // Takes a body, not a file, so that the only way to reach it is through
    // ContentPack::verifyImage(). There is no entry point that parses pack
    // structure without a signature behind it.
    bool applyDialogueCbor(const uint8_t* body, size_t len);
    bool applyQuestCbor(const uint8_t* body, size_t len);

    // Replace a table from a NUL-terminated JSON buffer.
    //
    // KEPT, and no longer reachable from the filesystem. These are the
    // authoring and test entry points: the unit tests drive them, and the
    // signing tool consumes the same JSON on a desktop. What changed in P3-W1
    // is that nothing on SPIFFS can reach them, because an unsigned file must
    // not be able to change what the device says.
    //
    // If a future serial or BLE content push is added it MUST go through the
    // signed path, not through here. This is in-process API, not an input.
    bool applyDialogueJson(const char* json);
    bool applyQuestJson(const char* json);

    // Discards any pack and restores the compiled-in content.
    void resetToBaseline();

private:
    ContentStore() = default;

    void loadBaseline();

    // Reads and applies one pack file. Returns false when the file is absent,
    // oversized, unparseable or empty of valid entries; in every one of those
    // cases the caller keeps the baseline table.
    bool loadDialoguePack();
    bool loadQuestPack();

    DialogueLine _dialogue[CONTENT_MAX_DIALOGUE_LINES];
    QuestDef     _questDefs[CONTENT_MAX_QUEST_DEFS];
    uint8_t      _dialogueCount = 0;
    uint8_t      _questDefCount = 0;
    bool         _dialogueFromPack = false;
    bool         _questsFromPack = false;
    ContentPackStatus _dialogueStatus = PACK_ABSENT;
    ContentPackStatus _questStatus    = PACK_ABSENT;
};
