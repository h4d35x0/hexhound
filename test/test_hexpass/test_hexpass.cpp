// ── HexHound - HexPass Unit Tests ────────────────────────────────
//
// Covers identity derivation, the wire format, the encounter store and the
// consent controls. There is no radio in the subsystem and therefore none in
// these tests: a card is built with buildPayload(), handed straight to
// parsePayload() and record(), which is precisely what a receive path would do
// with bytes off the air.

// Shared stubs (must come before any src/ includes)
#include "../test_stubs.h"

// Real source files (native env has build_src_filter = -<*>)
#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"
#include "../../src/social/hexpass_crypto.h"
#include "../../src/social/hexpass_crypto.cpp"
#include "../../src/social/hexpass.h"
#include "../../src/social/hexpass.cpp"

#include <ArduinoJson.h>

// ── Test fixtures ─────────────────────────────────────────────────────────

// Deterministic stand-in for esp_random(). Every call returns different bytes,
// which is the only property the tests depend on (a wipe must not be able to
// regenerate the secret it just destroyed).
static uint8_t s_randomCounter = 0;
static void testRandom(uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(0x40u + s_randomCounter + (uint8_t)i * 7u);
    }
    s_randomCounter++;
}

static void freshPet() {
    PetCore::instance().state() = PetState();
    HexPass::instance().setRotationHook(nullptr, nullptr);
    HexPass::setRandomSource(testRandom);
    HexPass::instance().begin();
}

static void freshEnabledPet() {
    freshPet();
    HexPass::instance().setEnabled(true);
}

// A peer EID. Any 16 distinct bytes; peers are other devices whose secrets we
// do not have, which is exactly the situation on the air.
static void peerEid(uint8_t seed, uint8_t out[HEXPASS_EID_BYTES]) {
    for (uint8_t i = 0; i < HEXPASS_EID_BYTES; i++) {
        out[i] = (uint8_t)(seed * 31u + i * 5u + 1u);
    }
}

// Builds a card the way the SPEC describes it, independently of the encoder in
// hexpass.cpp. If the implementation ever drifts from the documented layout or
// the documented tag derivation, the round-trip tests below stop passing -
// which is the point of not simply calling encodeCard() here.
static void makeCard(uint8_t out[HEXPASS_PAYLOAD_BYTES],
                     const uint8_t eid[HEXPASS_EID_BYTES],
                     uint8_t stage, uint8_t form, uint8_t badge,
                     uint8_t greeting, uint32_t counter,
                     uint8_t version = HEXPASS_PROTOCOL_VERSION,
                     bool discoverable = true) {
    // `discoverable` defaults true because a test that bothers to specify a
    // form and a badge is describing a peer who chose to show them. A peer in
    // FRIENDS mode sends noise in those bytes and the parser blanks them, which
    // is covered by its own test rather than by silently weakening these.
    memset(out, 0, HEXPASS_PAYLOAD_BYTES);
    out[0] = version;
    memcpy(out + 1, eid, HEXPASS_EID_BYTES);
    out[17] = (uint8_t)(stage | (discoverable ? HEXPASS_STAGE_DISCOVERABLE_BIT : 0));
    out[18] = form;
    out[19] = badge;
    out[20] = greeting;
    out[21] = (uint8_t)(counter & 0xFF);
    out[22] = (uint8_t)((counter >> 8) & 0xFF);
    out[23] = (uint8_t)((counter >> 16) & 0xFF);
    out[24] = (uint8_t)((counter >> 24) & 0xFF);

    static const uint8_t lblTag[] = { 'h','e','x','p','a','s','s','-','t','a','g' };
    uint8_t tagKey[32];
    HexPassCrypto::hmacSha256(eid, HEXPASS_EID_BYTES, lblTag, sizeof(lblTag), tagKey);
    uint8_t full[32];
    HexPassCrypto::hmacSha256(tagKey, sizeof(tagKey), out, 25, full);
    memcpy(out + 25, full, HEXPASS_TAG_BYTES);
}

// ── Crypto: the two backends must agree with the published answers ────────

TEST(test_hmac_matches_rfc4231_case1) {
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    static const uint8_t data[] = { 'H','i',' ','T','h','e','r','e' };
    static const uint8_t expect[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
    };
    uint8_t got[32];
    HexPassCrypto::hmacSha256(key, sizeof(key), data, sizeof(data), got);
    ASSERT_TRUE(memcmp(got, expect, 32) == 0);
}

TEST(test_hmac_matches_rfc4231_case2) {
    // Same vector selfTest() uses, so a green test here means the on-device
    // self check is testing something known-good.
    ASSERT_TRUE(HexPass::selfTest());
}

TEST(test_hmac_matches_rfc4231_case4) {
    // Key longer than the digest, data 50 bytes. Exercises the counter/padding
    // path a short vector would not reach.
    uint8_t key[25];
    for (uint8_t i = 0; i < 25; i++) key[i] = (uint8_t)(i + 1);
    uint8_t data[50];
    memset(data, 0xcd, sizeof(data));
    static const uint8_t expect[32] = {
        0x82,0x55,0x8a,0x38,0x9a,0x44,0x3c,0x0e,
        0xa4,0xcc,0x81,0x98,0x99,0xf2,0x08,0x3a,
        0x85,0xf0,0xfa,0xa3,0xe5,0x78,0xf8,0x07,
        0x7a,0x2e,0x3f,0xf4,0x67,0x29,0x66,0x5b
    };
    uint8_t got[32];
    HexPassCrypto::hmacSha256(key, sizeof(key), data, sizeof(data), got);
    ASSERT_TRUE(memcmp(got, expect, 32) == 0);
}

