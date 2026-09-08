#pragma once

#include <stdint.h>
#include <stddef.h>

#include "hexpass_types.h"
#include "hexpass_crypto.h"

// ── HexHound - HexPass ───────────────────────────────────────────
//
// Identity, payload and encounter storage for the passing-pets feature.
// Specified by docs/hexpass-threat-model.md; read that first, it is the
// authority and this file is its implementation.
//
// ════════════════════════════════════════════════════════════════════════════
//  THERE IS NO RADIO IN THIS SUBSYSTEM AND THERE MUST NOT BE ONE YET.
//
//  No advertising, no scanning, no NimBLE, no esp_ble_*. The threat model is
//  a DRAFT awaiting human review, and its own closing section says the parts
//  that touch the air are the ones that cannot be quietly fixed once units are
//  in the wild. Everything here is exercised by feeding buildPayload() output
//  into parsePayload() and record(), which is exactly what the radio would do
//  and needs no radio to test.
//
//  If you are about to add a BLE include to src/social/, stop and get the
//  threat model signed off first.
// ════════════════════════════════════════════════════════════════════════════
//
// ── Wire format (33 bytes, fixed, no optional fields) ─────────────────────
//
//   offset  size  field
//   0       1     version      HEXPASS_PROTOCOL_VERSION; unknown is rejected
//   1       16    eid          rotating ephemeral identifier
//   17      1     stage        1..5
//   18      1     form         0..6
//   19      1     badge        one owner-selected achievement id
//   20      1     greeting     index into a FIXED preset table, never free text
//   21      4     counter      little-endian, monotonic within an epoch
//   25      8     tag          HMAC over bytes 0..24 under the epoch tag key
//
// A variable-length payload leaks in its length alone, so parse() rejects any
// buffer that is not exactly HEXPASS_PAYLOAD_BYTES.
//
// ── Derivations ───────────────────────────────────────────────────────────
//
//   EID(epoch) = HMAC-SHA256(secret, "hexpass-eid" || epoch_le32)[0..15]
//   FriendID   = HMAC-SHA256(sort(EID_a, EID_b), "hexpass-friend")[0..7]
//   tagKey     = HMAC-SHA256(EID, "hexpass-tag")
//   tag        = HMAC-SHA256(tagKey, payload[0..24])[0..7]
//
// The threat model gives the first two verbatim and leaves two things
// unstated, resolved here and flagged in docs/p2w1-wiring.md rather than
// silently chosen:
//
//   1. The encoding of `epoch` in the EID message. Fixed as 4-byte
//      little-endian. Any encoding works as long as both ends agree; what
//      would NOT work is leaving it to whoever writes the radio layer.
//   2. What "the epoch key" for the tag is. It cannot be secret-derived: the
//      receiver does not have the sender's secret and never will without a
//      pairing handshake, so a secret-keyed tag would be unverifiable. It is
//      therefore derived from the EID, which is public in the payload. The
//      consequence is stated plainly: the tag is an INTEGRITY check that
//      detects edits to a captured card. It authenticates nothing, exactly as
//      the threat model says ("It does not prove identity to a stranger, and
//      is not claimed to"). Anyone can mint a fresh valid card. Replay and
//      rate limiting, not the tag, are what stop that mattering.

#define HEXPASS_PROTOCOL_VERSION  1
#define HEXPASS_PAYLOAD_BYTES     33

// Field offsets, named so the builder and the parser cannot drift apart.
#define HEXPASS_OFF_VERSION    0
#define HEXPASS_OFF_EID        1
#define HEXPASS_OFF_STAGE     17
#define HEXPASS_OFF_FORM      18
#define HEXPASS_OFF_BADGE     19
#define HEXPASS_OFF_GREETING  20
#define HEXPASS_OFF_COUNTER   21
#define HEXPASS_OFF_TAG       25
// Bytes covered by the tag: everything before it.
#define HEXPASS_SIGNED_BYTES  HEXPASS_OFF_TAG

// The field ranges (HEXPASS_STAGE_MIN/MAX, HEXPASS_FORM_MAX,
// HEXPASS_GREETING_COUNT, HEXPASS_BADGE_COUNT) are in hexpass_types.h, beside
// the persisted structs, because the save loader needs them too. A received
// card is untrusted input and each of those fields ends up indexing a table on
// the encounter screen, so they are checked at parse AND re-checked on load.

// The FIXED preset greeting table, HEXPASS_GREETING_COUNT entries. A greeting
// is an index into this and can never be free text: "any free-form text the
// user can author" is on the threat model's never-transmitted list, because a
// user-authored string is a handle and a handle is an identity.
extern const char* const HEXPASS_GREETINGS[];

// The FIXED badge table, HEXPASS_BADGE_COUNT entries. Same reasoning: an id
// into a compiled-in list, never a string. Index 0 is "no badge".
extern const char* const HEXPASS_BADGES[];

