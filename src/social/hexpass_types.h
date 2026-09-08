#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ── HexHound - HexPass Persisted Types ───────────────────────────
//
// The shapes that live inside PetState and therefore inside the save file.
// Deliberately free of every other project header so pet_core.h can include it
// without a cycle: hexpass.h needs PetState, PetState needs these types, and
// only splitting them keeps that from being circular.
//
// ── Sizing ────────────────────────────────────────────────────────────────
//
// The T-Dongle S3 has no PSRAM and roughly 250 KB of RAM. Every structure here
// is fixed size and lives inside PetState, so the whole subsystem's memory cost
// is known at compile time and an attacker filling the air with fake cards
// cannot change it by a single byte. That is the point: "the encounter store is
// a fixed-size ring. It cannot be grown by an attacker into a memory problem"
// is a property of the type, not of the code that maintains it.
//
// ── Nothing here is a transmitted format ──────────────────────────────────
//
// These are on-device records. The wire format is the fixed 33-byte payload in
// hexpass.h and shares no struct with these, so a field added here can never
// accidentally start being broadcast.

#define HEXPASS_SECRET_BYTES     32
#define HEXPASS_EID_BYTES        16
#define HEXPASS_FRIEND_BYTES      8
#define HEXPASS_TAG_BYTES         8

// How many friendships the device remembers. 16 is a judgement, not a derived
// number: it is large enough that a day of a small meetup fits, small enough
// that the ring plus the block list stays under a kilobyte. Residual risk 4 in
// the threat model ("conference density") is exactly the case this cannot
// satisfy, and no ring size fixes that - it changes which encounters survive,
// not whether some are lost.
#define HEXPASS_MAX_ENCOUNTERS   16
#define HEXPASS_MAX_BLOCKED       8

// Rate limits. See the ambiguity note in hexpass.h: the threat model's "at most
// one recorded per epoch, and at most N per day" is enforced BOTH per friend
// and globally, because only the global cap actually bounds the ring against a
// flood of unfamiliar cards.
#define HEXPASS_MAX_PER_FRIEND_PER_DAY   4
#define HEXPASS_MAX_PER_DAY             32

// Badge 0 means "no badge selected", which is what a fresh pet broadcasts.
#define HEXPASS_BADGE_NONE        0

// ── Bounded field ranges ──────────────────────────────────────────────────
//
// These live beside the persisted types, not beside the tables they bound, on
// purpose: the pet serialiser has to re-check every one of them when it reads
// a save, and it must not need the whole HexPass API to do it.
//
// All four of stage, form, badge and greeting INDEX A TABLE when an encounter
// is drawn. On a board with no MMU an out-of-range index does not fault, it
// renders whatever bytes follow the table, so "the save had a bad value" turns
// into garbage on the glass rather than a crash anyone could debug. Checking
// them on load is the whole defence.
// How much of the pet a stranger can see. Friends always see everything: they
// hold the seed, so the personality fields are meaningful to them regardless.
enum HexPassVisibility : uint8_t {
    // Rotating identifier and stage only. Strangers learn that A hound passed
    // and roughly how grown it is, which is about 2.3 bits and not enough to
    // pick one device out of a crowd. This is the default.
    HEXPASS_VIS_FRIENDS = 0,
    // Full card in the clear: form, badge and greeting readable by anyone
    // nearby. Roughly 11 bits of fingerprint. The owner opts into this
    // knowingly, having been shown what it costs.
    HEXPASS_VIS_DISCOVERABLE = 1,
    HEXPASS_VIS_COUNT
};

// Top bit of the stage byte carries the discoverable flag. Stage is 1..5 so
// the low three bits are all it needs; this keeps the card 33 bytes in both
// visibility modes, because a length that varied with the setting would leak
// the setting to anyone counting bytes.
#define HEXPASS_STAGE_DISCOVERABLE_BIT  0x80
#define HEXPASS_STAGE_MASK              0x0F

#define HEXPASS_STAGE_MIN      1
#define HEXPASS_STAGE_MAX      5
#define HEXPASS_FORM_MAX       6      // FORM_PACKMASTER; 0 is FORM_UNSET
#define HEXPASS_GREETING_COUNT 8
#define HEXPASS_BADGE_COUNT    8