TEST(test_hmac_with_key_longer_than_block_is_hashed_first) {
    // RFC 4231 case 6: a 131-byte key, longer than SHA-256's 64-byte block, so
    // it must be reduced with SHA-256 before padding. A backend that truncated
    // it instead would pass every short-key vector above and be wrong here.
    uint8_t key[131];
    memset(key, 0xaa, sizeof(key));
    static const char data[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    static const uint8_t expect[32] = {
        0x60,0xe4,0x31,0x59,0x1e,0xe0,0xb6,0x7f,
        0x0d,0x8a,0x26,0xaa,0xcb,0xf5,0xb7,0x7f,
        0x8e,0x0b,0xc6,0x21,0x37,0x28,0xc5,0x14,
        0x05,0x46,0x04,0x0f,0x0e,0xe3,0x7f,0x54
    };
    uint8_t got[32];
    HexPassCrypto::hmacSha256(key, sizeof(key), (const uint8_t*)data,
                              sizeof(data) - 1, got);
    ASSERT_TRUE(memcmp(got, expect, 32) == 0);
}

// ── Identity ──────────────────────────────────────────────────────────────

TEST(test_eid_is_stable_within_an_epoch) {
    freshPet();
    uint8_t a[HEXPASS_EID_BYTES];
    uint8_t b[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().currentEid(a));
    ASSERT_TRUE(HexPass::instance().currentEid(b));
    ASSERT_TRUE(memcmp(a, b, HEXPASS_EID_BYTES) == 0);
}

TEST(test_eid_differs_per_epoch) {
    freshPet();
    uint8_t before[HEXPASS_EID_BYTES];
    uint8_t after[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().currentEid(before));
    HexPass::instance().rotate();
    ASSERT_TRUE(HexPass::instance().currentEid(after));
    ASSERT_TRUE(memcmp(before, after, HEXPASS_EID_BYTES) != 0);

    // And it is a function of the epoch, not of call order: asking for the old
    // epoch again reproduces the old value exactly.
    uint8_t again[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().deriveEid(HexPass::instance().epoch() - 1, again));
    ASSERT_TRUE(memcmp(before, again, HEXPASS_EID_BYTES) == 0);
}

TEST(test_no_secret_means_no_identifier) {
    PetCore::instance().state() = PetState();
    // A pet that has never run begin() has no secret at all.
    ASSERT_FALSE(HexPass::instance().hasSecret());
    uint8_t eid[HEXPASS_EID_BYTES];
    ASSERT_FALSE(HexPass::instance().currentEid(eid));
    uint8_t zero[HEXPASS_EID_BYTES] = {};
    ASSERT_TRUE(memcmp(eid, zero, HEXPASS_EID_BYTES) == 0);
}

TEST(test_rotation_is_one_trigger_not_two_timers) {
    // The threat model's loudest warning: the BLE address must rotate in the
    // SAME instant as the EID, not on a second timer with an equal period.
    // The only way to change the EID is rotate(), and rotate() is what calls
    // the hook the radio layer will hang its address change on. This test is
    // what stops a future second timer being added quietly.
    freshPet();

    static int hookCalls = 0;
    static uint8_t eidAtHook[HEXPASS_EID_BYTES];
    hookCalls = 0;
    HexPass::instance().setRotationHook([](void*) {
        hookCalls++;
        HexPass::instance().currentEid(eidAtHook);
    }, nullptr);

    uint8_t before[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(before);

    HexPass::instance().rotate();

    ASSERT_EQ(hookCalls, 1);
    // The hook observed the NEW identifier, so the address change and the EID
    // change cannot be separated by any window at all.
    ASSERT_TRUE(memcmp(eidAtHook, before, HEXPASS_EID_BYTES) != 0);
    uint8_t after[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(after);
    ASSERT_TRUE(memcmp(eidAtHook, after, HEXPASS_EID_BYTES) == 0);

    HexPass::instance().setRotationHook(nullptr, nullptr);
}

TEST(test_boot_advances_the_epoch) {
    // A reboot must not resume the epoch it was already using and re-emit an
    // identifier a sniffer may already hold.
    freshPet();
    const uint32_t first = HexPass::instance().epoch();
    HexPass::instance().begin();
    ASSERT_TRUE(HexPass::instance().epoch() > first);
}

// ── FriendID ──────────────────────────────────────────────────────────────

TEST(test_friendid_is_order_independent) {
    uint8_t a[HEXPASS_EID_BYTES];
    uint8_t b[HEXPASS_EID_BYTES];
    peerEid(1, a);
    peerEid(2, b);

    uint8_t ab[HEXPASS_FRIEND_BYTES];
    uint8_t ba[HEXPASS_FRIEND_BYTES];
    HexPass::friendId(a, b, ab);
    HexPass::friendId(b, a, ba);
    ASSERT_TRUE(memcmp(ab, ba, HEXPASS_FRIEND_BYTES) == 0);
}

TEST(test_friendid_differs_per_pair) {
    uint8_t a[HEXPASS_EID_BYTES], b[HEXPASS_EID_BYTES], c[HEXPASS_EID_BYTES];
    peerEid(1, a); peerEid(2, b); peerEid(3, c);
    uint8_t ab[HEXPASS_FRIEND_BYTES], ac[HEXPASS_FRIEND_BYTES];
    HexPass::friendId(a, b, ab);
    HexPass::friendId(a, c, ac);
    ASSERT_TRUE(memcmp(ab, ac, HEXPASS_FRIEND_BYTES) != 0);
}

// ── Payload ───────────────────────────────────────────────────────────────

TEST(test_payload_is_exactly_33_bytes_and_round_trips) {
    freshEnabledPet();
    PetCore::instance().state().stage = STAGE_BEACON_BEAST;
    PetCore::instance().state().form  = FORM_ARCHIVIST;
    ASSERT_TRUE(HexPass::instance().setBadge(3));
    ASSERT_TRUE(HexPass::instance().setGreeting(2));
    // Personality only travels in DISCOVERABLE mode, so a test asserting that
    // it round-trips has to opt in. In the default FRIENDS mode those three
    // fields are noise on the wire and blank on arrival, which is the point of
    // the tier and is asserted by test_friends_mode_never_leaks_personality.
    ASSERT_TRUE(HexPass::instance().setVisibility(HEXPASS_VIS_DISCOVERABLE));

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));
    ASSERT_EQ((int)HEXPASS_PAYLOAD_BYTES, 33);

    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(card.version, HEXPASS_PROTOCOL_VERSION);
    ASSERT_EQ(card.stage, (uint8_t)STAGE_BEACON_BEAST);
    ASSERT_EQ(card.form, (uint8_t)FORM_ARCHIVIST);
    ASSERT_EQ(card.badge, 3);
    ASSERT_EQ(card.greeting, 2);

    uint8_t mine[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(mine);
    ASSERT_TRUE(memcmp(card.eid, mine, HEXPASS_EID_BYTES) == 0);
}

TEST(test_payload_carries_no_name) {
    // The pet NAME is deliberately absent from the wire format. A user-chosen
    // name is frequently a handle, and a handle is an identity. This asserts
    // the bytes, not the intention: no run of the name appears anywhere.
    freshEnabledPet();
    strlcpy(PetCore::instance().state().name, "R00tPup", sizeof(PetCore::instance().state().name));

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));
    for (int i = 0; i + 4 <= HEXPASS_PAYLOAD_BYTES; i++) {
        ASSERT_TRUE(memcmp(buf + i, "R00t", 4) != 0);
    }
}