// A parsed card. Populated only when parse() returned PARSE_OK.
struct HexPassCard {
    uint8_t  version  = 0;
    uint8_t  eid[HEXPASS_EID_BYTES] = {};
    uint8_t  stage    = HEXPASS_STAGE_MIN;
    uint8_t  form     = 0;
    uint8_t  badge    = HEXPASS_BADGE_NONE;
    uint8_t  greeting = 0;
    uint32_t counter  = 0;
    // Whether the SENDER opted into being readable by strangers. When false,
    // form/badge/greeting above are blanked by the parser, because the sender
    // filled them with noise and carrying that forward would have the UI state
    // a form and badge the sender never chose.
    bool     discoverable = false;
};

enum HexPassParse : uint8_t {
    HEXPASS_PARSE_OK = 0,
    HEXPASS_PARSE_BAD_LENGTH,    // not exactly 33 bytes
    HEXPASS_PARSE_BAD_VERSION,   // reject unknown versions rather than guessing
    HEXPASS_PARSE_BAD_TAG,       // edited in flight, or not a HexPass card
    HEXPASS_PARSE_BAD_FIELD      // stage/form/badge/greeting out of range
};

// Why record() did or did not store something. A single bool could not tell
// "we already know them" from "they are blocked", and the screen needs both.
enum HexPassRecord : uint8_t {
    HEXPASS_RECORD_NEW = 0,       // new friendship stored
    HEXPASS_RECORD_REPEAT,        // known friend, meet count advanced
    HEXPASS_RECORD_DISABLED,      // opt-in off, or private mode on
    HEXPASS_RECORD_NO_SECRET,     // no identity yet; begin() not called
    HEXPASS_RECORD_SELF,          // our own card came back to us
    HEXPASS_RECORD_REPLAY,        // this (EID, counter) was already seen
    HEXPASS_RECORD_RATE_EPOCH,    // already recorded this friend this epoch
    HEXPASS_RECORD_RATE_FRIEND,   // this friend hit its daily cap
    HEXPASS_RECORD_RATE_DAY,      // the device hit its daily cap
    HEXPASS_RECORD_BLOCKED        // blocked FriendID, never recorded again
};

class HexPass {
public:
    static HexPass& instance();

    // Ensures an identity exists (generating one on first run) and advances
    // the epoch so a reboot never re-emits an identifier this device already
    // used. Clears all volatile state. Safe to call more than once.
    void begin();

    // ── Identity ──────────────────────────────────────────────────────────

    bool     hasSecret() const;
    uint32_t epoch() const;

    // EID for an arbitrary epoch. Writes HEXPASS_EID_BYTES. All-zero, and
    // returns false, when there is no secret: a zero EID is not a valid
    // identifier and callers must not treat it as one.
    bool deriveEid(uint32_t epoch, uint8_t out[HEXPASS_EID_BYTES]) const;
    bool currentEid(uint8_t out[HEXPASS_EID_BYTES]) const;

    // ── Rotation: ONE trigger, never two timers ───────────────────────────
    //
    // The threat model's single loudest warning is that rotating the payload
    // identifier while the BLE MAC stays put accomplishes nothing, and that
    // two timers with equal periods are not the same thing as one trigger.
    //
    // So there is exactly ONE way to rotate, this function, and the future
    // radio layer does NOT get its own timer. It registers its address change
    // through setRotationHook() and is called from inside rotate(), after the
    // epoch has advanced. A second timer cannot be added without deleting
    // this comment and the test that asserts the hook fires exactly once per
    // rotation, in the same call that changed the EID.
    void rotate();
    void setRotationHook(void (*hook)(void* user), void* user = nullptr);

    // ── Friendship ────────────────────────────────────────────────────────

    // FriendID for a pair of EIDs. Order-independent by construction: the two
    // EIDs are sorted before hashing, so both devices compute the same value.
    // Static because it depends on nothing but its arguments.
    static void friendId(const uint8_t a[HEXPASS_EID_BYTES],
                         const uint8_t b[HEXPASS_EID_BYTES],
                         uint8_t out[HEXPASS_FRIEND_BYTES]);

    // ── Payload ───────────────────────────────────────────────────────────

    // Builds this device's card into `out` and advances the per-epoch counter.
    // Returns false when broadcasting() is false or there is no secret, and in
    // that case `out` is zeroed: a caller that ignores the return value must
    // not end up transmitting a stale card.
    bool buildPayload(uint8_t out[HEXPASS_PAYLOAD_BYTES]);

    // Parses and verifies a card. Static: parsing must not depend on our own
    // state, so a malformed card is rejected identically whether or not this
    // device has an identity.
    static HexPassParse parsePayload(const uint8_t* buf, size_t len,
                                     HexPassCard& out);

    // ── Encounters ────────────────────────────────────────────────────────

