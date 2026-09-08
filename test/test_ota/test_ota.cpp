// ── HexHound - OTA Update Engine Unit Tests ─────────────────────
//
// Covers P3-W2: the signed firmware envelope, the session state machine, and
// the portable half of the post-boot health policy.
//
// The organising rule for this file, the OTA analogue of "the pet can still
// speak": EVERY refusal is followed by an assertion that the device is still
// booting what it was booting. Concretely that means the target was never
// begun (for a header refusal), the boot partition was never set, and the
// session is not sitting in FAILED while reporting ACCEPTED.
//
// The property this suite exists to pin down is stated in docs/p3w2-wiring.md
// as "verify before WRITE, not verify before commit". A test that only checked
// bootPartitionSet() would pass on a session that erased the passive slot for
// an unsigned image and then declined to boot it. That is not the claim being
// made, so it is not the claim being tested.

#include "../test_stubs.h"

#include "fixtures.h"

#include "../../src/content/content_crypto.cpp"
#include "../../src/ota/ota_image.cpp"
#include "../../src/ota/ota_session.cpp"
#include "../../src/ota/ota_health.cpp"

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != nullptr)

// ── Helpers ───────────────────────────────────────────────────────────────

using OtaImage::Verdict;

static OtaImage::KeyRing ringOf(const uint8_t (*ids)[4],
                                const uint8_t (*keys)[32],
                                size_t count) {
    OtaImage::KeyRing r;
    r.ids   = ids;
    r.keys  = keys;
    r.count = count;
    return r;
}

// ── The trust anchor, handed in through the seam ──────────────────────────
//
// OtaImage::checkSignature(), OtaImage::accept() and OtaSession all take a
// KeyRing defaulting to firmwareKeys(). So this suite substitutes a fixture
// key the same way any caller would, with an ordinary function argument, and
// ota_pubkey.h is left exactly as the firmware sees it.
//
// It used to redefine OTA_TRUSTED_KEYS and OTA_TRUSTED_KEY_IDS with the
// preprocessor before including ota_image.cpp, which worked but meant the
// firmware's real trust anchor was not the one under test and would have
// stopped matching the code the day firmwareKeys() was built differently.
static const uint8_t (*const kFixtureKeyIds)[4] =
    (const uint8_t (*)[4])FIX_OTA_TEST_KEYID;
static const uint8_t (*const kFixtureKeys)[32] =
    (const uint8_t (*)[32])FIX_OTA_TEST_PUBKEY;
static const uint8_t (*const kOtherKeyIds)[4] =
    (const uint8_t (*)[4])FIX_OTA_OTHER_KEYID;
static const uint8_t (*const kOtherKeys)[32] =
    (const uint8_t (*)[32])FIX_OTA_OTHER_PUBKEY;

// Static storage, because a session holds its ring for as long as it lives.
// It holds it BY VALUE, so this would be safe either way, but a test that
// only works because of that is a test that breaks if it ever changes.
static const OtaImage::KeyRing kFixtureRing =
    ringOf(kFixtureKeyIds, kFixtureKeys, 1);
static const OtaImage::KeyRing kOtherRing =
    ringOf(kOtherKeyIds, kOtherKeys, 1);

// The slot the session tests install into. Deliberately far smaller than a real
// app partition: the only thing the session needs from it is a real bound, and
// FIX_OTA_HDR_TOO_LARGE overshoots any plausible slot by construction. The pure
// checkFit tests use FIX_OTA_SLOT_BYTES, which is the real app0 size.
//
// Big enough to hold FIX_OTA_BIG_IMAGE_LEN, which is the whole point of that
// fixture: a slot that could not take it would turn every multi-pass test into
// an IMAGE_TOO_LARGE refusal that never reached the loop under test.
#define SESSION_SLOT_BYTES 16384u

// ── The multi-pass body has to actually be multi-pass ─────────────────────
//
// Checked at COMPILE time against the firmware's real chunk size, because the
// failure mode being guarded is silent: raise HEXHOUND_OTA_CHUNK_MAX past
// FIX_OTA_BIG_IMAGE_LEN and finish()'s loop quietly goes back to one pass while
// every test in this file still passes. The generator makes the same two checks
// when it emits the fixture; this one is what catches the chunk size moving
// afterwards.
static_assert(FIX_OTA_BIG_IMAGE_LEN > 2u * (uint32_t)HEXHOUND_OTA_CHUNK_MAX,
              "the big fixture must span more than two read-back chunks");
static_assert(FIX_OTA_BIG_IMAGE_LEN % (uint32_t)HEXHOUND_OTA_CHUNK_MAX != 0u,
              "the big fixture must not divide evenly, or the short final "
              "read is never exercised");
static_assert(FIX_OTA_BIG_IMAGE_LEN <= SESSION_SLOT_BYTES,
              "the test slot must be able to hold the big fixture");
// The small bodies stay single-pass on purpose. That boundary is a case the
// loop has to get right too, and it is the one every other test here covers.
static_assert(FIX_OTA_IMAGE_LEN <= (uint32_t)HEXHOUND_OTA_CHUNK_MAX,
              "the small fixture is supposed to verify in a single pass");

static uint8_t s_slot[SESSION_SLOT_BYTES];

// ── The multi-pass body, rebuilt here from the documented rule ────────────
//
// FIX_OTA_BIG_IMAGE_LEN bytes of chained SHA-512, the same rule the generator
// used, because 10000 literal bytes would have added roughly 830 lines to
// fixtures.h that nobody reads.
//
//     block = SHA512(label); emit; block = SHA512(block); repeat
//
// This is a SECOND implementation of the generator's filler, and that is
// deliberate rather than merely convenient: the two have to agree byte for
// byte or the digest inside the signed header does not match, so the agreement
// is itself checked. It also cannot be used to hide a broken read-back loop,
// because anything this function produces still has to hash to a value sitting
// inside an Ed25519 signature.
static void fillBigImage(uint8_t* out, uint32_t count) {
    uint8_t block[64];
    ContentCrypto::sha512((const uint8_t*)FIX_OTA_BIG_IMAGE_LABEL,
                          strlen(FIX_OTA_BIG_IMAGE_LABEL),
                          nullptr, 0, nullptr, 0, block);
    uint32_t off = 0;
    while (off < count) {
        uint32_t n = count - off;
        if (n > sizeof(block)) n = (uint32_t)sizeof(block);
        memcpy(out + off, block, n);
        off += n;
        if (off < count) {
            uint8_t next[64];
            ContentCrypto::sha512(block, sizeof(block),
                                  nullptr, 0, nullptr, 0, next);
            memcpy(block, next, sizeof(block));
        }
    }
}

// Built once. A 10 KB static is nothing on a desktop and rebuilding it per
// test would spend a few hundred SHA-512 compressions for no reason.
static uint8_t s_bigImage[FIX_OTA_BIG_IMAGE_LEN];
static bool    s_bigImageReady = false;

static const uint8_t* bigImage() {
    if (!s_bigImageReady) {
        fillBigImage(s_bigImage, FIX_OTA_BIG_IMAGE_LEN);
        s_bigImageReady = true;
    }
    return s_bigImage;
}

// A fresh target and session for one test. The storage is scrubbed rather than
// merely reattached, because corrupt() writes through it and a leaked bit flip
// would make the next test lie.
struct SessionRig {
    MemoryOtaTarget target;
    OtaSession      session;

    SessionRig() : target(s_slot, SESSION_SLOT_BYTES) {
        memset(s_slot, 0, sizeof(s_slot));
        session.attach(&target);
        // Every fixture header below is signed by the fixture key, which is
        // deliberately NOT in ota_pubkey.h. Handed in the same way any caller
        // would; see kFixtureRing.
        session.setTrustedKeys(kFixtureRing);
    }
};

// ── The invariant that must never be forgotten ────────────────────────────
//
// A session in FAILED never reports the verdict ACCEPTED. This was broken once,
// which is why it lives in a helper and is asserted after every single failure
// path rather than being remembered case by case.
static bool failedIsHonest(const OtaSession& s) {
    if (s.state() != OtaSession::FAILED) return true;
    return s.verdict() != OtaImage::ACCEPTED;
}

// The other half of every refusal: the device is still booting what it booted.
static bool deviceUnchanged(const MemoryOtaTarget& t) {
    return !t.bootPartitionSet();
}

static bool armAndOffer(SessionRig& rig, const uint8_t* hdr, size_t len,
                        uint32_t nowMs, Verdict& out) {
    if (!rig.session.arm(nowMs, OTA_ARM_WINDOW_MS)) return false;
    out = rig.session.offerHeader(hdr, len, nowMs);
    return true;
}

// Streams a body in 500-byte chunks, which is not a divisor of 1536, so the
// last chunk is short and the accumulator has to be right about it.
static bool streamBody(SessionRig& rig, const uint8_t* body, uint32_t len,
                       uint32_t startMs) {
    uint32_t off = 0;
    uint32_t t   = startMs;
    while (off < len) {
        uint32_t n = len - off;
        if (n > 500) n = 500;
        if (!rig.session.offerChunk(body + off, n, t)) return false;
        off += n;
        t   += 10;
    }
    return true;
}

// ══ Envelope: the good case and the shape of the fixtures ═════════════════

TEST(test_board_id_matches_the_fixtures) {
    // If ota_identity.h ever moves the default board id, every fit check below
    // starts failing for a reason with nothing to do with OTA. This assertion
    // is what says so out loud instead.
    ASSERT_STREQ(FIX_OTA_BOARD_ID, HEXHOUND_OTA_BOARD_ID);
    ASSERT_EQ((uint32_t)FIX_OTA_SLOT_BYTES, (uint32_t)HEXHOUND_OTA_SLOT_BYTES);
    ASSERT_EQ((uint32_t)FIX_OTA_HEADER_BYTES, (uint32_t)OTA_HEADER_BYTES);
}