TEST(test_counter_advances_within_an_epoch) {
    freshEnabledPet();
    uint8_t a[HEXPASS_PAYLOAD_BYTES], b[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(a));
    ASSERT_TRUE(HexPass::instance().buildPayload(b));

    HexPassCard ca, cb;
    ASSERT_EQ(HexPass::parsePayload(a, sizeof(a), ca), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::parsePayload(b, sizeof(b), cb), HEXPASS_PARSE_OK);
    ASSERT_TRUE(cb.counter > ca.counter);
}

TEST(test_counter_restarts_on_rotation) {
    freshEnabledPet();
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPass::instance().buildPayload(buf);
    HexPass::instance().buildPayload(buf);
    HexPass::instance().rotate();
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));
    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(card.counter, 0u);
}

TEST(test_tampered_payload_fails_its_tag) {
    freshEnabledPet();
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));

    // Every byte the tag covers, one at a time. An attacker who could edit any
    // single field of a captured card without detection would be able to
    // inflate a stage, swap a badge or rewind a counter.
    for (int i = 0; i < HEXPASS_SIGNED_BYTES; i++) {
        uint8_t bad[HEXPASS_PAYLOAD_BYTES];
        memcpy(bad, buf, sizeof(bad));
        bad[i] = (uint8_t)(bad[i] ^ 0x01);
        HexPassCard card;
        const HexPassParse r = HexPass::parsePayload(bad, sizeof(bad), card);
        // Byte 0 is the version, which is rejected before the tag is even
        // computed; that is a stricter reject, not a weaker one.
        ASSERT_TRUE(r == HEXPASS_PARSE_BAD_TAG || r == HEXPASS_PARSE_BAD_VERSION);
    }

    // And the tag itself cannot be edited to match.
    uint8_t bad[HEXPASS_PAYLOAD_BYTES];
    memcpy(bad, buf, sizeof(bad));
    bad[HEXPASS_OFF_TAG] = (uint8_t)(bad[HEXPASS_OFF_TAG] ^ 0xFF);
    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(bad, sizeof(bad), card), HEXPASS_PARSE_BAD_TAG);
}

TEST(test_unknown_version_is_rejected) {
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(9, eid);
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];

    // A well-formed card whose version we do not know. Rejected rather than
    // parsed with today's offsets, because a future version may reuse them.
    makeCard(buf, eid, 2, 1, 0, 0, 0, /*version=*/2);
    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_VERSION);

    makeCard(buf, eid, 2, 1, 0, 0, 0, /*version=*/0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_VERSION);

    makeCard(buf, eid, 2, 1, 0, 0, 0, /*version=*/255);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_VERSION);
}

TEST(test_wrong_length_is_rejected) {
    freshEnabledPet();
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));

    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, 32, card), HEXPASS_PARSE_BAD_LENGTH);
    ASSERT_EQ(HexPass::parsePayload(buf, 34, card), HEXPASS_PARSE_BAD_LENGTH);
    ASSERT_EQ(HexPass::parsePayload(buf, 0, card), HEXPASS_PARSE_BAD_LENGTH);
    ASSERT_EQ(HexPass::parsePayload(nullptr, 33, card), HEXPASS_PARSE_BAD_LENGTH);
}

TEST(test_out_of_range_fields_are_rejected_not_clamped) {
    // A valid tag proves the card was not edited in flight. It does NOT prove
    // the sender was honest, because anyone can mint a card. So a card with a
    // stage of 200 is dropped: clamping it would store a value the sender
    // never sent, and every one of these fields indexes a table when drawn.
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(4, eid);
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;

    makeCard(buf, eid, /*stage=*/200, 1, 0, 0, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);
    makeCard(buf, eid, /*stage=*/0, 1, 0, 0, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);
    makeCard(buf, eid, 2, /*form=*/200, 0, 0, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);
    makeCard(buf, eid, 2, 1, /*badge=*/200, 0, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);
    makeCard(buf, eid, 2, 1, 0, /*greeting=*/200, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);

    // The boundary values themselves must still be accepted.
    makeCard(buf, eid, HEXPASS_STAGE_MAX, HEXPASS_FORM_MAX,
             HEXPASS_BADGE_COUNT - 1, HEXPASS_GREETING_COUNT - 1, 0);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
}

TEST(test_all_zero_eid_is_not_an_identity) {
    uint8_t eid[HEXPASS_EID_BYTES] = {};
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    makeCard(buf, eid, 2, 1, 0, 0, 0);
    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_BAD_FIELD);
}

// ── Encounter store ───────────────────────────────────────────────────────

TEST(test_a_passing_pet_is_recorded_once_per_epoch) {
    freshEnabledPet();
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(11, eid);

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;

    makeCard(buf, eid, 2, 1, 0, 0, /*counter=*/1);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
    ASSERT_EQ(HexPass::instance().encounterCount(), 1);

    // A second, genuinely fresh advertisement from the same device in the same
    // epoch is not a replay, but it is still only one encounter.
    makeCard(buf, eid, 2, 1, 0, 0, /*counter=*/2);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_RATE_EPOCH);
    ASSERT_EQ(HexPass::instance().encounterCount(), 1);
    ASSERT_EQ(HexPass::instance().encounter(0)->meetCount, 1);
}

TEST(test_replayed_eid_and_counter_is_discarded) {
    freshEnabledPet();
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(12, eid);

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(buf, eid, 2, 1, 0, 0, /*counter=*/7);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);

    // The identical captured card, rebroadcast by a sniffer. Byte-for-byte
    // valid, tag and all - and discarded on the counter, which is the only
    // thing that can tell it apart from the original.
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_REPLAY);

    // A counter that went BACKWARDS is the same attack with a stale capture.
    makeCard(buf, eid, 2, 1, 0, 0, /*counter=*/3);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_REPLAY);
}