    // The whole pipeline: block list, self check, replay, rate limits, then
    // store. Nothing else may write the encounter ring.
    HexPassRecord record(const HexPassCard& card);

    uint8_t                 encounterCount() const;
    const HexPassEncounter* encounter(uint8_t index) const;
    // Index of a stored friendship, or -1.
    int                     findFriend(const uint8_t fid[HEXPASS_FRIEND_BYTES]) const;

    // ── Block list ────────────────────────────────────────────────────────

    // Blocks a FriendID and forgets any encounter already stored for it.
    // Returns false only when the block list is full.
    bool block(const uint8_t fid[HEXPASS_FRIEND_BYTES]);
    bool isBlocked(const uint8_t fid[HEXPASS_FRIEND_BYTES]) const;
    uint8_t blockedCount() const;

    // ── Consent and control ───────────────────────────────────────────────

    bool enabled() const;
    void setEnabled(bool on);
    bool privateMode() const;
    void setPrivateMode(bool on);

    // How much a STRANGER sees. Independent of enabled(), which governs
    // whether anything is sent at all. Defaults to FRIENDS.
    //
    // Callers presenting this MUST show what DISCOVERABLE costs before it is
    // chosen: form, badge and greeting together are about 11 bits, which in a
    // crowded room is enough for an observer to follow a device across an
    // identifier rotation by its cosmetic profile. That is a reasonable trade
    // at a conference and a poor one on a commute, and it is the owner's call
    // to make knowingly, not a default to be inherited.
    HexPassVisibility visibility() const;
    bool setVisibility(HexPassVisibility v);
    // The single question the radio layer will ask before it transmits.
    bool broadcasting() const;

    // Owner-selected card contents. Both range-checked; an out-of-range value
    // is refused rather than clamped, so a caller cannot quietly broadcast a
    // different badge from the one it asked for.
    uint8_t badge() const;
    bool    setBadge(uint8_t badgeId);
    uint8_t greeting() const;
    bool    setGreeting(uint8_t greetingId);

    // Erases encounters, friendships AND the secret, then generates a new
    // secret. Destroying the secret is what makes a wipe a genuine reset: every
    // identifier derived from the old one becomes underivable, so past
    // sightings cannot be linked to the device's new identity.
    void wipe();

    // ── Introspection ─────────────────────────────────────────────────────
    // "The device must be able to say plainly what it is broadcasting. A user
    // who cannot inspect it cannot consent to it." These exist so the UI can
    // show the actual bytes rather than a description of them.

    // Formats the card this device would broadcast right now as lowercase hex,
    // WITHOUT advancing the counter, so inspecting is not transmitting. Needs
    // HEXPASS_PAYLOAD_BYTES * 2 + 1 chars. Returns false when disabled.
    bool describePayloadHex(char* out, size_t outLen) const;

    // Runs the crypto backend's known-answer test. False means the identity
    // derivation on THIS build cannot be trusted and HexPass must stay off.
    static bool selfTest() { return HexPassCrypto::selfTest(); }

    // ── Save support ──────────────────────────────────────────────────────
    // Re-derives volatile state after a save has been loaded into PetState.
    // Separate from begin() because loading must not rotate the epoch twice.
    void afterLoad();

    // Test seam. Replaces the entropy source so a test can make identity
    // generation deterministic. Passing nullptr restores the platform source.
    static void setRandomSource(void (*fn)(uint8_t* out, size_t len));

private:
    HexPass() = default;

    HexPassState&       st();
    const HexPassState& st() const;

    void generateSecret();
    void resetVolatile();
    // True when this (eid, counter) pair has been seen since the last
    // rotation; records it when it has not.
    bool seenBefore(const uint8_t eid[HEXPASS_EID_BYTES], uint32_t counter);
    // Rolls the daily counters when PetState::questDay has moved on.
    void refreshDay();
    // Index of the least recently seen encounter. The ring is full when this
    // is called, so it always returns a valid index.
    uint8_t evictionVictim() const;

    // ── Volatile replay table ─────────────────────────────────────────────
    // NOT persisted. Its entries are only meaningful within one epoch, and an
    // EID does not survive an epoch boundary, so carrying it across a reboot
    // would preserve nothing but stale bytes. Fixed size for the same reason
    // as the ring: an attacker must not be able to grow it.
    static const uint8_t REPLAY_SLOTS = 16;
    struct ReplaySlot {
        uint8_t  eid[HEXPASS_EID_BYTES] = {};
        uint32_t lastCounter = 0;
        bool     used = false;
    };
    ReplaySlot _replay[REPLAY_SLOTS];
    uint8_t    _replayNext = 0;

    // Per-epoch advertisement counter. Resets on rotate(), which is what
    // "monotonic within an epoch" means.
    uint32_t _counter = 0;

    void (*_rotationHook)(void* user) = nullptr;
    void*  _rotationUser = nullptr;
};