TEST(test_fixture_key_is_not_a_firmware_key) {
    // fixtures.h claims the test key is deliberately outside the firmware's
    // trusted list, which is the whole reason rotating keys/ota-dev-signing.key
    // cannot break this suite. Claims about a trust anchor are worth checking.
    //
    // This reads the REAL firmwareKeys() now. It could not before: the suite
    // used to redefine OTA_TRUSTED_KEYS out from under ota_image.cpp, so the
    // only way to see the genuine list was to save a pointer to it before
    // clobbering it, and the thing being asserted about was a copy the code
    // under test no longer used.
    const OtaImage::KeyRing& firmware = OtaImage::firmwareKeys();
    ASSERT_GT(firmware.count, (size_t)0);
    ASSERT_NOT_NULL(firmware.ids);
    ASSERT_NOT_NULL(firmware.keys);

    for (size_t i = 0; i < firmware.count; i++) {
        ASSERT_TRUE(memcmp(firmware.keys[i], FIX_OTA_TEST_PUBKEY, 32) != 0);
        ASSERT_TRUE(memcmp(firmware.keys[i], FIX_OTA_OTHER_PUBKEY, 32) != 0);
        ASSERT_TRUE(memcmp(firmware.ids[i], FIX_OTA_TEST_KEYID, 4) != 0);
        ASSERT_TRUE(memcmp(firmware.ids[i], FIX_OTA_OTHER_KEYID, 4) != 0);
    }

    // And the consequence, stated as behaviour rather than as a byte compare:
    // a session left on its DEFAULT ring refuses the good fixture image with
    // UNKNOWN_KEY. This is the assertion that would catch the fixture key
    // being added to ota_pubkey.h by accident.
    MemoryOtaTarget target(s_slot, SESSION_SLOT_BYTES);
    OtaSession defaultRing;
    defaultRing.attach(&target);
    ASSERT_TRUE(defaultRing.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(defaultRing.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::UNKNOWN_KEY);
    ASSERT_EQ(target.beginCalls(), 0u);
    ASSERT_FALSE(target.bootPartitionSet());
}

TEST(test_the_session_seam_defaults_to_the_firmware_keys) {
    // The seam must not change the device path. A freshly constructed session
    // reports exactly firmwareKeys(), and a session that never has the setter
    // called behaves exactly as it did before the seam existed.
    OtaSession fresh;
    const OtaImage::KeyRing& firmware = OtaImage::firmwareKeys();
    ASSERT_EQ(fresh.trustedKeys().count, firmware.count);
    ASSERT_TRUE(fresh.trustedKeys().ids  == firmware.ids);
    ASSERT_TRUE(fresh.trustedKeys().keys == firmware.keys);

    // The ring is configuration, not per-attempt state. arm(), a failure and
    // cancel() must all leave it alone: a trust anchor that quietly reverts
    // partway through a session is one nobody can reason about.
    SessionRig rig;
    ASSERT_TRUE(rig.session.trustedKeys().keys == kFixtureRing.keys);

    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_TRUE(rig.session.trustedKeys().keys == kFixtureRing.keys);

    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_BAD_MAGIC, OTA_HEADER_BYTES, 0),
              OtaImage::BAD_MAGIC);
    ASSERT_TRUE(rig.session.trustedKeys().keys == kFixtureRing.keys);

    ASSERT_TRUE(rig.session.cancel("done"));
    ASSERT_TRUE(rig.session.trustedKeys().keys == kFixtureRing.keys);

    // And it still installs afterwards, so the ring really did survive.
    ASSERT_TRUE(rig.session.arm(1000, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 1000),
              OtaImage::ACCEPTED);
}

TEST(test_valid_header_parses_fits_and_verifies) {
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, sizeof(FIX_OTA_HDR_OK), h),
              OtaImage::ACCEPTED);

    ASSERT_EQ(h.imageLen, (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_EQ(h.headerVersion, (uint16_t)OTA_HEADER_VERSION);
    ASSERT_EQ(h.flags, (uint16_t)0);
    ASSERT_EQ(h.fwVersion, (uint32_t)FIX_OTA_FW_VERSION);
    ASSERT_EQ(h.buildNumber, (uint32_t)FIX_OTA_BUILD_NUMBER);
    ASSERT_STREQ(h.boardId, FIX_OTA_BOARD_ID);

    // The signed prefix must be the bytes that actually arrived, not a
    // re-serialisation of the parsed fields.
    ASSERT_TRUE(memcmp(h.signedPrefix, FIX_OTA_HDR_OK,
                       OTA_SIGNED_PREFIX_BYTES) == 0);
    ASSERT_TRUE(memcmp(h.sig, FIX_OTA_HDR_OK + 128, OTA_SIG_BYTES) == 0);

    ASSERT_EQ(OtaImage::checkFit(h, FIX_OTA_SLOT_BYTES), OtaImage::ACCEPTED);

    const OtaImage::KeyRing& ring = kFixtureRing;
    ASSERT_EQ(OtaImage::checkSignature(h, ring), OtaImage::ACCEPTED);

    OtaImage::Header viaAccept;
    ASSERT_EQ(OtaImage::accept(FIX_OTA_HDR_OK, sizeof(FIX_OTA_HDR_OK),
                               FIX_OTA_SLOT_BYTES, viaAccept, ring),
              OtaImage::ACCEPTED);
}

TEST(test_short_and_null_headers_are_refused) {
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(nullptr, OTA_HEADER_BYTES, h), OtaImage::SHORT_HEADER);

    // Every prefix, not just a convenient one.
    for (size_t len = 0; len < OTA_HEADER_BYTES; len++) {
        if (OtaImage::parse(FIX_OTA_HDR_OK, len, h) != OtaImage::SHORT_HEADER) {
            printf("FAIL\n    a %u-byte header was not SHORT_HEADER\n", (unsigned)len);
            s_testsFailed++;
            return;
        }
    }
    // And one byte more than a header is fine: the image follows it.
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, h),
              OtaImage::ACCEPTED);
}

// ── One table for every corrupted fixture ─────────────────────────────────
//
// Each row is the verdict that fixture's own comment in fixtures.h claims. If
// the code and the comment ever disagree, this table is where it shows up.
struct HeaderCase {
    const char*    what;
    const uint8_t* hdr;
    Verdict        expect;
};

static const HeaderCase kHeaderCases[] = {
    { "bad magic",         FIX_OTA_HDR_BAD_MAGIC,        OtaImage::BAD_MAGIC },
    { "header version 2",  FIX_OTA_HDR_BAD_VERSION,      OtaImage::BAD_HEADER_VERSION },
    { "unknown flag bit",  FIX_OTA_HDR_BAD_FLAGS,        OtaImage::BAD_FLAGS },
    { "reserved not zero", FIX_OTA_HDR_BAD_RESERVED,     OtaImage::BAD_RESERVED },
    { "wrong board",       FIX_OTA_HDR_WRONG_BOARD,      OtaImage::BOARD_MISMATCH },
    { "oversized image",   FIX_OTA_HDR_TOO_LARGE,        OtaImage::IMAGE_TOO_LARGE },
    { "undersized image",  FIX_OTA_HDR_TOO_SMALL,        OtaImage::IMAGE_TOO_SMALL },
    { "unknown key id",    FIX_OTA_HDR_UNKNOWN_KEY,      OtaImage::UNKNOWN_KEY },
    { "forged key id",     FIX_OTA_HDR_FORGED_KEYID,     OtaImage::BAD_SIGNATURE },
    { "transplanted sig",  FIX_OTA_HDR_TRANSPLANTED_SIG, OtaImage::BAD_SIGNATURE },
    { "untagged sig",      FIX_OTA_HDR_UNTAGGED_SIG,     OtaImage::BAD_SIGNATURE },
};

TEST(test_every_corrupted_fixture_gets_the_verdict_it_claims) {
    const OtaImage::KeyRing& ring = kFixtureRing;

    for (size_t i = 0; i < sizeof(kHeaderCases) / sizeof(kHeaderCases[0]); i++) {
        OtaImage::Header h;
        const Verdict got = OtaImage::accept(kHeaderCases[i].hdr, OTA_HEADER_BYTES,
                                             FIX_OTA_SLOT_BYTES, h, ring);
        if (got != kHeaderCases[i].expect) {
            printf("FAIL\n    %s: got %s, fixture claims %s\n",
                   kHeaderCases[i].what, OtaImage::verdictName(got),
                   OtaImage::verdictName(kHeaderCases[i].expect));
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_board_mismatch_is_refused_on_its_own) {
    // Not through accept(), so it is clear this is checkFit's decision and not
    // a signature failure wearing a different name. The wrong-board fixture is
    // correctly signed; that is the point of it.
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_WRONG_BOARD, OTA_HEADER_BYTES, h),
              OtaImage::ACCEPTED);
    ASSERT_STREQ(h.boardId, FIX_OTA_OTHER_BOARD_ID);
    ASSERT_EQ(OtaImage::checkFit(h, FIX_OTA_SLOT_BYTES), OtaImage::BOARD_MISMATCH);

    // And the signature over it really is good, so nothing else could have
    // refused it. Five of seven targets are ESP32-S3; the board id is the only
    // thing standing between a T-Dongle image and a Waveshare panel.
    const OtaImage::KeyRing& ring = kFixtureRing;
    ASSERT_EQ(OtaImage::checkSignature(h, ring), OtaImage::ACCEPTED);
}

TEST(test_oversized_and_undersized_images_are_refused) {
    OtaImage::Header big;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_TOO_LARGE, OTA_HEADER_BYTES, big),
              OtaImage::ACCEPTED);
    ASSERT_EQ(big.imageLen, (uint32_t)FIX_OTA_SLOT_BYTES + 1u);
    ASSERT_EQ(OtaImage::checkFit(big, FIX_OTA_SLOT_BYTES), OtaImage::IMAGE_TOO_LARGE);

    // Exactly filling the slot is not too large. Off-by-one in the other
    // direction would refuse a legitimate full-size release.
    OtaImage::Header ok;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, ok),
              OtaImage::ACCEPTED);
    ASSERT_EQ(OtaImage::checkFit(ok, ok.imageLen), OtaImage::ACCEPTED);
    ASSERT_EQ(OtaImage::checkFit(ok, ok.imageLen - 1), OtaImage::IMAGE_TOO_LARGE);

    // A zero slot is refused as too large by checkFit on its own, which is why
    // the session asks the target first and answers NO_UPDATE_SLOT instead.
    ASSERT_EQ(OtaImage::checkFit(ok, 0), OtaImage::IMAGE_TOO_LARGE);

    OtaImage::Header small;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_TOO_SMALL, OTA_HEADER_BYTES, small),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(small.imageLen < (uint32_t)OTA_MIN_IMAGE_BYTES);
    ASSERT_EQ(OtaImage::checkFit(small, FIX_OTA_SLOT_BYTES), OtaImage::IMAGE_TOO_SMALL);

    // The board is checked before the size, so a wrong-board image never gets
    // told it is the wrong size.
    OtaImage::Header wrongBoard;
    OtaImage::parse(FIX_OTA_HDR_WRONG_BOARD, OTA_HEADER_BYTES, wrongBoard);
    ASSERT_EQ(OtaImage::checkFit(wrongBoard, 0), OtaImage::BOARD_MISMATCH);
}