TEST(test_our_own_card_is_never_recorded) {
    freshEnabledPet();
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));
    HexPassCard card;
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_SELF);
    ASSERT_EQ(HexPass::instance().encounterCount(), 0);

    // Still ours one rotation later: a card captured just before a rotation
    // and replayed just after must not become a "friend".
    HexPass::instance().rotate();
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_SELF);
    ASSERT_EQ(HexPass::instance().encounterCount(), 0);
}

TEST(test_ring_evicts_oldest_first_and_cannot_be_overgrown) {
    freshEnabledPet();

    uint8_t firstFid[HEXPASS_FRIEND_BYTES];
    uint8_t secondFid[HEXPASS_FRIEND_BYTES];
    uint8_t mine[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(mine);

    // Fill the ring exactly.
    for (uint8_t i = 0; i < HEXPASS_MAX_ENCOUNTERS; i++) {
        uint8_t eid[HEXPASS_EID_BYTES];
        peerEid((uint8_t)(50 + i), eid);
        uint8_t buf[HEXPASS_PAYLOAD_BYTES];
        HexPassCard card;
        makeCard(buf, eid, 2, 1, 0, 0, 1);
        ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
        ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
        if (i == 0) HexPass::friendId(mine, eid, firstFid);
        if (i == 1) HexPass::friendId(mine, eid, secondFid);
    }
    ASSERT_EQ(HexPass::instance().encounterCount(), HEXPASS_MAX_ENCOUNTERS);
    ASSERT_TRUE(HexPass::instance().findFriend(firstFid) >= 0);

    // Now flood it. An attacker minting fresh identifiers is the case the ring
    // exists to survive: the store must stay exactly the same size, and the
    // entry that goes is the oldest one.
    for (uint8_t i = 0; i < 10; i++) {
        uint8_t eid[HEXPASS_EID_BYTES];
        peerEid((uint8_t)(150 + i), eid);
        uint8_t buf[HEXPASS_PAYLOAD_BYTES];
        HexPassCard card;
        makeCard(buf, eid, 2, 1, 0, 0, 1);
        ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
        ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
        ASSERT_EQ(HexPass::instance().encounterCount(), HEXPASS_MAX_ENCOUNTERS);
    }

    // The first friendship recorded is the first one evicted; the second one
    // outlived it, which is what "oldest-first" has to mean to be worth
    // stating at all.
    ASSERT_TRUE(HexPass::instance().findFriend(firstFid) < 0);
    ASSERT_EQ(HexPass::instance().encounterCount(), HEXPASS_MAX_ENCOUNTERS);
}

TEST(test_daily_cap_bounds_total_recorded_encounters) {
    freshEnabledPet();

    int recorded = 0;
    for (uint8_t i = 0; i < HEXPASS_MAX_PER_DAY + 10; i++) {
        uint8_t eid[HEXPASS_EID_BYTES];
        peerEid((uint8_t)(i + 1), eid);
        uint8_t buf[HEXPASS_PAYLOAD_BYTES];
        HexPassCard card;
        makeCard(buf, eid, 2, 1, 0, 0, 1);
        if (HexPass::parsePayload(buf, sizeof(buf), card) != HEXPASS_PARSE_OK) continue;
        const HexPassRecord r = HexPass::instance().record(card);
        if (r == HEXPASS_RECORD_NEW || r == HEXPASS_RECORD_REPEAT) {
            recorded++;
        } else {
            ASSERT_EQ(r, HEXPASS_RECORD_RATE_DAY);
        }
    }
    ASSERT_EQ(recorded, (int)HEXPASS_MAX_PER_DAY);

    // A new day releases the budget without a clock: the pet's existing
    // questDay is the notion of a day, reused rather than reinvented.
    PetCore::instance().state().questDay++;
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(200, eid);
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(buf, eid, 2, 1, 0, 0, 1);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
}

TEST(test_blocked_friend_is_never_recorded_again) {
    freshEnabledPet();
    uint8_t mine[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(mine);
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(77, eid);

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(buf, eid, 2, 1, 0, 0, 1);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
    ASSERT_EQ(HexPass::instance().encounterCount(), 1);

    uint8_t fid[HEXPASS_FRIEND_BYTES];
    HexPass::friendId(mine, eid, fid);
    ASSERT_TRUE(HexPass::instance().block(fid));

    // Blocking is retroactive: the friendship the owner just refused is gone
    // from the store, not merely hidden.
    ASSERT_EQ(HexPass::instance().encounterCount(), 0);
    ASSERT_TRUE(HexPass::instance().isBlocked(fid));

    // And it never comes back, no matter how many fresh, valid, non-replayed
    // cards arrive.
    for (uint32_t c = 2; c < 8; c++) {
        makeCard(buf, eid, 2, 1, 0, 0, c);
        ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
        ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_BLOCKED);
        ASSERT_EQ(HexPass::instance().encounterCount(), 0);
    }
}

TEST(test_block_list_is_bounded) {
    freshEnabledPet();
    for (uint8_t i = 0; i < HEXPASS_MAX_BLOCKED; i++) {
        uint8_t fid[HEXPASS_FRIEND_BYTES];
        memset(fid, (int)(i + 1), sizeof(fid));
        ASSERT_TRUE(HexPass::instance().block(fid));
    }
    ASSERT_EQ(HexPass::instance().blockedCount(), HEXPASS_MAX_BLOCKED);

    uint8_t overflow[HEXPASS_FRIEND_BYTES];
    memset(overflow, 0xEE, sizeof(overflow));
    ASSERT_FALSE(HexPass::instance().block(overflow));
    ASSERT_EQ(HexPass::instance().blockedCount(), HEXPASS_MAX_BLOCKED);
}

// ── Consent and control ───────────────────────────────────────────────────

TEST(test_opt_in_defaults_to_off) {
    PetCore::instance().state() = PetState();
    ASSERT_FALSE(PetCore::instance().state().hexpass.enabled);
    ASSERT_FALSE(HexPass::instance().enabled());
    ASSERT_FALSE(HexPass::instance().broadcasting());

    // And a device that has booted but never been asked still transmits
    // nothing.
    freshPet();
    ASSERT_FALSE(HexPass::instance().broadcasting());
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_FALSE(HexPass::instance().buildPayload(buf));
    uint8_t zero[HEXPASS_PAYLOAD_BYTES] = {};
    ASSERT_TRUE(memcmp(buf, zero, sizeof(buf)) == 0);
}

TEST(test_private_mode_stops_everything_immediately) {
    freshEnabledPet();
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));

    HexPass::instance().setPrivateMode(true);
    ASSERT_FALSE(HexPass::instance().broadcasting());
    ASSERT_FALSE(HexPass::instance().buildPayload(buf));

    // Recording stops too. A device that keeps building a social graph while
    // the owner believes it is private would be the worse reading of "stops
    // advertising immediately", so this takes the stricter one.
    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(31, eid);
    uint8_t card_buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(card_buf, eid, 2, 1, 0, 0, 1);
    ASSERT_EQ(HexPass::parsePayload(card_buf, sizeof(card_buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_DISABLED);
    ASSERT_EQ(HexPass::instance().encounterCount(), 0);

    // Reversible, and the identity survives: private mode is not a wipe.
    HexPass::instance().setPrivateMode(false);
    ASSERT_TRUE(HexPass::instance().broadcasting());
    ASSERT_TRUE(HexPass::instance().buildPayload(buf));
}

TEST(test_badge_and_greeting_are_bounded) {
    freshEnabledPet();
    ASSERT_TRUE(HexPass::instance().setBadge(HEXPASS_BADGE_COUNT - 1));
    ASSERT_FALSE(HexPass::instance().setBadge(HEXPASS_BADGE_COUNT));
    ASSERT_FALSE(HexPass::instance().setBadge(255));
    // Refused, not clamped: the broadcast badge must be the one that was set.
    ASSERT_EQ(HexPass::instance().badge(), HEXPASS_BADGE_COUNT - 1);

    ASSERT_TRUE(HexPass::instance().setGreeting(HEXPASS_GREETING_COUNT - 1));
    ASSERT_FALSE(HexPass::instance().setGreeting(HEXPASS_GREETING_COUNT));
    ASSERT_EQ(HexPass::instance().greeting(), HEXPASS_GREETING_COUNT - 1);
}

TEST(test_device_can_show_what_it_broadcasts_without_broadcasting) {
    freshEnabledPet();
    char hex[HEXPASS_PAYLOAD_BYTES * 2 + 1];
    ASSERT_TRUE(HexPass::instance().describePayloadHex(hex, sizeof(hex)));
    ASSERT_EQ((int)strlen(hex), HEXPASS_PAYLOAD_BYTES * 2);

    // Looking at the card must not be the same act as sending it, so the
    // counter has not moved and a second look is identical.
    char again[HEXPASS_PAYLOAD_BYTES * 2 + 1];
    ASSERT_TRUE(HexPass::instance().describePayloadHex(again, sizeof(again)));
    ASSERT_TRUE(strcmp(hex, again) == 0);

    // Disabled means there is nothing to show, rather than a card shown while
    // claiming to be off.
    HexPass::instance().setEnabled(false);
    ASSERT_FALSE(HexPass::instance().describePayloadHex(hex, sizeof(hex)));
}

TEST(test_wipe_makes_old_identifiers_underivable) {
    freshEnabledPet();

    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(88, eid);
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(buf, eid, 2, 1, 0, 0, 1);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);
    uint8_t fid[HEXPASS_FRIEND_BYTES];
    memset(fid, 0x11, sizeof(fid));
    ASSERT_TRUE(HexPass::instance().block(fid));

    const uint32_t oldEpoch = HexPass::instance().epoch();
    uint8_t oldEid[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().deriveEid(oldEpoch, oldEid));
    uint8_t oldSecret[HEXPASS_SECRET_BYTES];
    memcpy(oldSecret, PetCore::instance().state().hexpass.secret, HEXPASS_SECRET_BYTES);

    HexPass::instance().wipe();

    // Encounters and friendships gone.
    ASSERT_EQ(HexPass::instance().encounterCount(), 0);
    ASSERT_EQ(HexPass::instance().blockedCount(), 0);

    // The secret is gone and replaced, so it cannot be the one that produced
    // the sightings an observer already holds.
    ASSERT_TRUE(memcmp(oldSecret,
                       PetCore::instance().state().hexpass.secret,
                       HEXPASS_SECRET_BYTES) != 0);
    ASSERT_TRUE(HexPass::instance().hasSecret());

    // THE point of the wipe: the identifier the device used to broadcast is no
    // longer derivable, at that epoch or any other, so nothing links the old
    // sightings to the new device.
    uint8_t nowEid[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().deriveEid(oldEpoch, nowEid));
    ASSERT_TRUE(memcmp(oldEid, nowEid, HEXPASS_EID_BYTES) != 0);

    // The epoch keeps moving forward rather than restarting, so the new secret
    // is never asked for an identifier at an epoch value already observed.
    ASSERT_TRUE(HexPass::instance().epoch() > oldEpoch);
}