// One remembered friendship.
//
// It holds NO name, NO location and NO time of day. It cannot: none of those
// are in the payload, and the whole design depends on them never being added.
// What it holds is a derived friendship id and the last card that id showed.
struct HexPassEncounter {
    uint8_t  friendId[HEXPASS_FRIEND_BYTES] = {};
    // Our epoch when this friendship was last recorded. Enforces the
    // one-record-per-epoch rule without a clock.
    uint32_t lastEpoch = 0;
    // Monotonic recency stamp, used only to choose an eviction victim. Not a
    // timestamp: there is no RTC, and a counter that pretends to be a time
    // would be a fabricated fact. See HexPassState::seq.
    uint32_t lastSeq   = 0;
    uint16_t meetCount = 0;
    // PetState::questDay when metToday was last reset. The pet's existing
    // notion of a day, reused rather than reinvented.
    uint16_t lastDay   = 0;
    uint8_t  metToday  = 0;
    // Last card seen from this friend. EVERY ONE of these indexes a table when
    // the encounter is drawn, so every one is range-checked on load.
    uint8_t  stage     = 0;
    uint8_t  form      = 0;
    uint8_t  badge     = HEXPASS_BADGE_NONE;
    uint8_t  greeting  = 0;
};

// Everything HexPass persists. Embedded in PetState so it rides the existing
// A/B save path and needs no second storage mechanism.
struct HexPassState {
    // Never transmitted, under any circumstance. Generated once from
    // esp_random() and destroyed by wipe().
    uint8_t  secret[HEXPASS_SECRET_BYTES] = {};
    bool     secretValid = false;

    // OPT-IN, DEFAULTING TO OFF. Not opt-out, not on-with-a-notice. A default
    // of true here would be the single worst line in this subsystem.
    bool     enabled     = false;

    // How much of the pet a STRANGER can see. Independent of `enabled`, which
    // governs whether anything is transmitted at all.
    //
    // The reason this is a runtime setting rather than commented-out code kept
    // for later: commented code is never compiled, so it rots, and it is never
    // tested, so nobody knows whether it works. It looks like a finished
    // feature waiting to be switched on when it is really unverified code that
    // will need rewriting. A setting compiles and the tests keep it honest.
    //
    // The cost of DISCOVERABLE is concrete and must be shown to the owner
    // before they choose it: form x badge x greeting is about 11 bits of
    // fingerprint, which in a hall of a few hundred units is enough to link a
    // device across a rotation boundary by its cosmetic profile alone. That
    // defeats the rotation everything else rests on. It is a fine trade at a
    // conference where being seen is the point, and a bad one on a commute.
    // The owner decides, having been told, and can change it at any time.
    HexPassVisibility visibility = HEXPASS_VIS_FRIENDS;
    // One-action toggle that stops advertising immediately without discarding
    // the secret or the friendships.
    bool     privateMode = false;

    // Rotation counter. Persisted so a reboot cannot re-emit an identifier the
    // device already used; begin() advances it once on every boot.
    uint32_t epoch       = 0;
    // Monotonic stamp handed to encounters for eviction ordering.
    uint32_t seq         = 0;

    // Global daily cap bookkeeping, keyed to PetState::questDay.
    uint16_t day           = 0;
    uint16_t recordedToday = 0;

    uint8_t  encounterCount = 0;
    uint8_t  blockedCount   = 0;

    // What this device puts on its own card. Owner-chosen, both range-checked.
    uint8_t  badge    = HEXPASS_BADGE_NONE;
    uint8_t  greeting = 0;

    HexPassEncounter encounters[HEXPASS_MAX_ENCOUNTERS];
    uint8_t          blocked[HEXPASS_MAX_BLOCKED][HEXPASS_FRIEND_BYTES] = {};
};

// ── Hex codec ─────────────────────────────────────────────────────────────
//
// Lives here, in the header with no dependencies, for one reason: the pet
// serialiser has to write these byte arrays into JSON and read them back, and
// if it carried its own copy of this the two encodings could drift and a save
// would silently stop round-tripping. Header-inline also means pet_core does
// NOT gain a link dependency on the crypto translation unit, so the existing
// unit tests that compile pet_core.cpp keep building untouched.
namespace HexPassHex {

// `out` needs len * 2 + 1 chars. Always lowercase.
inline void encode(const uint8_t* bytes, size_t len, char* out) {
    static const char* const digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

// Strict: false unless `hex` is exactly len * 2 characters, all valid hex.
// A save is untrusted input, so a short, long or malformed string is a reject
// rather than a partial decode leaving half the buffer stale.
inline bool decode(const char* hex, uint8_t* out, size_t len) {
    if (hex == nullptr || out == nullptr) {
        return false;
    }
    if (strlen(hex) != len * 2) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        for (int nib = 0; nib < 2; nib++) {
            const char c = hex[i * 2 + nib];
            uint8_t v;
            if (c >= '0' && c <= '9')      v = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
            else return false;
            byte = (uint8_t)((byte << 4) | v);
        }
        out[i] = byte;
    }
    return true;
}

}  // namespace HexPassHex