TEST(test_unknown_key_and_bad_signature_are_different) {
    // The distinction that matters: an id naming no key this firmware holds is
    // a ROTATION problem the owner can be told about, and a signature that will
    // not verify under the key it named is an ATTACK or a corrupt file. Sending
    // an owner to chase the wrong one of those is the whole reason there are
    // two verdicts.
    const OtaImage::KeyRing& ring = kFixtureRing;

    OtaImage::Header unknown;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_UNKNOWN_KEY, OTA_HEADER_BYTES, unknown),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(memcmp(unknown.keyId, FIX_OTA_OTHER_KEYID, 4) == 0);
    ASSERT_EQ(OtaImage::checkSignature(unknown, ring), OtaImage::UNKNOWN_KEY);

    // Same image, offered to a verifier that DOES hold that key: now it
    // verifies. So UNKNOWN_KEY above really was about the ring and not about
    // the bytes.
    const OtaImage::KeyRing& otherRing = kOtherRing;
    ASSERT_EQ(OtaImage::checkSignature(unknown, otherRing), OtaImage::ACCEPTED);

    // And the forged id: signed by the other key, labelled with the test key's
    // id. The id is a selector, so the named key is tried and fails.
    OtaImage::Header forged;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_FORGED_KEYID, OTA_HEADER_BYTES, forged),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(memcmp(forged.keyId, FIX_OTA_TEST_KEYID, 4) == 0);
    ASSERT_EQ(OtaImage::checkSignature(forged, ring), OtaImage::BAD_SIGNATURE);

    // Forging the id buys nothing in either direction: relabelled with the
    // other key's id it verifies against a ring holding that key, which is the
    // proof that the signature, not the label, is what is doing the work.
    OtaImage::Header relabelled = forged;
    memcpy(relabelled.keyId, FIX_OTA_OTHER_KEYID, 4);
    memcpy(relabelled.signedPrefix + 56, FIX_OTA_OTHER_KEYID, 4);
    // The prefix just changed, so this must now FAIL under the other key too:
    // the key id is inside the signed region.
    ASSERT_EQ(OtaImage::checkSignature(relabelled, otherRing),
              OtaImage::BAD_SIGNATURE);
}

TEST(test_transplanted_and_untagged_signatures_are_refused) {
    const OtaImage::KeyRing& ring = kFixtureRing;

    // A real signature by the real key over a real header, spliced onto a
    // header that differs by one build number. Every byte is legitimate; only
    // the pairing is not.
    //
    // The fixture carries the ORIGINAL header, build 4242, and the signature
    // taken from the build-4243 one. So the signed prefix here is byte for
    // byte the good header, and the signature alone is the transplant. That is
    // the sharpest possible version of this case: nothing but the pairing is
    // wrong. (The generated comment in fixtures.h reads the other way round if
    // you take its parenthetical to describe the destination header.)
    OtaImage::Header moved;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_TRANSPLANTED_SIG, OTA_HEADER_BYTES, moved),
              OtaImage::ACCEPTED);
    ASSERT_EQ(moved.buildNumber, (uint32_t)FIX_OTA_BUILD_NUMBER);
    ASSERT_TRUE(memcmp(moved.signedPrefix, FIX_OTA_HDR_OK,
                       OTA_SIGNED_PREFIX_BYTES) == 0);
    ASSERT_TRUE(memcmp(moved.sig, FIX_OTA_HDR_OK + 128, OTA_SIG_BYTES) != 0);
    ASSERT_EQ(OtaImage::checkSignature(moved, ring), OtaImage::BAD_SIGNATURE);

    // The right key signing the right 128 bytes WITHOUT the domain tag. If this
    // verified, the tag would not really be in the signed message and any other
    // signing path over the same structure could be replayed as firmware.
    OtaImage::Header untagged;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_UNTAGGED_SIG, OTA_HEADER_BYTES, untagged),
              OtaImage::ACCEPTED);
    ASSERT_EQ(OtaImage::checkSignature(untagged, ring), OtaImage::BAD_SIGNATURE);

    // And the tag really is the only difference: the same 128 bytes verify
    // under the same key when the tag is left off the message by hand.
    ASSERT_TRUE(ContentCrypto::verify(untagged.signedPrefix,
                                      OTA_SIGNED_PREFIX_BYTES,
                                      untagged.sig, FIX_OTA_TEST_PUBKEY));
}

TEST(test_every_signed_prefix_bit_matters) {
    // The signature covers all 128 bytes. Walked on a stride because every
    // verify is a full curve operation in an unoptimised native build.
    const OtaImage::KeyRing& ring = kFixtureRing;
    OtaImage::Header base;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, base),
              OtaImage::ACCEPTED);

    for (size_t i = 0; i < OTA_SIGNED_PREFIX_BYTES; i += 7) {
        OtaImage::Header h = base;
        h.signedPrefix[i] ^= 0x01;
        if (OtaImage::checkSignature(h, ring) == OtaImage::ACCEPTED) {
            printf("FAIL\n    accepted signed prefix byte %u flipped\n", (unsigned)i);
            s_testsFailed++;
            return;
        }
    }
    for (size_t i = 0; i < OTA_SIG_BYTES; i += 11) {
        OtaImage::Header h = base;
        h.sig[i] ^= 0x01;
        if (OtaImage::checkSignature(h, ring) == OtaImage::ACCEPTED) {
            printf("FAIL\n    accepted signature byte %u flipped\n", (unsigned)i);
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_empty_or_malformed_keyring_trusts_nobody) {
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, h),
              OtaImage::ACCEPTED);

    ASSERT_EQ(OtaImage::checkSignature(h, ringOf(kFixtureKeyIds, kFixtureKeys, 0)),
              OtaImage::UNKNOWN_KEY);
    ASSERT_EQ(OtaImage::checkSignature(h, ringOf(nullptr, kFixtureKeys, 1)),
              OtaImage::UNKNOWN_KEY);
    ASSERT_EQ(OtaImage::checkSignature(h, ringOf(kFixtureKeyIds, nullptr, 1)),
              OtaImage::UNKNOWN_KEY);

    // A ring of the wrong SHAPE is refused too. count is trusted to describe
    // the arrays, so a ring claiming more rows than it has would walk off the
    // end; there is nothing the verifier can do about that except be handed a
    // truthful one, which is why the setter is firmware-source-only.
    ASSERT_EQ(OtaImage::checkSignature(h, ringOf(nullptr, nullptr, 0)),
              OtaImage::UNKNOWN_KEY);

    // The default argument is the firmware's own list, which does NOT hold the
    // fixture key. So the good fixture header, which every other test in this
    // file accepts, is refused outright when no ring is supplied.
    ASSERT_EQ(OtaImage::checkSignature(h), OtaImage::UNKNOWN_KEY);
    ASSERT_EQ(OtaImage::accept(FIX_OTA_HDR_OK, OTA_HEADER_BYTES,
                               FIX_OTA_SLOT_BYTES, h),
              OtaImage::UNKNOWN_KEY);
}

TEST(test_flash_digest_matches_and_mismatches) {
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, h),
              OtaImage::ACCEPTED);

    uint8_t digest[OTA_DIGEST_BYTES];
    ContentCrypto::sha512(FIX_OTA_IMAGE, sizeof(FIX_OTA_IMAGE),
                          nullptr, 0, nullptr, 0, digest);
    ASSERT_EQ(OtaImage::checkFlashDigest(h, digest), OtaImage::ACCEPTED);

    // The alternate body is the same length and entirely different content, so
    // there is no length discrepancy to notice first.
    uint8_t altDigest[OTA_DIGEST_BYTES];
    ContentCrypto::sha512(FIX_OTA_IMAGE_ALT, sizeof(FIX_OTA_IMAGE_ALT),
                          nullptr, 0, nullptr, 0, altDigest);
    ASSERT_EQ(OtaImage::checkFlashDigest(h, altDigest), OtaImage::DIGEST_MISMATCH);

    // Every byte of the digest has to matter.
    for (size_t i = 0; i < OTA_DIGEST_BYTES; i += 5) {
        uint8_t bad[OTA_DIGEST_BYTES];
        memcpy(bad, digest, sizeof(bad));
        bad[i] ^= 0x01;
        if (OtaImage::checkFlashDigest(h, bad) != OtaImage::DIGEST_MISMATCH) {
            printf("FAIL\n    digest byte %u did not matter\n", (unsigned)i);
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_every_verdict_has_a_name_and_help) {
    // Values 0 through 22 are the whole enum today, and they travel on the
    // wire. A verdict appended without a name renders to the owner as
    // "unknown", which is the failure mode this enum exists to avoid.
    for (int v = 0; v <= (int)OtaImage::OUT_OF_SEQUENCE; v++) {
        const char* name = OtaImage::verdictName((Verdict)v);
        const char* help = OtaImage::verdictHelp((Verdict)v);
        if (strcmp(name, "unknown") == 0 || name[0] == '\0' || help[0] == '\0') {
            printf("FAIL\n    verdict %d has no name or no help\n", v);
            s_testsFailed++;
            return;
        }
    }
    ASSERT_STREQ(OtaImage::verdictName((Verdict)200), "unknown");
    ASSERT_STREQ(OtaImage::verdictName(OtaImage::ACCEPTED), "accepted");
    ASSERT_STREQ(OtaImage::verdictName(OtaImage::NO_UPDATE_SLOT), "no_update_slot");
}

// ══ Session: the happy path ═══════════════════════════════════════════════

TEST(test_happy_path_reaches_ready_and_only_then_sets_boot) {
    SessionRig rig;
    ASSERT_EQ(rig.session.state(), OtaSession::IDLE);
    ASSERT_FALSE(rig.session.readyToReboot());

    ASSERT_TRUE(rig.session.arm(1000, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.state(), OtaSession::ARMED);
    ASSERT_EQ(rig.target.beginCalls(), 0u);

    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 1000),
              OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::RECEIVING);
    ASSERT_EQ(rig.session.imageLen(), (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    // The boot pointer has NOT moved yet, and must not until the very end.
    ASSERT_TRUE(deviceUnchanged(rig.target));

    // A zero-length chunk is a legal no-op that refreshes the stall timer.
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, 0, 1100));
    ASSERT_EQ(rig.session.bytesWritten(), 0u);

    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 1100));
    ASSERT_EQ(rig.session.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_EQ(rig.target.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_EQ(rig.session.state(), OtaSession::RECEIVING);
    // Still nothing irreversible has happened.
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_EQ(rig.target.setBootCalls(), 0u);

    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_TRUE(rig.session.readyToReboot());
    ASSERT_EQ(rig.session.verdict(), OtaImage::ACCEPTED);

    // Only now.
    ASSERT_TRUE(rig.target.bootPartitionSet());
    ASSERT_EQ(rig.target.setBootCalls(), 1u);
    ASSERT_EQ(rig.target.abortCalls(), 0u);

    // And what is on the flash is the image that was signed for.
    ASSERT_TRUE(memcmp(rig.target.data(), FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN) == 0);
}

TEST(test_one_whole_image_in_a_single_chunk) {
    // 1536 bytes is under HEXHOUND_OTA_CHUNK_MAX, so the boundary case of a
    // transfer with exactly one write must also reach READY.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_TRUE(rig.target.bootPartitionSet());
}

// ══ Session: verify before WRITE ══════════════════════════════════════════

TEST(test_rejected_header_never_touches_the_target) {
    // ── The single most important property in the subsystem ───────────────
    //
    // The claim in docs/p3w2-wiring.md is "verify before WRITE", not "verify
    // before commit". So this asserts the NEGATIVE: begin() was never called,
    // which on hardware is esp_ota_begin(), which erases the passive slot.
    //
    // bytesWritten() alone would not prove it. MemoryOtaTarget::begin() sets
    // _written back to 0, so a session that erased the slot and then refused
    // the image would still report zero bytes written. beginCalls() is the
    // observation that distinguishes those, and it is why the hook exists.
    for (size_t i = 0; i < sizeof(kHeaderCases) / sizeof(kHeaderCases[0]); i++) {
        SessionRig rig;
        Verdict got = OtaImage::ACCEPTED;
        if (!armAndOffer(rig, kHeaderCases[i].hdr, OTA_HEADER_BYTES, 5000, got)) {
            printf("FAIL\n    %s: arm() refused\n", kHeaderCases[i].what);
            s_testsFailed++;
            return;
        }

        if (got != kHeaderCases[i].expect ||
            rig.target.beginCalls()   != 0u ||
            rig.target.bytesWritten() != 0u ||
            rig.session.bytesWritten()!= 0u ||
            rig.target.bootPartitionSet() ||
            rig.session.state() != OtaSession::FAILED ||
            !failedIsHonest(rig.session)) {
            printf("FAIL\n    %s: verdict %s, beginCalls %u, targetBytes %u, "
                   "sessionBytes %u, booted %d, state %s\n",
                   kHeaderCases[i].what, OtaImage::verdictName(got),
                   (unsigned)rig.target.beginCalls(),
                   (unsigned)rig.target.bytesWritten(),
                   (unsigned)rig.session.bytesWritten(),
                   (int)rig.target.bootPartitionSet(),
                   OtaSession::stateName(rig.session.state()));
            s_testsFailed++;
            return;
        }

        // fail() aborts the target on every path, unconditionally.
        if (rig.target.abortCalls() == 0u) {
            printf("FAIL\n    %s: the target was not aborted\n", kHeaderCases[i].what);
            s_testsFailed++;
            return;
        }
    }

    // A truncated header offered to the session takes the same path.
    SessionRig shortRig;
    Verdict shortVerdict = OtaImage::ACCEPTED;
    ASSERT_TRUE(armAndOffer(shortRig, FIX_OTA_HDR_OK, OTA_HEADER_BYTES - 1,
                            5000, shortVerdict));
    ASSERT_EQ(shortVerdict, OtaImage::SHORT_HEADER);
    ASSERT_EQ(shortRig.target.beginCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(shortRig.target));
    ASSERT_TRUE(failedIsHonest(shortRig.session));
}

// ══ Session: failure paths ════════════════════════════════════════════════

TEST(test_offer_before_arming_is_refused) {
    SessionRig rig;
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 100),
              OtaImage::NOT_ARMED);
    // NOT_ARMED is a refusal by the SESSION and deliberately does not overwrite
    // the verdict about an image, because no image was ever judged.
    ASSERT_EQ(rig.session.state(), OtaSession::IDLE);
    ASSERT_STREQ(rig.session.failReason(), "not_armed");
    ASSERT_EQ(rig.target.beginCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // Same from READY, which is not ARMED either.
    SessionRig ready;
    ASSERT_TRUE(ready.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(ready.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(ready.session.offerChunk(FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(ready.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(ready.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 20),
              OtaImage::NOT_ARMED);
    ASSERT_EQ(ready.session.state(), OtaSession::READY);
    ASSERT_EQ(ready.target.beginCalls(), 1u);
}

TEST(test_arm_window_expires) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(10000, OTA_ARM_WINDOW_MS));

    rig.session.tick(10000 + OTA_ARM_WINDOW_MS - 1);
    ASSERT_EQ(rig.session.state(), OtaSession::ARMED);

    rig.session.tick(10000 + OTA_ARM_WINDOW_MS);
    ASSERT_EQ(rig.session.state(), OtaSession::IDLE);
    ASSERT_STREQ(rig.session.failReason(), "arm_window_expired");

    // A host arriving after the window is simply not armed. The device
    // deliberately does not sit there accepting firmware.
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES,
                                      10000 + OTA_ARM_WINDOW_MS + 1),
              OtaImage::NOT_ARMED);
    ASSERT_EQ(rig.target.beginCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));
}