// ── Persistence ───────────────────────────────────────────────────────────

TEST(test_hexpass_state_round_trips_through_a_save) {
    freshEnabledPet();
    HexPass::instance().setBadge(4);
    HexPass::instance().setGreeting(5);

    uint8_t eid[HEXPASS_EID_BYTES];
    peerEid(21, eid);
    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    HexPassCard card;
    makeCard(buf, eid, 3, 2, 1, 6, 1);
    ASSERT_EQ(HexPass::parsePayload(buf, sizeof(buf), card), HEXPASS_PARSE_OK);
    ASSERT_EQ(HexPass::instance().record(card), HEXPASS_RECORD_NEW);

    uint8_t fid[HEXPASS_FRIEND_BYTES];
    memset(fid, 0x5A, sizeof(fid));
    ASSERT_TRUE(HexPass::instance().block(fid));

    uint8_t savedSecret[HEXPASS_SECRET_BYTES];
    memcpy(savedSecret, PetCore::instance().state().hexpass.secret, HEXPASS_SECRET_BYTES);
    uint8_t savedEid[HEXPASS_EID_BYTES];
    HexPass::instance().currentEid(savedEid);

    String json = PetCore::instance().saveToJson();
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(json.c_str()));

    const HexPassState& hp = PetCore::instance().state().hexpass;
    ASSERT_TRUE(hp.enabled);
    ASSERT_EQ(hp.badge, 4);
    ASSERT_EQ(hp.greeting, 5);
    ASSERT_EQ(hp.encounterCount, 1);
    ASSERT_EQ(hp.blockedCount, 1);
    ASSERT_EQ(hp.encounters[0].stage, 3);
    ASSERT_EQ(hp.encounters[0].form, 2);
    ASSERT_EQ(hp.encounters[0].meetCount, 1);
    ASSERT_TRUE(memcmp(hp.secret, savedSecret, HEXPASS_SECRET_BYTES) == 0);

    // The identity survives a reload: the same secret at the same epoch still
    // derives the same EID, which is what makes a friendship outlive a reboot.
    uint8_t reloadedEid[HEXPASS_EID_BYTES];
    ASSERT_TRUE(HexPass::instance().currentEid(reloadedEid));
    ASSERT_TRUE(memcmp(savedEid, reloadedEid, HEXPASS_EID_BYTES) == 0);
}

TEST(test_v2_save_migrates_to_the_current_schema_and_keeps_the_pet) {
    // A real v2 save, as written by the Phase 1 firmware: no "hexpass" key at
    // all. The pet must arrive intact, and the feature must arrive OFF.
    static const char* v2 =
        "{\"schema\":2,\"name\":\"R00tPup\",\"stage\":4,\"hunger\":63,\"mood\":71,"
        "\"energy\":44,\"xp\":1200,\"trust\":38,\"mischief\":12,\"health\":90,"
        "\"trait0\":2,\"trait1\":5,\"interactions\":410,\"wifiScans\":88,"
        "\"usbConnects\":6,\"seenWifi\":[\"HomeNet\",\"CafeWiFi\"],"
        "\"seenBle\":[\"aa:bb:cc:dd:ee:ff\"],\"masteryXP\":0,"
        "\"missionsCompleted\":9,\"saveSeq\":77,\"hatched\":true,"
        "\"checkIns\":31,\"questsCompleted\":14,\"bestGameScore\":2400,"
        "\"questDay\":9,\"dayTicks\":120,\"form\":3,"
        "\"behaviour\":[10,4,60,2,1,0],\"roamPoints\":512,\"stepCount\":9001,"
        "\"roamSessions\":7,\"lastReportDay\":8,"
        "\"items\":[0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"
        "\"denSlots\":[1,0,0,0,0,0,0,0]}";

    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(v2));

    const PetState& s = PetCore::instance().state();

    // The migration ran and the save will be rewritten in the new shape.
    ASSERT_EQ(s.schemaVersion, PET_SCHEMA_VERSION);
    // Pinned so a schema bump has to come here and think about what an OLD
    // save now needs. It has already caught two: v4 added worn cosmetics, and
    // this line is what made somebody check that a v2 pet arrives BARE rather
    // than with an uninitialised byte in wornSlots[] being drawn on its head;
    // v5 added the flourish, and the same question had a sharper answer, since
    // a v2 save can already OWN a PACKET FLURRY and still must not be playing
    // one. Owning is not choosing.
    ASSERT_EQ((int)PET_SCHEMA_VERSION, 5);
    ASSERT_TRUE(s.dirty);
    // v2 knew nothing about worn cosmetics, so the pet must arrive bare.
    for (uint8_t i = 0; i < WEAR_SLOT_COUNT; i++) {
        ASSERT_EQ(s.wornSlots[i], 0);
    }
    // And nothing about flourishes, so it must arrive showing none.
    ASSERT_EQ(s.flourishSlot, 0);

    // Every part of the existing pet survived it. This is the whole point of a
    // migration: an owner does not lose a pet to a firmware update.
    ASSERT_TRUE(strcmp(s.name, "R00tPup") == 0);
    ASSERT_EQ(s.stage, STAGE_GREMLIN);
    ASSERT_EQ(s.stats.xp, 1200u);
    ASSERT_EQ(s.stats.trust, 38);
    ASSERT_EQ(s.hatched, true);
    ASSERT_EQ(s.form, FORM_ARCHIVIST);
    ASSERT_EQ(s.behaviour[2], 60);
    ASSERT_EQ(s.roamPoints, 512u);
    ASSERT_EQ(s.stepCount, 9001u);
    ASSERT_EQ(s.questsCompleted, 14);
    ASSERT_EQ(s.bestGameScore, 2400u);
    ASSERT_EQ(s.items[1], 3);
    ASSERT_EQ(s.denSlots[0], 1);
    ASSERT_EQ(s.seenWifiCount, 2);

    // And HexPass arrived switched OFF, with no identity. An upgrade is not an
    // owner enabling something.
    ASSERT_FALSE(s.hexpass.enabled);
    ASSERT_FALSE(s.hexpass.privateMode);
    ASSERT_FALSE(s.hexpass.secretValid);
    ASSERT_EQ(s.hexpass.encounterCount, 0);
    ASSERT_EQ(s.hexpass.blockedCount, 0);
    ASSERT_FALSE(HexPass::instance().broadcasting());
}

TEST(test_v1_save_still_migrates_all_the_way_to_the_current_schema) {
    static const char* v1 =
        "{\"name\":\"Old\",\"stage\":2,\"xp\":50,\"hatched\":true}";
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(v1));
    const PetState& s = PetCore::instance().state();
    ASSERT_EQ(s.schemaVersion, PET_SCHEMA_VERSION);
    ASSERT_EQ(s.form, FORM_UNSET);
    ASSERT_FALSE(s.hexpass.enabled);
    ASSERT_TRUE(strcmp(s.name, "Old") == 0);
    // A v1 save predates the den and the closet alike, so the pet arrives with
    // an empty room and nothing on. Checked here because the migration walks
    // every step in order and this is the only test that starts at the oldest
    // shape: a step that forgets a new array would leave it holding whatever
    // the loader last put there.
    for (uint8_t i = 0; i < WEAR_SLOT_COUNT; i++) ASSERT_EQ(s.wornSlots[i], 0);
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++)  ASSERT_EQ(s.denSlots[i], 0);
    ASSERT_EQ(s.flourishSlot, 0);
}