TEST(test_transfer_stall_aborts_the_slot) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, 500, 100));

    rig.session.tick(100 + OTA_RX_STALL_MS - 1);
    ASSERT_EQ(rig.session.state(), OtaSession::RECEIVING);

    rig.session.tick(100 + OTA_RX_STALL_MS);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_EQ(rig.session.verdict(), OtaImage::TRANSFER_STALLED);
    ASSERT_STREQ(rig.session.failReason(), "transfer_stalled");
    ASSERT_EQ(rig.target.bytesWritten(), 0u);   // aborted
    ASSERT_GT(rig.target.abortCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));
}

TEST(test_overrun_by_exactly_one_byte_is_refused) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(rig.session.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);

    // One byte past the SIGNED length. Refused outright, not clamped.
    static const uint8_t extra[1] = { 0x00 };
    ASSERT_FALSE(rig.session.offerChunk(extra, 1, 2000));
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_EQ(rig.session.verdict(), OtaImage::TRANSFER_OVERRUN);
    ASSERT_STREQ(rig.session.failReason(), "overrun");
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // The last chunk that straddles the end is refused whole, so a host cannot
    // get its first N bytes in and then be told about the rest.
    SessionRig straddle;
    ASSERT_TRUE(straddle.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(straddle.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(straddle.session.offerChunk(FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN - 1, 10));
    ASSERT_FALSE(straddle.session.offerChunk(FIX_OTA_IMAGE, 2, 20));
    ASSERT_EQ(straddle.session.verdict(), OtaImage::TRANSFER_OVERRUN);
    ASSERT_TRUE(deviceUnchanged(straddle.target));
}

TEST(test_truncated_transfer_is_refused) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN - 1, 10));
    ASSERT_EQ(rig.session.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN - 1u);

    // One byte short is not "close enough" for a boot image.
    ASSERT_EQ(rig.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_STREQ(rig.session.failReason(), "truncated");
    ASSERT_EQ(rig.target.setBootCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // And zero bytes is refused the same way, not treated as a no-op success.
    SessionRig empty;
    ASSERT_TRUE(empty.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(empty.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_EQ(empty.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_TRUE(deviceUnchanged(empty.target));
    ASSERT_TRUE(failedIsHonest(empty.session));
}

TEST(test_null_and_oversized_chunks_are_refused) {
    SessionRig nullRig;
    ASSERT_TRUE(nullRig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(nullRig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_FALSE(nullRig.session.offerChunk(nullptr, 16, 10));
    ASSERT_EQ(nullRig.session.state(), OtaSession::FAILED);
    ASSERT_EQ(nullRig.session.verdict(), OtaImage::BAD_CHUNK);
    ASSERT_STREQ(nullRig.session.failReason(), "null_chunk");
    ASSERT_TRUE(deviceUnchanged(nullRig.target));
    ASSERT_TRUE(failedIsHonest(nullRig.session));

    // A null pointer with a zero length is the no-op case and is checked
    // BEFORE the length, so it is still a refusal. Stated explicitly because
    // the ordering is what decides it.
    SessionRig nullZero;
    ASSERT_TRUE(nullZero.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(nullZero.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_FALSE(nullZero.session.offerChunk(nullptr, 0, 10));
    ASSERT_EQ(nullZero.session.verdict(), OtaImage::BAD_CHUNK);

    // One byte past HEXHOUND_OTA_CHUNK_MAX. Bounded so a host cannot make the
    // device stack-copy something unreasonable, and checked before the overrun
    // rule so an oversized chunk is named as such.
    static uint8_t big[HEXHOUND_OTA_CHUNK_MAX + 1];
    memset(big, 0xA5, sizeof(big));
    SessionRig bigRig;
    ASSERT_TRUE(bigRig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(bigRig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_FALSE(bigRig.session.offerChunk(big, sizeof(big), 10));
    ASSERT_EQ(bigRig.session.verdict(), OtaImage::BAD_CHUNK);
    ASSERT_STREQ(bigRig.session.failReason(), "chunk_too_large");
    ASSERT_TRUE(deviceUnchanged(bigRig.target));
    ASSERT_TRUE(failedIsHonest(bigRig.session));

    // A chunk offered while not RECEIVING is a sequence error reported through
    // the return value, and it deliberately leaves the verdict alone.
    SessionRig idle;
    ASSERT_FALSE(idle.session.offerChunk(FIX_OTA_IMAGE, 16, 10));
    ASSERT_EQ(idle.session.state(), OtaSession::IDLE);
    ASSERT_STREQ(idle.session.failReason(), "not_receiving");
    ASSERT_EQ(idle.target.beginCalls(), 0u);
}

TEST(test_finish_out_of_sequence) {
    // From IDLE. OUT_OF_SEQUENCE and not DIGEST_MISMATCH, because telling the
    // owner their update did not arrive intact when no image was ever offered
    // sends them to fix the wrong thing.
    SessionRig idle;
    ASSERT_EQ(idle.session.finish(), OtaImage::OUT_OF_SEQUENCE);
    ASSERT_EQ(idle.session.state(), OtaSession::IDLE);
    ASSERT_STREQ(idle.session.failReason(), "not_receiving");
    ASSERT_TRUE(deviceUnchanged(idle.target));

    // From ARMED, before a header.
    SessionRig armed;
    ASSERT_TRUE(armed.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(armed.session.finish(), OtaImage::OUT_OF_SEQUENCE);
    ASSERT_EQ(armed.session.state(), OtaSession::ARMED);
    ASSERT_EQ(armed.target.beginCalls(), 0u);

    // From READY, i.e. finish() twice. The staged image must not be disturbed
    // and setBootPartition() must not be called a second time.
    SessionRig ready;
    ASSERT_TRUE(ready.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(ready.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(ready.session.offerChunk(FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(ready.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(ready.session.finish(), OtaImage::OUT_OF_SEQUENCE);
    ASSERT_EQ(ready.session.state(), OtaSession::READY);
    ASSERT_EQ(ready.session.verdict(), OtaImage::ACCEPTED);
    ASSERT_EQ(ready.target.setBootCalls(), 1u);
    ASSERT_EQ(ready.target.abortCalls(), 0u);
}

// ══ Session: every MemoryOtaTarget fail-switch ════════════════════════════

TEST(test_no_update_slot_is_named_honestly) {
    // A zero slot size. checkFit would call this IMAGE_TOO_LARGE, which would
    // send the owner looking for a smaller file that does not exist, so the
    // session asks the target first.
    SessionRig rig;
    rig.target.failSlotSize(true);
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::NO_UPDATE_SLOT);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_STREQ(rig.session.failReason(), "no_update_slot");
    ASSERT_EQ(rig.target.beginCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // No target attached at all is the same answer.
    OtaSession detached;
    ASSERT_TRUE(detached.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(detached.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::NO_UPDATE_SLOT);
    ASSERT_STREQ(detached.failReason(), "no_target");
    ASSERT_TRUE(failedIsHonest(detached));
}

TEST(test_device_flash_failures_blame_the_device) {
    // begin() refuses. Everything about the image was already proven, so the
    // verdict must blame this device's flash rather than the update file.
    SessionRig beginRig;
    beginRig.target.failBegin(true);
    ASSERT_TRUE(beginRig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(beginRig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::DEVICE_FLASH_FAILED);
    ASSERT_EQ(beginRig.target.beginCalls(), 1u);
    ASSERT_STREQ(beginRig.session.failReason(), "begin_failed");
    ASSERT_TRUE(deviceUnchanged(beginRig.target));
    ASSERT_TRUE(failedIsHonest(beginRig.session));

    // write() refuses partway through.
    SessionRig writeRig;
    ASSERT_TRUE(writeRig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(writeRig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(writeRig.session.offerChunk(FIX_OTA_IMAGE, 500, 10));
    writeRig.target.failWrite(true);
    ASSERT_FALSE(writeRig.session.offerChunk(FIX_OTA_IMAGE + 500, 500, 20));
    ASSERT_EQ(writeRig.session.verdict(), OtaImage::DEVICE_FLASH_FAILED);
    ASSERT_STREQ(writeRig.session.failReason(), "write_failed");
    ASSERT_EQ(writeRig.target.bytesWritten(), 0u);   // aborted
    ASSERT_TRUE(deviceUnchanged(writeRig.target));
    ASSERT_TRUE(failedIsHonest(writeRig.session));

    // end() refuses. On hardware this is the SDK's own structural check of the
    // image, which is a genuinely different question from the digest.
    SessionRig endRig;
    endRig.target.failEnd(true);
    ASSERT_TRUE(endRig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(endRig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(endRig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(endRig.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_STREQ(endRig.session.failReason(), "image_invalid");
    ASSERT_EQ(endRig.target.setBootCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(endRig.target));
    ASSERT_TRUE(failedIsHonest(endRig.session));
}

TEST(test_readback_failure_blames_the_device_not_the_update) {
    // ── The flash that took the write and will not give it back ───────────
    //
    // finish() reads the image back out to hash it. If those reads fail, the
    // truthful answer is that this device's storage is broken, NOT that the
    // update did not arrive intact: the two send the owner in completely
    // different directions, one to a hardware problem and one to re-download
    // a file that was always fine.
    //
    // This branch had no test at all until failRead() existed, and it is the
    // failure the read-back digest is built to survive.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));

    // Everything on the way in succeeded. end() will succeed too, so this is
    // reached only by the read loop.
    ASSERT_EQ(rig.session.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);
    rig.target.failRead(true);

    ASSERT_EQ(rig.session.finish(), OtaImage::DEVICE_FLASH_FAILED);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_STREQ(rig.session.failReason(), "readback_failed");
    ASSERT_EQ(rig.target.setBootCalls(), 0u);
    ASSERT_GT(rig.target.abortCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_FALSE(rig.session.readyToReboot());
    ASSERT_TRUE(failedIsHonest(rig.session));

    // DEVICE_FLASH_FAILED and not DIGEST_MISMATCH, stated as the thing the
    // owner is shown, because that string is the whole point of the two
    // verdicts being distinct.
    ASSERT_STREQ(OtaImage::verdictHelp(rig.session.verdict()),
                 "This device's storage could not take the update. Nothing "
                 "was changed.");

    // With reads working again the same session retries and installs, so the
    // refusal really was about the reads and left nothing else broken.
    SessionRig retry;
    ASSERT_TRUE(retry.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(retry.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(retry, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(retry.session.finish(), OtaImage::ACCEPTED);
    ASSERT_TRUE(retry.target.bootPartitionSet());
}

TEST(test_set_boot_failure_does_not_blame_the_update) {
    // The image was good and this device could not switch to it. Saying
    // "digest mismatch" here would blame a perfectly valid update file for a
    // failure that happened entirely on this side.
    SessionRig rig;
    rig.target.failSetBoot(true);
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));

    ASSERT_EQ(rig.session.finish(), OtaImage::SET_BOOT_FAILED);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_STREQ(rig.session.failReason(), "set_boot_failed");
    ASSERT_EQ(rig.target.setBootCalls(), 1u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_FALSE(rig.session.readyToReboot());
    ASSERT_TRUE(failedIsHonest(rig.session));
}

// ══ Session: the read-back digest ═════════════════════════════════════════

TEST(test_corrupted_flash_is_caught_by_the_readback_digest) {
    // ── The test that justifies hashing flash rather than the wire ────────
    //
    // Every byte offered to the session was correct, every write() returned
    // true, and the running total matched the signed length exactly. A session
    // that accumulated its digest from the incoming stream would find that
    // digest correct and would mark this slot bootable.
    //
    // corrupt() then models flash that did not durably hold what it was given.
    // Only a digest computed by reading the bytes BACK OUT can tell the
    // difference, and that difference is a bricked device.
    for (uint32_t at = 0; at < FIX_OTA_IMAGE_LEN; at += 511) {
        SessionRig rig;
        if (!rig.session.arm(0, OTA_ARM_WINDOW_MS)) { s_testsFailed++; return; }
        if (rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0) !=
            OtaImage::ACCEPTED) { s_testsFailed++; return; }
        if (!streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10)) {
            s_testsFailed++; return;
        }

        // Everything the session could observe on the way in says success.
        if (rig.session.bytesWritten() != (uint32_t)FIX_OTA_IMAGE_LEN) {
            s_testsFailed++; return;
        }

        rig.target.corrupt(at);

        const Verdict v = rig.session.finish();
        if (v != OtaImage::DIGEST_MISMATCH ||
            rig.target.bootPartitionSet() ||
            rig.target.setBootCalls() != 0u ||
            rig.session.state() != OtaSession::FAILED ||
            !failedIsHonest(rig.session)) {
            printf("FAIL\n    a bit flipped at offset %u was not caught: %s\n",
                   (unsigned)at, OtaImage::verdictName(v));
            s_testsFailed++;
            return;
        }
    }

    // The last byte of the image specifically, which is where a read-back loop
    // that is short by one chunk tail would stop looking.
    SessionRig last;
    ASSERT_TRUE(last.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(last.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(last, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    last.target.corrupt(FIX_OTA_IMAGE_LEN - 1);
    ASSERT_EQ(last.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_TRUE(deviceUnchanged(last.target));
}

// ══ Session: the read-back loop, over more than one chunk ═════════════════

TEST(test_the_test_filler_matches_the_generator) {
    // FIRST, before anything uses the big body. If the C filler and the Python
    // one ever drift, this says so in one line. Without it the drift would
    // surface as a DIGEST_MISMATCH inside a session test and would read as a
    // bug in the read-back loop, which is the exact confusion the big fixture
    // exists to remove.
    uint8_t digest[OTA_DIGEST_BYTES];
    ContentCrypto::sha512(bigImage(), FIX_OTA_BIG_IMAGE_LEN,
                          nullptr, 0, nullptr, 0, digest);
    ASSERT_TRUE(memcmp(digest, FIX_OTA_BIG_IMAGE_SHA512, OTA_DIGEST_BYTES) == 0);

    // And the signed header vouches for those same bytes, so the fixture and
    // the filler are bound by a signature and not merely by this assertion.
    OtaImage::Header h;
    ASSERT_EQ(OtaImage::parse(FIX_OTA_HDR_BIG_OK, OTA_HEADER_BYTES, h),
              OtaImage::ACCEPTED);
    ASSERT_EQ(h.imageLen, (uint32_t)FIX_OTA_BIG_IMAGE_LEN);
    ASSERT_EQ(OtaImage::checkSignature(h, kFixtureRing), OtaImage::ACCEPTED);
    ASSERT_EQ(OtaImage::checkFlashDigest(h, digest), OtaImage::ACCEPTED);

    // The body is position-dependent, which is what makes a skipped or
    // repeated chunk detectable. A body of repeated bytes would hash the same
    // under several genuinely broken loops and every test below would pass
    // vacuously, so this is worth asserting rather than assuming: no two
    // chunk-sized windows are alike, and neither are the 64-byte blocks.
    const uint8_t* body = bigImage();
    ASSERT_TRUE(memcmp(body, body + HEXHOUND_OTA_CHUNK_MAX,
                       HEXHOUND_OTA_CHUNK_MAX) != 0);
    for (uint32_t off = 64; off + 64 <= FIX_OTA_BIG_IMAGE_LEN; off += 64) {
        if (memcmp(body, body + off, 64) == 0) {
            printf("FAIL\n    the big body repeats at offset %u\n", (unsigned)off);
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_readback_loop_verifies_a_multi_chunk_image) {
    // ── The loop that lets a 1.2 MB image be checked in 250 KB of RAM ─────
    //
    // Every other fixture here is 1536 bytes, under HEXHOUND_OTA_CHUNK_MAX,
    // so finish() has always read the whole image in ONE pass and the offset
    // and remainder arithmetic has never been exercised at all.
    //
    // 10000 bytes is two full chunks plus a short 1808-byte tail. A loop that
    // stops after the full chunks, double-counts one, or mishandles the
    // remainder produces a different digest and cannot reach READY.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_BIG_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.imageLen(), (uint32_t)FIX_OTA_BIG_IMAGE_LEN);

    ASSERT_TRUE(streamBody(rig, bigImage(), FIX_OTA_BIG_IMAGE_LEN, 10));
    ASSERT_EQ(rig.session.bytesWritten(), (uint32_t)FIX_OTA_BIG_IMAGE_LEN);
    ASSERT_EQ(rig.target.bytesWritten(), (uint32_t)FIX_OTA_BIG_IMAGE_LEN);
    ASSERT_TRUE(deviceUnchanged(rig.target));

    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_TRUE(rig.target.bootPartitionSet());
    ASSERT_TRUE(memcmp(rig.target.data(), bigImage(), FIX_OTA_BIG_IMAGE_LEN) == 0);

    // Chunked on the way IN as well as on the way out: the transfer above ran
    // in 500-byte offers, so the write path crossed the chunk boundaries at
    // 4096 and 8192 at offsets that do not line up with them.
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    ASSERT_EQ(rig.target.setBootCalls(), 1u);
}

TEST(test_readback_loop_catches_corruption_in_every_chunk) {
    // One bit flipped in each region the loop has to visit. This is the test
    // that distinguishes a loop that reads all three slices from one that
    // reads the first, or the first two, or the last one twice.
    //
    // The final entry is the byte the whole exercise is about: the last byte
    // of the short tail, which a loop that only reads whole chunks never
    // looks at.
    struct Spot { const char* where; uint32_t at; };
    const Spot spots[] = {
        { "first byte of chunk 0",        0u },
        { "middle of chunk 0",            HEXHOUND_OTA_CHUNK_MAX / 2u },
        { "last byte of chunk 0",         HEXHOUND_OTA_CHUNK_MAX - 1u },
        { "first byte of chunk 1",        HEXHOUND_OTA_CHUNK_MAX },
        { "last byte of chunk 1",         2u * HEXHOUND_OTA_CHUNK_MAX - 1u },
        { "first byte of the short tail", 2u * HEXHOUND_OTA_CHUNK_MAX },
        { "middle of the short tail",     2u * HEXHOUND_OTA_CHUNK_MAX + 900u },
        { "last byte of the image",       FIX_OTA_BIG_IMAGE_LEN - 1u },
    };

    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        SessionRig rig;
        if (!rig.session.arm(0, OTA_ARM_WINDOW_MS)) { s_testsFailed++; return; }
        if (rig.session.offerHeader(FIX_OTA_HDR_BIG_OK, OTA_HEADER_BYTES, 0) !=
            OtaImage::ACCEPTED) { s_testsFailed++; return; }
        if (!streamBody(rig, bigImage(), FIX_OTA_BIG_IMAGE_LEN, 10)) {
            s_testsFailed++; return;
        }

        rig.target.corrupt(spots[i].at);

        const Verdict v = rig.session.finish();
        if (v != OtaImage::DIGEST_MISMATCH ||
            rig.target.bootPartitionSet() ||
            rig.target.setBootCalls() != 0u ||
            !failedIsHonest(rig.session)) {
            printf("FAIL\n    corruption at %s (offset %u) was not caught: %s\n",
                   spots[i].where, (unsigned)spots[i].at,
                   OtaImage::verdictName(v));
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_readback_loop_rejects_a_truncated_multi_chunk_transfer) {
    // Short by one byte on a body whose length is not a chunk multiple. The
    // truncation check is on the byte count, so this never reaches the loop,
    // but it is the case where an implementation that rounded the expected
    // length up to a chunk boundary would wave the transfer through.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_BIG_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, bigImage(), FIX_OTA_BIG_IMAGE_LEN - 1u, 10));
    ASSERT_EQ(rig.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_STREQ(rig.session.failReason(), "truncated");
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // And short by exactly the tail, i.e. stopping on a clean chunk boundary,
    // which is the truncation an off-by-one in the loop bound would produce.
    SessionRig onBoundary;
    ASSERT_TRUE(onBoundary.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(onBoundary.session.offerHeader(FIX_OTA_HDR_BIG_OK,
                                             OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(onBoundary, bigImage(),
                           2u * HEXHOUND_OTA_CHUNK_MAX, 10));
    ASSERT_EQ(onBoundary.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_TRUE(deviceUnchanged(onBoundary.target));
}

TEST(test_readback_failure_partway_through_a_multi_chunk_image) {
    // failRead() on a body big enough that the loop has already completed a
    // pass before it trips. The small fixture could only ever fail on the
    // first read, so this is the first time the failure is reached with a
    // partially accumulated hash and a non-zero offset behind it.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_BIG_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, bigImage(), FIX_OTA_BIG_IMAGE_LEN, 10));

    rig.target.failRead(true);
    ASSERT_EQ(rig.session.finish(), OtaImage::DEVICE_FLASH_FAILED);
    ASSERT_STREQ(rig.session.failReason(), "readback_failed");
    ASSERT_EQ(rig.target.setBootCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));
}

TEST(test_a_header_that_vouches_for_other_bytes_is_caught) {
    // A correctly signed, in-bounds, right-board header whose digest names
    // FIX_OTA_IMAGE_ALT, offered with FIX_OTA_IMAGE as the body. The header is
    // ACCEPTED, so this is caught nowhere but the digest check.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_DIGEST_MISMATCH,
                                      OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));

    ASSERT_EQ(rig.session.finish(), OtaImage::DIGEST_MISMATCH);
    ASSERT_STREQ(rig.session.failReason(), "digest_mismatch");
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // And the same header with the body it really vouches for goes through.
    SessionRig good;
    ASSERT_TRUE(good.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(good.session.offerHeader(FIX_OTA_HDR_DIGEST_MISMATCH,
                                       OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(good, FIX_OTA_IMAGE_ALT, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(good.session.finish(), OtaImage::ACCEPTED);
    ASSERT_TRUE(good.target.bootPartitionSet());
}

// ══ Session: the two brick vectors, as regression tests ═══════════════════

TEST(test_ready_refuses_rearm_and_leaves_the_staged_image) {
    // ── Brick vector 1 ────────────────────────────────────────────────────
    //
    // From READY, otadata already names the passive slot as the next boot
    // target. A second arm() followed by offerHeader() would reach begin(),
    // which on hardware is esp_ota_begin(), which ERASES that same slot. The
    // device would then be pointed at an erased partition and would not come
    // back.
    //
    // NOTE ON WHAT THIS TEST CAN AND CANNOT PROVE. MemoryOtaTarget::begin()
    // deliberately does not scrub its buffer, so "the staged bytes are still
    // there" would be true even if the slot HAD been re-begun. The assertion
    // that actually carries the property is beginCalls() == 1, because that is
    // the call the hardware turns into an erase. The byte comparison below is
    // supporting evidence, not the proof.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);

    ASSERT_FALSE(rig.session.arm(5000, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_STREQ(rig.session.failReason(), "already_staged");
    ASSERT_EQ(rig.session.verdict(), OtaImage::ACCEPTED);

    // Nothing was erased, nothing was aborted, and the boot pointer still
    // names the slot that holds the verified image.
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    ASSERT_EQ(rig.target.abortCalls(), 0u);
    ASSERT_TRUE(rig.target.bootPartitionSet());
    ASSERT_EQ(rig.target.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_TRUE(memcmp(rig.target.data(), FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN) == 0);

    // A header offered after the refused re-arm is refused too, because the
    // session never left READY. This is the second half of the vector: the
    // arm() refusal is only useful if it actually keeps offerHeader() away
    // from begin().
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 5000),
              OtaImage::NOT_ARMED);
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);

    // The arm window timer must not quietly walk it out of READY either.
    rig.session.tick(5000 + OTA_ARM_WINDOW_MS * 2);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_TRUE(rig.target.bootPartitionSet());
}

TEST(test_ready_refuses_cancel_and_leaves_the_staged_image) {
    // ── Brick vector 1, the other door ────────────────────────────────────
    //
    // cancel() from READY cannot undo setBootPartition(), so returning to IDLE
    // would tell the owner an update was cancelled while the device still
    // restarts into it. Here the fake IS load-bearing and honest: abort() sets
    // _written to 0, so bytesWritten() staying at the image length is real
    // evidence that abort() was not called.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);

    ASSERT_FALSE(rig.session.cancel("owner_changed_their_mind"));
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_STREQ(rig.session.failReason(), "already_staged");
    ASSERT_EQ(rig.session.verdict(), OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.readyToReboot());

    ASSERT_EQ(rig.target.abortCalls(), 0u);
    ASSERT_EQ(rig.target.beginCalls(), 1u);
    ASSERT_TRUE(rig.target.bootPartitionSet());
    ASSERT_EQ(rig.target.bytesWritten(), (uint32_t)FIX_OTA_IMAGE_LEN);
    ASSERT_TRUE(memcmp(rig.target.data(), FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN) == 0);
}

TEST(test_update_on_trial_is_refused_at_both_gates) {
    // ── Brick vector 2, the worse one ─────────────────────────────────────
    //
    // While the RUNNING image is on trial, the "next" partition is not spare
    // space: it is the PREVIOUS, known-good firmware and the only thing there
    // is to roll back to. Beginning an update erases it, leaving the device
    // running something unconfirmed with nothing behind it.
    //
    // Neither gate touches the target at all, so no fake could make these pass
    // by accident: the refusals are decided entirely inside OtaSession, and
    // beginCalls() == 0 is checked against the same fake that happily records a
    // begin() on every other path in this file.

    // Gate 1: arm() refuses outright.
    SessionRig atArm;
    atArm.session.setRunningImageOnTrial(true);
    ASSERT_TRUE(atArm.session.runningImageOnTrial());
    ASSERT_FALSE(atArm.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(atArm.session.state(), OtaSession::IDLE);
    ASSERT_EQ(atArm.session.verdict(), OtaImage::UPDATE_ON_TRIAL);
    ASSERT_STREQ(atArm.session.failReason(), "running_image_on_trial");
    ASSERT_EQ(atArm.target.beginCalls(), 0u);
    ASSERT_TRUE(deviceUnchanged(atArm.target));

    // Gate 2: arming legitimately preceded the trial resolving, so the gate
    // that actually makes the guarantee is the one just before the target is
    // touched.
    SessionRig atOffer;
    ASSERT_TRUE(atOffer.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(atOffer.session.state(), OtaSession::ARMED);
    atOffer.session.setRunningImageOnTrial(true);

    ASSERT_EQ(atOffer.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::UPDATE_ON_TRIAL);
    ASSERT_EQ(atOffer.session.state(), OtaSession::FAILED);
    ASSERT_EQ(atOffer.session.verdict(), OtaImage::UPDATE_ON_TRIAL);
    ASSERT_STREQ(atOffer.session.failReason(), "running_image_on_trial");
    ASSERT_EQ(atOffer.target.beginCalls(), 0u);
    ASSERT_EQ(atOffer.target.bytesWritten(), 0u);
    ASSERT_TRUE(deviceUnchanged(atOffer.target));
    ASSERT_TRUE(failedIsHonest(atOffer.session));

    // The refusal expires on its own: a trial lasts seconds, and once it
    // resolves the same session installs normally. A refusal that had to be
    // cleared by hand would be its own support problem.
    SessionRig after;
    after.session.setRunningImageOnTrial(true);
    ASSERT_FALSE(after.session.arm(0, OTA_ARM_WINDOW_MS));
    after.session.setRunningImageOnTrial(false);
    ASSERT_TRUE(after.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(after.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(after, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10));
    ASSERT_EQ(after.session.finish(), OtaImage::ACCEPTED);
    ASSERT_TRUE(after.target.bootPartitionSet());
}

// ══ Session: teardown and retry ═══════════════════════════════════════════

TEST(test_cancel_returns_to_idle_and_aborts_the_target) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.cancel("nothing_to_cancel"));   // IDLE is a no-op
    ASSERT_EQ(rig.target.abortCalls(), 0u);

    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, 500, 10));

    ASSERT_TRUE(rig.session.cancel("host_disconnected"));
    ASSERT_EQ(rig.session.state(), OtaSession::IDLE);
    ASSERT_EQ(rig.session.verdict(), OtaImage::UPDATE_ABANDONED);
    ASSERT_STREQ(rig.session.failReason(), "host_disconnected");
    ASSERT_GT(rig.target.abortCalls(), 0u);
    ASSERT_EQ(rig.target.bytesWritten(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));

    // A null reason still names something, because "" on a diagnostics screen
    // is indistinguishable from a bug.
    SessionRig unnamed;
    ASSERT_TRUE(unnamed.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_TRUE(unnamed.session.cancel(nullptr));
    ASSERT_STREQ(unnamed.session.failReason(), "cancelled");
}

TEST(test_rearm_during_transfer_abandons_the_first_attempt) {
    // Re-arming from a terminal state is how the owner retries. Re-arming
    // mid-transfer would let a second host interrupt the first, so the
    // in-flight attempt is torn down and the target aborted first.
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, 500, 10));
    ASSERT_EQ(rig.session.bytesWritten(), 500u);

    ASSERT_TRUE(rig.session.arm(2000, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.state(), OtaSession::ARMED);
    ASSERT_EQ(rig.session.bytesWritten(), 0u);
    ASSERT_GT(rig.target.abortCalls(), 0u);
    ASSERT_EQ(rig.target.bytesWritten(), 0u);
    ASSERT_TRUE(deviceUnchanged(rig.target));

    // arm() clears the verdict, because it describes an attempt that is over.
    ASSERT_EQ(rig.session.verdict(), OtaImage::ACCEPTED);
    ASSERT_STREQ(rig.session.failReason(), "");

    // And the retry installs cleanly.
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 2000),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 2100));
    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.target.bootPartitionSet());
}

TEST(test_retry_after_a_failure_installs_cleanly) {
    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(0, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_UNTAGGED_SIG,
                                      OTA_HEADER_BYTES, 0),
              OtaImage::BAD_SIGNATURE);
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_TRUE(failedIsHonest(rig.session));

    // The owner presses confirm again. No power cycle required.
    ASSERT_TRUE(rig.session.arm(1000, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.verdict(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 1000),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(streamBody(rig, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 1100));
    ASSERT_EQ(rig.session.finish(), OtaImage::ACCEPTED);
    ASSERT_EQ(rig.session.state(), OtaSession::READY);
    ASSERT_TRUE(rig.target.bootPartitionSet());
}

// ══ Session: millis() rollover ════════════════════════════════════════════

TEST(test_arm_window_survives_the_millis_rollover) {
    // millis() wraps every 49.7 days. Signed or naive arithmetic here would
    // either arm forever or disarm instantly at the wrap; both are wrong and
    // neither shows up in a test that starts the clock at zero.
    const uint32_t armedAt = 0xFFFFFFFFu - 1000u;

    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(armedAt, OTA_ARM_WINDOW_MS));

    // One millisecond before the window closes, having already wrapped.
    const uint32_t nearly = (uint32_t)(armedAt + OTA_ARM_WINDOW_MS - 1u);
    ASSERT_TRUE(nearly < armedAt);          // the clock really did wrap
    rig.session.tick(nearly);
    ASSERT_EQ(rig.session.state(), OtaSession::ARMED);

    // A header offered across the wrap is still accepted, because the window
    // is genuinely still open.
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, nearly),
              OtaImage::ACCEPTED);

    SessionRig expired;
    ASSERT_TRUE(expired.session.arm(armedAt, OTA_ARM_WINDOW_MS));
    expired.session.tick((uint32_t)(armedAt + OTA_ARM_WINDOW_MS));
    ASSERT_EQ(expired.session.state(), OtaSession::IDLE);
    ASSERT_STREQ(expired.session.failReason(), "arm_window_expired");
    ASSERT_EQ(expired.target.beginCalls(), 0u);
}

TEST(test_stall_timer_survives_the_millis_rollover) {
    const uint32_t rxAt = 0xFFFFFFFFu - 100u;

    SessionRig rig;
    ASSERT_TRUE(rig.session.arm(rxAt, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(rig.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, rxAt),
              OtaImage::ACCEPTED);
    ASSERT_TRUE(rig.session.offerChunk(FIX_OTA_IMAGE, 500, rxAt));

    const uint32_t nearly = (uint32_t)(rxAt + OTA_RX_STALL_MS - 1u);
    ASSERT_TRUE(nearly < rxAt);             // wrapped
    rig.session.tick(nearly);
    ASSERT_EQ(rig.session.state(), OtaSession::RECEIVING);

    rig.session.tick((uint32_t)(rxAt + OTA_RX_STALL_MS));
    ASSERT_EQ(rig.session.state(), OtaSession::FAILED);
    ASSERT_EQ(rig.session.verdict(), OtaImage::TRANSFER_STALLED);
    ASSERT_TRUE(deviceUnchanged(rig.target));
    ASSERT_TRUE(failedIsHonest(rig.session));

    // A transfer that spans the wrap and keeps sending is never stalled.
    SessionRig spanning;
    ASSERT_TRUE(spanning.session.arm(rxAt, OTA_ARM_WINDOW_MS));
    ASSERT_EQ(spanning.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, rxAt),
              OtaImage::ACCEPTED);
    uint32_t t = rxAt;
    uint32_t off = 0;
    while (off < (uint32_t)FIX_OTA_IMAGE_LEN) {
        uint32_t n = (uint32_t)FIX_OTA_IMAGE_LEN - off;
        if (n > 256) n = 256;
        ASSERT_TRUE(spanning.session.offerChunk(FIX_OTA_IMAGE + off, n, t));
        off += n;
        t = (uint32_t)(t + 30u);
        spanning.session.tick(t);
    }
    ASSERT_EQ(spanning.session.state(), OtaSession::RECEIVING);
    ASSERT_EQ(spanning.session.finish(), OtaImage::ACCEPTED);
    ASSERT_TRUE(spanning.target.bootPartitionSet());
}

// ══ Session: the invariant, swept ═════════════════════════════════════════

TEST(test_a_failed_session_never_reports_accepted) {
    // Asserted inline on every failure path above. Swept here as well, because
    // this invariant was broken once and the cost of it being broken again is
    // that the companion app renders a refusal to the owner as a success.
    for (size_t i = 0; i < sizeof(kHeaderCases) / sizeof(kHeaderCases[0]); i++) {
        SessionRig rig;
        Verdict got = OtaImage::ACCEPTED;
        armAndOffer(rig, kHeaderCases[i].hdr, OTA_HEADER_BYTES, 0, got);
        if (!failedIsHonest(rig.session)) {
            printf("FAIL\n    %s left a FAILED session reporting ACCEPTED\n",
                   kHeaderCases[i].what);
            s_testsFailed++;
            return;
        }
    }

    // The session-side causes too, each driven to FAILED by its own route.
    {
        SessionRig r; r.target.failSlotSize(true);
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::NO_UPDATE_SLOT);
    }
    {
        SessionRig r; r.target.failBegin(true);
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::DEVICE_FLASH_FAILED);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        r.session.offerChunk(nullptr, 8, 10);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::BAD_CHUNK);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        streamBody(r, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10);
        static const uint8_t one[1] = { 0 };
        r.session.offerChunk(one, 1, 3000);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::TRANSFER_OVERRUN);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        r.session.tick(OTA_RX_STALL_MS);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::TRANSFER_STALLED);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        streamBody(r, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN - 1, 10);
        r.session.finish();
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::DIGEST_MISMATCH);
    }
    {
        SessionRig r; r.target.failSetBoot(true);
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        streamBody(r, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10);
        r.session.finish();
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::SET_BOOT_FAILED);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.setRunningImageOnTrial(true);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::UPDATE_ON_TRIAL);
    }
    {
        SessionRig r;
        r.session.arm(0, OTA_ARM_WINDOW_MS);
        r.session.offerHeader(FIX_OTA_HDR_OK, OTA_HEADER_BYTES, 0);
        streamBody(r, FIX_OTA_IMAGE, FIX_OTA_IMAGE_LEN, 10);
        r.target.corrupt(64);
        r.session.finish();
        ASSERT_TRUE(failedIsHonest(r.session));
        ASSERT_EQ(r.session.verdict(), OtaImage::DIGEST_MISMATCH);
    }
}

TEST(test_state_names_are_all_defined) {
    ASSERT_STREQ(OtaSession::stateName(OtaSession::IDLE), "idle");
    ASSERT_STREQ(OtaSession::stateName(OtaSession::ARMED), "armed");
    ASSERT_STREQ(OtaSession::stateName(OtaSession::RECEIVING), "receiving");
    ASSERT_STREQ(OtaSession::stateName(OtaSession::VERIFYING), "verifying");
    ASSERT_STREQ(OtaSession::stateName(OtaSession::READY), "ready");
    ASSERT_STREQ(OtaSession::stateName(OtaSession::FAILED), "failed");
    ASSERT_STREQ(OtaSession::stateName((OtaSession::State)77), "unknown");
}

// ══ Health policy ═════════════════════════════════════════════════════════

static const uint8_t kMilestones[] = {
    OTA_HEALTH_DISPLAY, OTA_HEALTH_STORAGE, OTA_HEALTH_PET,
    OTA_HEALTH_CRYPTO,  OTA_HEALTH_LOOP
};
static const size_t kMilestoneCount = sizeof(kMilestones) / sizeof(kMilestones[0]);

static void reachAll(OtaHealthPolicy& p) {
    for (size_t i = 0; i < kMilestoneCount; i++) p.reached(kMilestones[i]);
}

TEST(test_health_waits_before_the_settle_time) {
    // An image that reaches every milestone and then immediately crashes
    // should not have been confirmed in between, which is why the settle time
    // is not zero.
    OtaHealthPolicy p;
    p.begin(1000);
    reachAll(p);
    ASSERT_TRUE(p.allRequired());
    ASSERT_EQ(p.missingMask(), (uint8_t)0);

    ASSERT_EQ(p.evaluate(1000), OtaHealthPolicy::WAIT);
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_SETTLE_MS - 1), OtaHealthPolicy::WAIT);
}

TEST(test_health_confirms_once_settled_with_everything) {
    OtaHealthPolicy p;
    p.begin(1000);
    reachAll(p);
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_SETTLE_MS), OtaHealthPolicy::CONFIRM);
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_SETTLE_MS + 1), OtaHealthPolicy::CONFIRM);

    // ── The deadline is on the MILESTONES, not on the confirmation ────────
    //
    // CONFIRM is tested before ROLLBACK, so an image that reaches all five
    // milestones is confirmed even if it got there after
    // OTA_HEALTH_DEADLINE_MS. That is deliberate and ota_health.h now says so:
    // an image with display, storage, pet, crypto and loop all reporting is a
    // working device by every measure this firmware has, and rolling it back
    // for being slow would cost the owner a functioning update and buy no
    // safety. SPIFFS formats and save migrations make slow boots real here.
    //
    // The rollback bias is for images that cannot be vouched for, and this is
    // not one. A genuinely wedged image never reaches loop(), so it never
    // calls evaluate() at all and the trial watchdog is what catches it.
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::CONFIRM);
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_DEADLINE_MS * 4), OtaHealthPolicy::CONFIRM);

    // Milestones arriving one at a time: no CONFIRM until the last one lands.
    OtaHealthPolicy slow;
    slow.begin(0);
    for (size_t i = 0; i < kMilestoneCount; i++) {
        if (i + 1 < kMilestoneCount) {
            if (slow.evaluate(OTA_HEALTH_SETTLE_MS) != OtaHealthPolicy::WAIT) {
                printf("FAIL\n    confirmed with only %u milestones\n", (unsigned)i);
                s_testsFailed++;
                return;
            }
        }
        slow.reached(kMilestones[i]);
    }
    ASSERT_EQ(slow.evaluate(OTA_HEALTH_SETTLE_MS), OtaHealthPolicy::CONFIRM);
}

TEST(test_health_rolls_back_at_the_deadline) {
    OtaHealthPolicy p;
    p.begin(1000);
    for (size_t i = 0; i + 1 < kMilestoneCount; i++) p.reached(kMilestones[i]);
    ASSERT_FALSE(p.allRequired());

    // Not one millisecond early: an image that is merely slow gets its full
    // window, because rolling back a healthy image presents as "updates
    // randomly refuse to stick".
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_DEADLINE_MS - 1), OtaHealthPolicy::WAIT);
    ASSERT_EQ(p.evaluate(1000 + OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::ROLLBACK);

    // Nothing reported at all is the same answer. Merely still executing does
    // not earn an image permanence.
    OtaHealthPolicy silent;
    silent.begin(0);
    ASSERT_EQ(silent.reachedMask(), (uint8_t)0);
    ASSERT_EQ(silent.evaluate(OTA_HEALTH_SETTLE_MS), OtaHealthPolicy::WAIT);
    ASSERT_EQ(silent.evaluate(OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::ROLLBACK);
}

TEST(test_any_single_missing_milestone_forces_rollback) {
    // Each required milestone, on its own, is enough to refuse the image. This
    // is also the test that catches a milestone added to OTA_HEALTH_REQUIRED
    // without a call site: docs/p3w2-wiring.md warns that such a milestone
    // makes every update roll back after 20 seconds with no obvious cause.
    for (size_t missing = 0; missing < kMilestoneCount; missing++) {
        OtaHealthPolicy p;
        p.begin(0);
        for (size_t i = 0; i < kMilestoneCount; i++) {
            if (i != missing) p.reached(kMilestones[i]);
        }

        if (p.allRequired() ||
            p.evaluate(OTA_HEALTH_SETTLE_MS) != OtaHealthPolicy::WAIT ||
            p.evaluate(OTA_HEALTH_DEADLINE_MS) != OtaHealthPolicy::ROLLBACK ||
            p.missingMask() != kMilestones[missing]) {
            printf("FAIL\n    missing %s did not force a rollback\n",
                   OtaHealthPolicy::milestoneName(kMilestones[missing]));
            s_testsFailed++;
            return;
        }
    }
}

TEST(test_missing_mask_names_the_right_milestones) {
    OtaHealthPolicy p;
    p.begin(0);
    ASSERT_EQ(p.missingMask(), (uint8_t)OTA_HEALTH_REQUIRED);

    p.reached(OTA_HEALTH_DISPLAY);
    p.reached(OTA_HEALTH_LOOP);
    ASSERT_EQ(p.reachedMask(), (uint8_t)(OTA_HEALTH_DISPLAY | OTA_HEALTH_LOOP));
    ASSERT_EQ(p.missingMask(),
              (uint8_t)(OTA_HEALTH_STORAGE | OTA_HEALTH_PET | OTA_HEALTH_CRYPTO));

    // Reporting the same milestone twice is not an error and does not unset it.
    p.reached(OTA_HEALTH_DISPLAY);
    ASSERT_EQ(p.reachedMask(), (uint8_t)(OTA_HEALTH_DISPLAY | OTA_HEALTH_LOOP));

    reachAll(p);
    ASSERT_EQ(p.missingMask(), (uint8_t)0);

    // Being told the update was rejected is much less useful than being told
    // the display never came up, so the names have to be there.
    for (size_t i = 0; i < kMilestoneCount; i++) {
        const char* n = OtaHealthPolicy::milestoneName(kMilestones[i]);
        if (n[0] == '\0' || strcmp(n, "unknown") == 0) {
            printf("FAIL\n    milestone bit 0x%02x has no name\n", kMilestones[i]);
            s_testsFailed++;
            return;
        }
    }
    ASSERT_STREQ(OtaHealthPolicy::milestoneName(OTA_HEALTH_DISPLAY), "display");
    ASSERT_STREQ(OtaHealthPolicy::milestoneName(0x80), "unknown");
    // A mask rather than a single bit is not a milestone name either.
    ASSERT_STREQ(OtaHealthPolicy::milestoneName(OTA_HEALTH_REQUIRED), "unknown");
}

TEST(test_health_decision_latches_after_it_is_acted_on) {
    // Confirming can fail, and the rollback-and-reboot that follows can also
    // fail when there is no other image to go to. Without the latch the device
    // spins on a call that has already told it no, for as long as it is
    // switched on.
    OtaHealthPolicy p;
    p.begin(0);
    reachAll(p);
    ASSERT_FALSE(p.decided());
    ASSERT_EQ(p.evaluate(OTA_HEALTH_SETTLE_MS), OtaHealthPolicy::CONFIRM);

    p.markDecided();
    ASSERT_TRUE(p.decided());
    ASSERT_EQ(p.evaluate(OTA_HEALTH_SETTLE_MS), OtaHealthPolicy::WAIT);
    ASSERT_EQ(p.evaluate(OTA_HEALTH_DEADLINE_MS * 10), OtaHealthPolicy::WAIT);

    // Same on the rollback side.
    OtaHealthPolicy bad;
    bad.begin(0);
    ASSERT_EQ(bad.evaluate(OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::ROLLBACK);
    bad.markDecided();
    ASSERT_EQ(bad.evaluate(OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::WAIT);

    // begin() starts a fresh trial: one trial per boot, one outcome.
    bad.begin(0);
    ASSERT_FALSE(bad.decided());
    ASSERT_EQ(bad.reachedMask(), (uint8_t)0);
    ASSERT_EQ(bad.evaluate(OTA_HEALTH_DEADLINE_MS), OtaHealthPolicy::ROLLBACK);
}

TEST(test_health_survives_the_millis_rollover) {
    // The trial window can straddle the wrap just as the arm window can, and
    // the consequence of getting it wrong is worse: an instant rollback of a
    // healthy image, or a pending image that is never decided at all.
    const uint32_t bootMs = 0xFFFFFFFFu - 5000u;

    OtaHealthPolicy p;
    p.begin(bootMs);
    reachAll(p);
    ASSERT_EQ(p.evaluate((uint32_t)(bootMs + OTA_HEALTH_SETTLE_MS - 1)),
              OtaHealthPolicy::WAIT);
    ASSERT_EQ(p.evaluate((uint32_t)(bootMs + OTA_HEALTH_SETTLE_MS)),
              OtaHealthPolicy::CONFIRM);

    OtaHealthPolicy q;
    q.begin(bootMs);
    q.reached(OTA_HEALTH_DISPLAY);
    ASSERT_EQ(q.evaluate((uint32_t)(bootMs + OTA_HEALTH_DEADLINE_MS - 1)),
              OtaHealthPolicy::WAIT);
    ASSERT_EQ(q.evaluate((uint32_t)(bootMs + OTA_HEALTH_DEADLINE_MS)),
              OtaHealthPolicy::ROLLBACK);
}

// ══ Runner ════════════════════════════════════════════════════════════════

int main() {
    printf("\n=== OTA: signed envelope ===\n");
    RUN_TEST(test_board_id_matches_the_fixtures);
    RUN_TEST(test_fixture_key_is_not_a_firmware_key);
    RUN_TEST(test_the_session_seam_defaults_to_the_firmware_keys);
    RUN_TEST(test_valid_header_parses_fits_and_verifies);
    RUN_TEST(test_short_and_null_headers_are_refused);
    RUN_TEST(test_every_corrupted_fixture_gets_the_verdict_it_claims);
    RUN_TEST(test_board_mismatch_is_refused_on_its_own);
    RUN_TEST(test_oversized_and_undersized_images_are_refused);
    RUN_TEST(test_unknown_key_and_bad_signature_are_different);
    RUN_TEST(test_transplanted_and_untagged_signatures_are_refused);
    RUN_TEST(test_every_signed_prefix_bit_matters);
    RUN_TEST(test_empty_or_malformed_keyring_trusts_nobody);
    RUN_TEST(test_flash_digest_matches_and_mismatches);
    RUN_TEST(test_every_verdict_has_a_name_and_help);

    printf("\n=== OTA: session, the happy path ===\n");
    RUN_TEST(test_happy_path_reaches_ready_and_only_then_sets_boot);
    RUN_TEST(test_one_whole_image_in_a_single_chunk);

    printf("\n=== OTA: session, verify before WRITE ===\n");
    RUN_TEST(test_rejected_header_never_touches_the_target);

    printf("\n=== OTA: session, failure paths ===\n");
    RUN_TEST(test_offer_before_arming_is_refused);
    RUN_TEST(test_arm_window_expires);
    RUN_TEST(test_transfer_stall_aborts_the_slot);
    RUN_TEST(test_overrun_by_exactly_one_byte_is_refused);
    RUN_TEST(test_truncated_transfer_is_refused);
    RUN_TEST(test_null_and_oversized_chunks_are_refused);
    RUN_TEST(test_finish_out_of_sequence);

    printf("\n=== OTA: session, target failures ===\n");
    RUN_TEST(test_no_update_slot_is_named_honestly);
    RUN_TEST(test_device_flash_failures_blame_the_device);
    RUN_TEST(test_readback_failure_blames_the_device_not_the_update);
    RUN_TEST(test_set_boot_failure_does_not_blame_the_update);

    printf("\n=== OTA: session, the read-back digest ===\n");
    RUN_TEST(test_corrupted_flash_is_caught_by_the_readback_digest);
    RUN_TEST(test_a_header_that_vouches_for_other_bytes_is_caught);

    printf("\n=== OTA: session, the read-back loop over many chunks ===\n");
    RUN_TEST(test_the_test_filler_matches_the_generator);
    RUN_TEST(test_readback_loop_verifies_a_multi_chunk_image);
    RUN_TEST(test_readback_loop_catches_corruption_in_every_chunk);
    RUN_TEST(test_readback_loop_rejects_a_truncated_multi_chunk_transfer);
    RUN_TEST(test_readback_failure_partway_through_a_multi_chunk_image);

    printf("\n=== OTA: session, brick vectors ===\n");
    RUN_TEST(test_ready_refuses_rearm_and_leaves_the_staged_image);
    RUN_TEST(test_ready_refuses_cancel_and_leaves_the_staged_image);
    RUN_TEST(test_update_on_trial_is_refused_at_both_gates);

    printf("\n=== OTA: session, teardown and retry ===\n");
    RUN_TEST(test_cancel_returns_to_idle_and_aborts_the_target);
    RUN_TEST(test_rearm_during_transfer_abandons_the_first_attempt);
    RUN_TEST(test_retry_after_a_failure_installs_cleanly);

    printf("\n=== OTA: session, millis() rollover ===\n");
    RUN_TEST(test_arm_window_survives_the_millis_rollover);
    RUN_TEST(test_stall_timer_survives_the_millis_rollover);

    printf("\n=== OTA: session, invariants ===\n");
    RUN_TEST(test_a_failed_session_never_reports_accepted);
    RUN_TEST(test_state_names_are_all_defined);

    printf("\n=== OTA: health policy ===\n");
    RUN_TEST(test_health_waits_before_the_settle_time);
    RUN_TEST(test_health_confirms_once_settled_with_everything);
    RUN_TEST(test_health_rolls_back_at_the_deadline);
    RUN_TEST(test_any_single_missing_milestone_forces_rollback);
    RUN_TEST(test_missing_mask_names_the_right_milestones);
    RUN_TEST(test_health_decision_latches_after_it_is_acted_on);
    RUN_TEST(test_health_survives_the_millis_rollover);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