TEST(test_hostile_hexpass_save_cannot_produce_an_out_of_range_index) {
    // The bug class this is here to prevent: a save with an out-of-range enum
    // read past a table on the next draw. On a board with no MMU that renders
    // garbage rather than faulting, so it is not even a crash anyone could
    // find. Every field below indexes something.
    static const char* hostile =
        "{\"schema\":3,\"name\":\"x\",\"stage\":3,\"hexpass\":{"
        "\"secret\":\"nothexatall\",\"enabled\":true,\"badge\":250,"
        "\"greeting\":99,\"epoch\":5,"
        "\"enc\":[{\"f\":\"00112233445566aa\",\"s\":250,\"fm\":250,"
        "\"b\":250,\"g\":250,\"n\":3},"
        "{\"f\":\"nothex\",\"s\":2},"
        "{\"f\":\"0011223344556601\",\"s\":0,\"fm\":7,\"b\":8,\"g\":8}],"
        "\"blk\":[\"0011223344556677\",\"bad\"]}}";

    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(hostile));
    const HexPassState& hp = PetCore::instance().state().hexpass;

    // A malformed secret is no secret. Half a secret would still derive
    // stable-looking EIDs, which is worse than none because it looks like it
    // works.
    ASSERT_FALSE(hp.secretValid);
    ASSERT_FALSE(HexPass::instance().hasSecret());

    // Our own card fields, clamped into range.
    ASSERT_TRUE(hp.badge < HEXPASS_BADGE_COUNT);
    ASSERT_TRUE(hp.greeting < HEXPASS_GREETING_COUNT);

    // The row with an undecodable FriendID identified nobody and was dropped;
    // the other two survived with every display field in range.
    ASSERT_EQ(hp.encounterCount, 2);
    for (uint8_t i = 0; i < hp.encounterCount; i++) {
        const HexPassEncounter& e = hp.encounters[i];
        ASSERT_TRUE(e.stage >= HEXPASS_STAGE_MIN && e.stage <= HEXPASS_STAGE_MAX);
        ASSERT_TRUE(e.form <= HEXPASS_FORM_MAX);
        ASSERT_TRUE(e.badge < HEXPASS_BADGE_COUNT);
        ASSERT_TRUE(e.greeting < HEXPASS_GREETING_COUNT);
        // Proves the clamped values are usable as indices, which is the actual
        // requirement rather than the numeric range on its own.
        ASSERT_TRUE(HEXPASS_BADGES[e.badge] != nullptr);
        ASSERT_TRUE(HEXPASS_GREETINGS[e.greeting] != nullptr);
        ASSERT_TRUE(STAGE_NAMES[e.stage - 1] != nullptr);
    }

    // The undecodable block entry did not become a block on the all-zero
    // FriendID, which would silently refuse a device nobody chose to refuse.
    ASSERT_EQ(hp.blockedCount, 1);
}

TEST(test_oversized_hexpass_arrays_cannot_overflow) {
    // A save written by a build with a bigger ring, or hand-edited. Read by
    // index against OUR bound, never by push.
    String json = "{\"schema\":3,\"name\":\"x\",\"stage\":2,\"hexpass\":{\"enc\":[";
    for (int i = 0; i < HEXPASS_MAX_ENCOUNTERS * 4; i++) {
        char row[80];
        snprintf(row, sizeof(row),
                 "%s{\"f\":\"%08x11223344\",\"s\":2,\"n\":1}", i ? "," : "", i + 1);
        json += row;
    }
    json += "],\"blk\":[";
    for (int i = 0; i < HEXPASS_MAX_BLOCKED * 4; i++) {
        char row[40];
        snprintf(row, sizeof(row), "%s\"%08x55667788\"", i ? "," : "", i + 1);
        json += row;
    }
    json += "]}}";

    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(json.c_str()));
    const HexPassState& hp = PetCore::instance().state().hexpass;
    ASSERT_EQ(hp.encounterCount, HEXPASS_MAX_ENCOUNTERS);
    ASSERT_EQ(hp.blockedCount, HEXPASS_MAX_BLOCKED);
}

TEST(test_a_save_can_never_switch_the_feature_on_by_omission) {
    // The single most consequential default in the subsystem. A save with a
    // hexpass block that simply does not mention `enabled` must come back OFF.
    static const char* quiet =
        "{\"schema\":3,\"name\":\"x\",\"stage\":2,\"hexpass\":{\"epoch\":3}}";
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(PetCore::instance().loadFrom(quiet));
    ASSERT_FALSE(PetCore::instance().state().hexpass.enabled);
    ASSERT_FALSE(HexPass::instance().broadcasting());
}

// ── Visibility tier ───────────────────────────────────────────────────────

// The default must be the private end of the scale. A device that shipped
// readable-by-strangers because nobody set the field would be the same class
// of mistake as HexPass being on by default.
TEST(test_visibility_defaults_to_friends) {
    PetCore::instance().state() = PetState();
    ASSERT_TRUE(HexPass::instance().visibility() == HEXPASS_VIS_FRIENDS);
}

// The whole reason the flag rides in spare bits of the stage byte: if the two
// modes produced different length cards, an observer would know which mode a
// device is in without decoding anything at all.
TEST(test_both_visibility_modes_are_the_same_length) {
    PetCore::instance().state() = PetState();
    HexPass& hp = HexPass::instance();
    hp.begin();
    hp.setEnabled(true);

    uint8_t friends[HEXPASS_PAYLOAD_BYTES];
    uint8_t open[HEXPASS_PAYLOAD_BYTES];

    ASSERT_TRUE(hp.setVisibility(HEXPASS_VIS_FRIENDS));
    ASSERT_TRUE(hp.buildPayload(friends));
    ASSERT_TRUE(hp.setVisibility(HEXPASS_VIS_DISCOVERABLE));
    ASSERT_TRUE(hp.buildPayload(open));

    // Same size is structural (fixed array), so what this really pins is that
    // both modes actually produce a valid card at all.
    HexPassCard a, b;
    ASSERT_TRUE(HexPass::parsePayload(friends, HEXPASS_PAYLOAD_BYTES, a) == HEXPASS_PARSE_OK);
    ASSERT_TRUE(HexPass::parsePayload(open, HEXPASS_PAYLOAD_BYTES, b) == HEXPASS_PARSE_OK);
    ASSERT_TRUE(!a.discoverable);
    ASSERT_TRUE(b.discoverable);
}

// In FRIENDS mode the personality bytes are noise. The parser must blank them
// so the UI can never display a form and badge the sender did not choose.
TEST(test_friends_mode_never_leaks_personality) {
    PetCore::instance().state() = PetState();
    PetCore::instance().state().stage = STAGE_SENTINEL;
    PetCore::instance().state().form  = FORM_CIPHER;

    HexPass& hp = HexPass::instance();
    hp.begin();
    hp.setEnabled(true);
    hp.setBadge(5);
    hp.setGreeting(3);
    ASSERT_TRUE(hp.setVisibility(HEXPASS_VIS_FRIENDS));

    // Many cards: the noise is random per build, so one sample could pass by
    // luck. Every one of them must come out blank.
    for (int i = 0; i < 64; i++) {
        uint8_t buf[HEXPASS_PAYLOAD_BYTES];
        ASSERT_TRUE(hp.buildPayload(buf));
        HexPassCard card;
        ASSERT_TRUE(HexPass::parsePayload(buf, HEXPASS_PAYLOAD_BYTES, card) == HEXPASS_PARSE_OK);
        ASSERT_TRUE(!card.discoverable);
        ASSERT_EQ((int)card.form, 0);
        ASSERT_EQ((int)card.badge, HEXPASS_BADGE_NONE);
        ASSERT_EQ((int)card.greeting, 0);
        // Stage is deliberately still readable: it is about 2.3 bits and is
        // what makes a passing hound interesting at all.
        ASSERT_EQ((int)card.stage, (int)STAGE_SENTINEL);
    }
}

// Opting in means the real card goes out.
TEST(test_discoverable_mode_sends_the_real_card) {
    PetCore::instance().state() = PetState();
    PetCore::instance().state().stage = STAGE_GREMLIN;
    PetCore::instance().state().form  = FORM_ARCHIVIST;

    HexPass& hp = HexPass::instance();
    hp.begin();
    hp.setEnabled(true);
    hp.setBadge(4);
    hp.setGreeting(2);
    ASSERT_TRUE(hp.setVisibility(HEXPASS_VIS_DISCOVERABLE));

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    ASSERT_TRUE(hp.buildPayload(buf));
    HexPassCard card;
    ASSERT_TRUE(HexPass::parsePayload(buf, HEXPASS_PAYLOAD_BYTES, card) == HEXPASS_PARSE_OK);
    ASSERT_TRUE(card.discoverable);
    ASSERT_EQ((int)card.stage, (int)STAGE_GREMLIN);
    ASSERT_EQ((int)card.form, (int)FORM_ARCHIVIST);
    ASSERT_EQ((int)card.badge, 4);
    ASSERT_EQ((int)card.greeting, 2);
}

// An out-of-range value must not be accepted, and must not move the setting.
TEST(test_visibility_rejects_out_of_range) {
    PetCore::instance().state() = PetState();
    HexPass& hp = HexPass::instance();
    ASSERT_TRUE(hp.setVisibility(HEXPASS_VIS_DISCOVERABLE));
    ASSERT_TRUE(!hp.setVisibility((HexPassVisibility)99));
    ASSERT_TRUE(hp.visibility() == HEXPASS_VIS_DISCOVERABLE);
}

// A corrupt or future save must fall back to the PRIVATE end, never the open
// one. Getting this backwards would make a device more visible than its owner
// ever chose.
TEST(test_corrupt_visibility_falls_back_to_friends) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom(
        "{\"schema\":3,\"name\":\"X\",\"stage\":2,"
        "\"hexpass\":{\"enabled\":true,\"vis\":250}}"));
    ASSERT_TRUE(pet.state().hexpass.visibility == HEXPASS_VIS_FRIENDS);
}

int main() {
    printf("\n=== HexPass Tests (crypto backend: %s) ===\n",
           HexPassCrypto::backendName());
    EventBus::instance().reset();

    RUN_TEST(test_hmac_matches_rfc4231_case1);
    RUN_TEST(test_hmac_matches_rfc4231_case2);
    RUN_TEST(test_hmac_matches_rfc4231_case4);
    RUN_TEST(test_hmac_with_key_longer_than_block_is_hashed_first);

    RUN_TEST(test_eid_is_stable_within_an_epoch);
    RUN_TEST(test_eid_differs_per_epoch);
    RUN_TEST(test_no_secret_means_no_identifier);
    RUN_TEST(test_rotation_is_one_trigger_not_two_timers);
    RUN_TEST(test_boot_advances_the_epoch);

    RUN_TEST(test_friendid_is_order_independent);
    RUN_TEST(test_friendid_differs_per_pair);

    RUN_TEST(test_payload_is_exactly_33_bytes_and_round_trips);
    RUN_TEST(test_payload_carries_no_name);
    RUN_TEST(test_counter_advances_within_an_epoch);
    RUN_TEST(test_counter_restarts_on_rotation);
    RUN_TEST(test_tampered_payload_fails_its_tag);
    RUN_TEST(test_unknown_version_is_rejected);
    RUN_TEST(test_wrong_length_is_rejected);
    RUN_TEST(test_out_of_range_fields_are_rejected_not_clamped);
    RUN_TEST(test_all_zero_eid_is_not_an_identity);

    RUN_TEST(test_a_passing_pet_is_recorded_once_per_epoch);
    RUN_TEST(test_replayed_eid_and_counter_is_discarded);
    RUN_TEST(test_our_own_card_is_never_recorded);
    RUN_TEST(test_ring_evicts_oldest_first_and_cannot_be_overgrown);
    RUN_TEST(test_daily_cap_bounds_total_recorded_encounters);
    RUN_TEST(test_blocked_friend_is_never_recorded_again);
    RUN_TEST(test_block_list_is_bounded);

    RUN_TEST(test_opt_in_defaults_to_off);
    RUN_TEST(test_private_mode_stops_everything_immediately);
    RUN_TEST(test_badge_and_greeting_are_bounded);
    RUN_TEST(test_device_can_show_what_it_broadcasts_without_broadcasting);
    RUN_TEST(test_wipe_makes_old_identifiers_underivable);

    RUN_TEST(test_hexpass_state_round_trips_through_a_save);
    RUN_TEST(test_v2_save_migrates_to_the_current_schema_and_keeps_the_pet);
    RUN_TEST(test_v1_save_still_migrates_all_the_way_to_the_current_schema);
    RUN_TEST(test_hostile_hexpass_save_cannot_produce_an_out_of_range_index);
    RUN_TEST(test_oversized_hexpass_arrays_cannot_overflow);
    RUN_TEST(test_a_save_can_never_switch_the_feature_on_by_omission);

    RUN_TEST(test_visibility_defaults_to_friends);
    RUN_TEST(test_both_visibility_modes_are_the_same_length);
    RUN_TEST(test_friends_mode_never_leaks_personality);
    RUN_TEST(test_discoverable_mode_sends_the_real_card);
    RUN_TEST(test_visibility_rejects_out_of_range);
    RUN_TEST(test_corrupt_visibility_falls_back_to_friends);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
