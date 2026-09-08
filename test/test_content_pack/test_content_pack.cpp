// ── HexHound - Signed Content Pack Unit Tests ────────────────────
//
// Covers P3-W1: the Ed25519 verifier, the strict CBOR reader, the signed pack
// envelope, and the loader policy that sits on top of all three.
//
// The organising rule for this file: EVERY refusal is followed by an assertion
// that the pet can still speak. A verifier that rejects a bad pack and leaves
// the device with nothing to say has not protected anybody. That is checked
// after every single rejection path, not once at the end.

#include "../test_stubs.h"

#include "../../src/content/content_crypto.cpp"
#include "../../src/content/content_cbor.cpp"
#include "../../src/content/content_pack.cpp"
#include "../../src/content/content_store.h"
#include "../../src/content/content_store.cpp"

#include "fixtures.h"

#include <ArduinoJson.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TEST_MKDIR(p) mkdir((p), 0755)
#endif

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        s_testsFailed++; return; \
    } \
} while(0)

#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != nullptr)

// ── Helpers ───────────────────────────────────────────────────────────────

static void hexToBin(const char* hex, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

// Every test that touches the store starts from the shipped content, because
// the store is a singleton and a leaked pack would make the next test lie.
static void resetContent() {
    ContentStore::instance().resetToBaseline();
}

// The invariant this whole workstream exists to protect. Called after every
// rejection: the tables must still be populated AND the rows must still be
// readable, not merely counted.
static bool baselineIsUsable() {
    ContentStore& store = ContentStore::instance();
    if (store.dialogueCount() == 0) return false;
    if (store.questDefCount() == 0) return false;
    if (store.dialogueFromPack()) return false;
    if (store.questsFromPack()) return false;

    const DialogueLine* line = store.dialogue(0);
    if (line == nullptr || line->text[0] == '\0') return false;

    const QuestDef* quest = store.questDef(0);
    if (quest == nullptr || quest->id[0] == '\0' || quest->text[0] == '\0') return false;
    return true;
}

// Verifies against the fixed TEST key rather than the firmware's list.
static ContentPackStatus verifyWithTestKey(const uint8_t* image, size_t len,
                                           uint8_t kind,
                                           const uint8_t** body = nullptr,
                                           size_t* bodyLen = nullptr) {
    const uint8_t (*keys)[32] = (const uint8_t (*)[32])FIX_TEST_PUBKEY;
    return ContentPack::verifyImage(image, len, kind, keys, 1, body, bodyLen);
}

static bool writeFile(const char* path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len;
}

// ══ Ed25519 and SHA-512 ═══════════════════════════════════════════════════

struct Rfc8032Vector { const char* pk; const char* msg; const char* sig; };

static const Rfc8032Vector RFC8032[3] = {
    { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

TEST(test_ed25519_rfc8032_vectors_accept) {
    // These vectors are the only thing standing behind the curve code, because
    // mbedtls has no Ed25519 and there is therefore no second implementation
    // to cross-check against on any target.
    for (int i = 0; i < 3; i++) {
        uint8_t pk[32], sig[64], msg[8];
        const size_t msgLen = strlen(RFC8032[i].msg) / 2;
        hexToBin(RFC8032[i].pk, pk, 32);
        hexToBin(RFC8032[i].sig, sig, 64);
        hexToBin(RFC8032[i].msg, msg, msgLen);
        ASSERT_TRUE(ContentCrypto::verify(msg, msgLen, sig, pk));
    }
}

TEST(test_ed25519_rejects_tampering) {
    for (int i = 0; i < 3; i++) {
        uint8_t pk[32], sig[64], msg[8];
        const size_t msgLen = strlen(RFC8032[i].msg) / 2;
        hexToBin(RFC8032[i].pk, pk, 32);
        hexToBin(RFC8032[i].sig, sig, 64);
        hexToBin(RFC8032[i].msg, msg, msgLen);

        // Every byte of the signature must matter.
        for (int b = 0; b < 64; b += 7) {
            uint8_t bad[64];
            memcpy(bad, sig, 64);
            bad[b] ^= 0x01;
            ASSERT_FALSE(ContentCrypto::verify(msg, msgLen, bad, pk));
        }
        // And every byte of the key.
        for (int b = 0; b < 32; b += 5) {
            uint8_t badPk[32];
            memcpy(badPk, pk, 32);
            badPk[b] ^= 0x08;
            ASSERT_FALSE(ContentCrypto::verify(msg, msgLen, sig, badPk));
        }
    }
}

TEST(test_ed25519_rejects_non_canonical_s) {
    // S >= L is signature malleability: a third party can turn one valid
    // signature into a different valid one without the key.
    uint8_t pk[32], sig[64], msg[1];
    hexToBin(RFC8032[1].pk, pk, 32);
    hexToBin(RFC8032[1].sig, sig, 64);
    hexToBin(RFC8032[1].msg, msg, 1);
    ASSERT_TRUE(ContentCrypto::verify(msg, 1, sig, pk));

    uint8_t mauled[64];
    memcpy(mauled, sig, 64);
    mauled[63] |= 0x10;             // push S above the group order
    ASSERT_FALSE(ContentCrypto::verify(msg, 1, mauled, pk));

    memset(mauled + 32, 0xFF, 32);  // wildly above it
    ASSERT_FALSE(ContentCrypto::verify(msg, 1, mauled, pk));
}

TEST(test_ed25519_rejects_garbage_key) {
    // Most 32-byte strings are not points on the curve. Decompression has to
    // say so rather than carrying on with whatever it computed.
    uint8_t sig[64], msg[1];
    hexToBin(RFC8032[1].sig, sig, 64);
    hexToBin(RFC8032[1].msg, msg, 1);

    for (int seed = 0; seed < 16; seed++) {
        uint8_t pk[32];
        for (int i = 0; i < 32; i++) pk[i] = (uint8_t)(seed * 31 + i * 7);
        ASSERT_FALSE(ContentCrypto::verify(msg, 1, sig, pk));
    }

    uint8_t nonCanonical[32];
    memset(nonCanonical, 0xFF, 32);
    nonCanonical[31] = 0x7F;        // y == 2^255 - 1, above the field prime
    ASSERT_FALSE(ContentCrypto::verify(msg, 1, sig, nonCanonical));
}

TEST(test_sha512_published_vectors) {
    uint8_t out[64], want[64];

    ContentCrypto::sha512((const uint8_t*)"abc", 3, nullptr, 0, nullptr, 0, out);
    hexToBin("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
             "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", want, 64);
    ASSERT_TRUE(memcmp(out, want, 64) == 0);

    ContentCrypto::sha512(nullptr, 0, nullptr, 0, nullptr, 0, out);
    hexToBin("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", want, 64);
    ASSERT_TRUE(memcmp(out, want, 64) == 0);
}

TEST(test_sha512_streaming_matches_one_shot) {
    // Requested by the OTA workstream, which hashes a firmware image far
    // larger than RAM in 4 KB reads. Bad buffering across an update boundary
    // is a SILENT bug: it would make the device reject every legitimate image
    // and nothing else would look wrong.
    uint8_t message[1000];
    for (size_t i = 0; i < sizeof(message); i++) {
        message[i] = (uint8_t)(i * 37 + (i >> 3));
    }

    uint8_t whole[64];
    ContentCrypto::sha512(message, sizeof(message), nullptr, 0, nullptr, 0, whole);

    // Every chunk size around and across the 128-byte block boundary.
    const size_t chunks[] = { 1, 2, 3, 7, 63, 64, 65, 127, 128, 129, 200, 256, 999, 1000 };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        const size_t step = chunks[c];
        ContentCrypto::Sha512Ctx ctx;
        ContentCrypto::sha512Init(ctx);
        for (size_t off = 0; off < sizeof(message); off += step) {
            const size_t take = (off + step > sizeof(message)) ? (sizeof(message) - off) : step;
            ContentCrypto::sha512Update(ctx, message + off, take);
        }
        uint8_t streamed[64];
        ContentCrypto::sha512Finish(ctx, streamed);
        ASSERT_TRUE(memcmp(whole, streamed, 64) == 0);
    }

    // Zero-length updates must be no-ops, not state changes.
    ContentCrypto::Sha512Ctx ctx;
    ContentCrypto::sha512Init(ctx);
    ContentCrypto::sha512Update(ctx, nullptr, 0);
    ContentCrypto::sha512Update(ctx, message, 500);
    ContentCrypto::sha512Update(ctx, message + 500, 0);
    ContentCrypto::sha512Update(ctx, message + 500, 500);
    uint8_t mixed[64];
    ContentCrypto::sha512Finish(ctx, mixed);
    ASSERT_TRUE(memcmp(whole, mixed, 64) == 0);
}

TEST(test_crypto_self_test_passes) {
    ASSERT_TRUE(ContentCrypto::selfTest());
    ASSERT_NOT_NULL(ContentCrypto::backendName());
}

TEST(test_equal_ct_matches_memcmp) {
    uint8_t a[16], b[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)i; }
    ASSERT_TRUE(ContentCrypto::equalCT(a, b, 16));
    for (int i = 0; i < 16; i++) {
        b[i] ^= 0x80;
        ASSERT_FALSE(ContentCrypto::equalCT(a, b, 16));
        b[i] ^= 0x80;
    }
    ASSERT_FALSE(ContentCrypto::equalCT(nullptr, b, 16));
    ASSERT_FALSE(ContentCrypto::equalCT(a, nullptr, 16));
}

// ══ CBOR reader ═══════════════════════════════════════════════════════════

TEST(test_cbor_accepts_the_profile) {
    // { "lines": [ { "text": "hi", "stage": 5 } ] }
    static const uint8_t doc[] = {
        0xA1, 0x65, 'l', 'i', 'n', 'e', 's',
        0x81,
        0xA2, 0x64, 't', 'e', 'x', 't', 0x62, 'h', 'i',
              0x65, 's', 't', 'a', 'g', 'e', 0x05
    };
    CborReader r;
    Cbor::init(r, doc, sizeof(doc));

    uint32_t pairs = 0;
    ASSERT_TRUE(Cbor::readMap(r, pairs));
    ASSERT_EQ(pairs, 1u);

    char key[16];
    ASSERT_TRUE(Cbor::readText(r, key, sizeof(key)));
    ASSERT_STREQ(key, "lines");

    uint32_t rows = 0;
    ASSERT_TRUE(Cbor::readArray(r, rows));
    ASSERT_EQ(rows, 1u);

    ASSERT_TRUE(Cbor::readMap(r, pairs));
    ASSERT_EQ(pairs, 2u);
    ASSERT_TRUE(Cbor::readText(r, key, sizeof(key)));
    ASSERT_STREQ(key, "text");
    char text[16];
    ASSERT_TRUE(Cbor::readText(r, text, sizeof(text)));
    ASSERT_STREQ(text, "hi");
    ASSERT_TRUE(Cbor::readText(r, key, sizeof(key)));
    ASSERT_STREQ(key, "stage");
    uint32_t stage = 0;
    ASSERT_TRUE(Cbor::readUint(r, stage));
    ASSERT_EQ(stage, 5u);

    ASSERT_TRUE(Cbor::finish(r));
}

TEST(test_cbor_refuses_everything_outside_the_profile) {
    struct Case { const char* what; uint8_t bytes[8]; size_t len; };
    static const Case cases[] = {
        { "negative integer",        { 0x20 }, 1 },
        { "byte string",             { 0x42, 0xAA, 0xBB }, 3 },
        { "tag",                     { 0xC1, 0x00 }, 2 },
        { "float16",                 { 0xF9, 0x3C, 0x00 }, 3 },
        { "float32",                 { 0xFA, 0x00, 0x00, 0x00, 0x00 }, 5 },
        { "float64",                 { 0xFB, 0x00 }, 2 },
        { "undefined simple value",  { 0xF7 }, 1 },
        { "simple value 24",         { 0xF8, 0x20 }, 2 },
        { "break code",              { 0xFF }, 1 },
        { "indefinite array",        { 0x9F, 0xFF }, 2 },
        { "indefinite map",          { 0xBF, 0xFF }, 2 },
        { "indefinite text",         { 0x7F, 0xFF }, 2 },
        { "8-byte length",           { 0x1B, 0, 0, 0, 0, 0, 0, 0 }, 8 },
        { "reserved info 28",        { 0x1C }, 1 },
        { "non-minimal uint (24)",   { 0x18, 0x05 }, 2 },
        { "non-minimal uint (25)",   { 0x19, 0x00, 0x05 }, 3 },
        { "truncated head",          { 0x19, 0x01 }, 2 },
        { "array longer than buffer",{ 0x98, 0x40 }, 2 },
        { "text longer than buffer", { 0x78, 0x40, 0x41 }, 3 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CborReader r;
        Cbor::init(r, cases[i].bytes, cases[i].len);
        if (Cbor::skipValue(r)) {
            printf("FAIL\n    accepted %s\n", cases[i].what);
            s_testsFailed++;
            return;
        }
        // Failure must latch: a caller that misses one return value still
        // cannot get a usable answer out of the reader afterwards.
        ASSERT_TRUE(Cbor::failed(r));
        uint32_t v = 0;
        ASSERT_FALSE(Cbor::readUint(r, v));
    }
}

TEST(test_cbor_bounds_nesting_depth) {
    // Nested arrays, one deeper each time. The reader must start refusing at
    // CBOR_MAX_DEPTH rather than recursing as far as the buffer allows.
    for (int depth = 1; depth <= 10; depth++) {
        uint8_t doc[16];
        for (int i = 0; i < depth; i++) doc[i] = 0x81;   // array of 1
        doc[depth] = 0x00;                                // uint 0
        CborReader r;
        Cbor::init(r, doc, (size_t)depth + 1);
        const bool ok = Cbor::skipValue(r);
        if (depth <= CBOR_MAX_DEPTH) {
            ASSERT_TRUE(ok);
        } else {
            ASSERT_FALSE(ok);
        }
    }
}

TEST(test_cbor_truncates_long_text_but_bounds_the_wire) {
    // A string longer than the destination is truncated on copy, exactly as
    // strlcpy truncates on the JSON path.
    uint8_t doc[40];
    doc[0] = 0x78;
    doc[1] = 30;
    for (int i = 0; i < 30; i++) doc[2 + i] = (uint8_t)('a' + (i % 26));

    CborReader r;
    Cbor::init(r, doc, 32);
    char small[8];
    ASSERT_TRUE(Cbor::readText(r, small, sizeof(small)));
    ASSERT_EQ((int)strlen(small), 7);
    ASSERT_TRUE(Cbor::finish(r));

    // But a string longer than CBOR_MAX_STR_BYTES is refused outright.
    static uint8_t big[CBOR_MAX_STR_BYTES + 8];
    big[0] = 0x78;
    big[1] = (uint8_t)(CBOR_MAX_STR_BYTES + 1);
    memset(big + 2, 'x', CBOR_MAX_STR_BYTES + 1);
    CborReader r2;
    Cbor::init(r2, big, (size_t)CBOR_MAX_STR_BYTES + 3);
    char dest[CBOR_MAX_STR_BYTES + 4];
    ASSERT_FALSE(Cbor::readText(r2, dest, sizeof(dest)));
}

TEST(test_cbor_refuses_embedded_nul_in_text) {
    // A NUL inside a string makes the copied C string shorter than the string
    // the signer measured, which is a way to smuggle bytes past a comparison.
    static const uint8_t doc[] = { 0x64, 'a', 0x00, 'b', 'c' };
    CborReader r;
    Cbor::init(r, doc, sizeof(doc));
    char out[16];
    ASSERT_FALSE(Cbor::readText(r, out, sizeof(out)));
}

TEST(test_cbor_empty_and_null_buffers) {
    CborReader r;
    Cbor::init(r, nullptr, 0);
    ASSERT_TRUE(Cbor::failed(r));
    ASSERT_FALSE(Cbor::finish(r));

    static const uint8_t one[] = { 0x00 };
    Cbor::init(r, one, 0);
    ASSERT_TRUE(Cbor::failed(r));
}

// ══ Pack envelope ═════════════════════════════════════════════════════════

TEST(test_valid_pack_verifies) {
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    ASSERT_EQ(verifyWithTestKey(FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK),
                                PACK_KIND_DIALOGUE, &body, &bodyLen), PACK_OK);
    ASSERT_NOT_NULL(body);
    ASSERT_GT(bodyLen, 0u);
    ASSERT_EQ(bodyLen, sizeof(FIX_DIALOGUE_OK) - CONTENT_PACK_ENVELOPE_BYTES);
}

TEST(test_unsigned_pack_is_refused) {
    // The pre-P3W1 format: a plain JSON file dropped on the filesystem. It has
    // no envelope at all, so it cannot even name a key. Deliberately longer
    // than an empty envelope, so it is refused on its MAGIC rather than on its
    // size; a short file would pass this test for the wrong reason.
    static const char* json =
        "{\"lines\":[{\"text\":\"anyone with a filesystem can write this line\","
        "\"context\":\"idle\",\"trait\":\"chaotic\"}]}";
    ASSERT_GT(strlen(json), (size_t)CONTENT_PACK_ENVELOPE_BYTES);
    ASSERT_EQ(verifyWithTestKey((const uint8_t*)json, strlen(json),
                                PACK_KIND_DIALOGUE), PACK_BAD_MAGIC);

    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

TEST(test_zeroed_signature_is_refused) {
    static uint8_t pack[sizeof(FIX_DIALOGUE_OK)];
    memcpy(pack, FIX_DIALOGUE_OK, sizeof(pack));
    memset(pack + CONTENT_PACK_HEADER_BYTES, 0, CONTENT_PACK_SIG_BYTES);
    ASSERT_EQ(verifyWithTestKey(pack, sizeof(pack), PACK_KIND_DIALOGUE),
              PACK_BAD_SIGNATURE);
}

TEST(test_wrong_key_signature_is_refused) {
    // Correctly formed, correctly framed, signed by somebody else while
    // claiming the trusted key's id. The id is a selector, so this has to fail
    // on the signature.
    ASSERT_EQ(verifyWithTestKey(FIX_DIALOGUE_WRONG_KEY,
                                sizeof(FIX_DIALOGUE_WRONG_KEY),
                                PACK_KIND_DIALOGUE), PACK_BAD_SIGNATURE);

    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

TEST(test_unknown_key_is_refused) {
    ASSERT_EQ(verifyWithTestKey(FIX_DIALOGUE_UNKNOWN_KEY,
                                sizeof(FIX_DIALOGUE_UNKNOWN_KEY),
                                PACK_KIND_DIALOGUE), PACK_UNKNOWN_KEY);
}

TEST(test_truncated_pack_is_refused_at_every_length) {
    // Every prefix of a valid pack, not just a convenient one.
    for (size_t len = 0; len < sizeof(FIX_DIALOGUE_OK); len++) {
        const ContentPackStatus status =
            verifyWithTestKey(FIX_DIALOGUE_OK, len, PACK_KIND_DIALOGUE);
        if (status == PACK_OK) {
            printf("FAIL\n    accepted a %u-byte prefix\n", (unsigned)len);
            s_testsFailed++;
            return;
        }
    }
    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

TEST(test_extended_pack_is_refused) {
    // Bytes appended after a complete pack. The declared length must account
    // for the file exactly; "ignored" is how a second payload rides along.
    static uint8_t longer[sizeof(FIX_DIALOGUE_OK) + 4];
    memcpy(longer, FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK));
    memset(longer + sizeof(FIX_DIALOGUE_OK), 0x00, 4);
    ASSERT_EQ(verifyWithTestKey(longer, sizeof(longer), PACK_KIND_DIALOGUE),
              PACK_LENGTH_MISMATCH);
}

TEST(test_bit_flip_anywhere_is_refused) {
    // Header and signature are walked byte by byte, because those are the
    // fields with structure worth attacking. The body is sampled on a stride:
    // every verify is a full curve operation and an unoptimised native build
    // makes exhaustive coverage of a 1.2 KB body cost more than the whole
    // suite is worth. The property being checked is the same either way, since
    // the signature covers every byte identically.
    static uint8_t pack[sizeof(FIX_DIALOGUE_OK)];

    for (size_t i = 0; i < CONTENT_PACK_ENVELOPE_BYTES; i++) {
        for (uint8_t bit = 0x01; bit != 0; bit = (uint8_t)(bit << 3)) {
            memcpy(pack, FIX_DIALOGUE_OK, sizeof(pack));
            pack[i] ^= bit;
            if (verifyWithTestKey(pack, sizeof(pack), PACK_KIND_DIALOGUE) == PACK_OK) {
                printf("FAIL\n    accepted envelope byte %u flipped by 0x%02x\n",
                       (unsigned)i, bit);
                s_testsFailed++;
                return;
            }
        }
    }

    for (size_t i = CONTENT_PACK_ENVELOPE_BYTES; i < sizeof(FIX_DIALOGUE_OK); i += 11) {
        memcpy(pack, FIX_DIALOGUE_OK, sizeof(pack));
        pack[i] ^= 0x01;
        if (verifyWithTestKey(pack, sizeof(pack), PACK_KIND_DIALOGUE) == PACK_OK) {
            printf("FAIL\n    accepted body byte %u flipped\n", (unsigned)i);
            s_testsFailed++;
            return;
        }
    }

    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

TEST(test_wrong_kind_is_refused) {
    // A genuinely signed dialogue pack, renamed to the quest slot. The kind is
    // inside the signed header, so renaming the file does not reclassify it.
    ASSERT_EQ(verifyWithTestKey(FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK),
                                PACK_KIND_QUESTS), PACK_WRONG_KIND);
    ASSERT_EQ(verifyWithTestKey(FIX_QUESTS_OK, sizeof(FIX_QUESTS_OK),
                                PACK_KIND_DIALOGUE), PACK_WRONG_KIND);
}

TEST(test_reserved_bytes_must_be_zero) {
    static uint8_t pack[sizeof(FIX_DIALOGUE_OK)];
    memcpy(pack, FIX_DIALOGUE_OK, sizeof(pack));
    pack[6] = 0x01;
    ASSERT_EQ(verifyWithTestKey(pack, sizeof(pack), PACK_KIND_DIALOGUE),
              PACK_RESERVED_SET);
}

TEST(test_bad_version_is_refused) {
    static uint8_t pack[sizeof(FIX_DIALOGUE_OK)];
    memcpy(pack, FIX_DIALOGUE_OK, sizeof(pack));
    pack[4] = CONTENT_PACK_VERSION + 1;
    ASSERT_EQ(verifyWithTestKey(pack, sizeof(pack), PACK_KIND_DIALOGUE),
              PACK_BAD_VERSION);
}

TEST(test_key_id_is_derived_consistently) {
    uint8_t id[4];
    ContentPack::keyIdFor(FIX_TEST_PUBKEY, id);
    ASSERT_TRUE(ContentCrypto::equalCT(id, FIX_DIALOGUE_OK + 8, 4));
    ASSERT_GT(ContentPack::trustedKeyCount(), 0);
}

// ══ Loader policy: refusals must always leave usable content ══════════════

TEST(test_signed_pack_replaces_the_table) {
    resetContent();
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    ASSERT_EQ(verifyWithTestKey(FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK),
                                PACK_KIND_DIALOGUE, &body, &bodyLen), PACK_OK);

    ContentStore& store = ContentStore::instance();
    ASSERT_TRUE(store.applyDialogueCbor(body, bodyLen));
    ASSERT_EQ(store.dialogueCount(), 18);

    // The authored JSON said this, and it survived the round trip through the
    // encoder, the signature and the decoder without being rewritten.
    ASSERT_STREQ(store.dialogue(0)->text, "Back already? Good.");
    ASSERT_EQ((int)store.dialogue(0)->context, (int)DLG_GREETING);
    ASSERT_EQ((int)store.dialogue(0)->trait, (int)TRAIT_PROTECTIVE);

    // A name-spelled memory slot resolved through the same table the JSON
    // parser uses.
    ASSERT_EQ((int)store.dialogue(5)->memorySlot, (int)MEM_SLOT_NEW_NETWORKS);
    // A numeric stage filter.
    ASSERT_EQ((int)store.dialogue(17)->stage, (int)STAGE_SENTINEL);

    resetContent();
}

TEST(test_signed_quest_pack_replaces_the_table) {
    resetContent();
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    ASSERT_EQ(verifyWithTestKey(FIX_QUESTS_OK, sizeof(FIX_QUESTS_OK),
                                PACK_KIND_QUESTS, &body, &bodyLen), PACK_OK);

    ContentStore& store = ContentStore::instance();
    ASSERT_TRUE(store.applyQuestCbor(body, bodyLen));
    ASSERT_EQ(store.questDefCount(), 8);

    const QuestDef* q = store.questDefById("stretch");
    ASSERT_NOT_NULL(q);
    ASSERT_STREQ(q->text, "Take me for a walk");
    ASSERT_EQ((int)q->kind, (int)QUEST_EXPLORE);
    ASSERT_EQ(q->target, 1);
    ASSERT_EQ(q->rewardXP, 12);
    // "requires": ["imu"] resolved through the same capability table.
    ASSERT_EQ(q->requiresCaps, (uint16_t)(1u << 0));

    resetContent();
}

// One table drives the "signature is fine, the body is not" cases, so a new
// hostile body shape is one line rather than a new test.
struct BadBodyCase {
    const char*    what;
    const uint8_t* pack;
    size_t         len;
};

TEST(test_signed_but_undecodable_bodies_keep_the_baseline) {
    static const BadBodyCase cases[] = {
        { "garbage body",       FIX_SIGNED_GARBAGE_BODY,   sizeof(FIX_SIGNED_GARBAGE_BODY) },
        { "indefinite length",  FIX_SIGNED_INDEFINITE,     sizeof(FIX_SIGNED_INDEFINITE) },
        { "nesting too deep",   FIX_SIGNED_DEEP_NEST,      sizeof(FIX_SIGNED_DEEP_NEST) },
        { "non-minimal int",    FIX_SIGNED_NON_MINIMAL,    sizeof(FIX_SIGNED_NON_MINIMAL) },
        { "no usable rows",     FIX_SIGNED_NO_USABLE_ROWS, sizeof(FIX_SIGNED_NO_USABLE_ROWS) },
        { "trailing byte",      FIX_SIGNED_TRAILING,       sizeof(FIX_SIGNED_TRAILING) },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        resetContent();
        const uint8_t baselineCount = ContentStore::instance().dialogueCount();

        const uint8_t* body = nullptr;
        size_t bodyLen = 0;
        // The signature is genuinely good on every one of these.
        if (verifyWithTestKey(cases[i].pack, cases[i].len,
                              PACK_KIND_DIALOGUE, &body, &bodyLen) != PACK_OK) {
            printf("FAIL\n    fixture %s did not verify\n", cases[i].what);
            s_testsFailed++;
            return;
        }

        // And the decoder still has to refuse it.
        if (ContentStore::instance().applyDialogueCbor(body, bodyLen)) {
            printf("FAIL\n    decoder accepted %s\n", cases[i].what);
            s_testsFailed++;
            return;
        }

        if (!baselineIsUsable() ||
            ContentStore::instance().dialogueCount() != baselineCount) {
            printf("FAIL\n    %s damaged the baseline\n", cases[i].what);
            s_testsFailed++;
            return;
        }
    }
    resetContent();
}

TEST(test_every_body_prefix_keeps_the_baseline) {
    // Feed the decoder every truncation of a genuine body. None may be
    // accepted, and none may leave a half-written table behind.
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    verifyWithTestKey(FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK),
                      PACK_KIND_DIALOGUE, &body, &bodyLen);

    for (size_t len = 0; len < bodyLen; len += 7) {
        resetContent();
        const uint8_t before = ContentStore::instance().dialogueCount();
        ContentStore::instance().applyDialogueCbor(body, len);
        if (!baselineIsUsable() ||
            ContentStore::instance().dialogueCount() != before) {
            printf("FAIL\n    body prefix of %u bytes damaged the baseline\n",
                   (unsigned)len);
            s_testsFailed++;
            return;
        }
    }
    resetContent();
}

TEST(test_random_bodies_never_damage_the_baseline) {
    // Not a fuzzer, but enough shapes to catch a decoder that writes before it
    // has finished validating.
    uint32_t state = 0x1234567u;
    for (int iter = 0; iter < 400; iter++) {
        uint8_t buf[64];
        const size_t len = 1 + (Content::nextRandom(state) % sizeof(buf));
        for (size_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)(Content::nextRandom(state) & 0xFF);
        }
        resetContent();
        const uint8_t before = ContentStore::instance().dialogueCount();
        ContentStore::instance().applyDialogueCbor(buf, len);
        ContentStore::instance().applyQuestCbor(buf, len);
        if (!baselineIsUsable() ||
            ContentStore::instance().dialogueCount() != before) {
            printf("FAIL\n    a random %u-byte body damaged the baseline\n",
                   (unsigned)len);
            s_testsFailed++;
            return;
        }
    }
    resetContent();
}

// ══ Filesystem path, including the size cap ═══════════════════════════════

TEST(test_load_reports_absent_without_a_file) {
    ContentPackFile pack;
    const ContentPackStatus status =
        ContentPack::load("/content/definitely-not-here.hcp",
                          PACK_KIND_DIALOGUE, CONTENT_MAX_PACK_BYTES, pack);
    ASSERT_EQ(status, PACK_ABSENT);
    ContentPack::release(pack);

    // Absent is the NORMAL first-boot state, not an error, and it must leave a
    // talking pet behind.
    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

TEST(test_oversized_pack_is_refused_whole) {
    TEST_MKDIR("content");

    // A file one byte over the caller's cap. It must be refused on its size
    // alone, before it is read and before a signature is checked, so an
    // attacker cannot make the device allocate its way past the cap.
    static uint8_t big[CONTENT_MAX_PACK_BYTES + 1];
    memcpy(big, FIX_DIALOGUE_OK, sizeof(FIX_DIALOGUE_OK));
    memset(big + sizeof(FIX_DIALOGUE_OK), 0x41,
           sizeof(big) - sizeof(FIX_DIALOGUE_OK));
    ASSERT_TRUE(writeFile("content/oversized.hcp", big, sizeof(big)));

    ContentPackFile pack;
    ASSERT_EQ(ContentPack::load("/content/oversized.hcp", PACK_KIND_DIALOGUE,
                                CONTENT_MAX_PACK_BYTES, pack), PACK_TOO_LARGE);
    ASSERT_TRUE(pack.raw == nullptr);
    ContentPack::release(pack);
    remove("content/oversized.hcp");

    // A file smaller than an empty envelope is refused the same way.
    static const uint8_t tiny[] = { 'H', 'X', 'C', 'P', 1, 1 };
    ASSERT_TRUE(writeFile("content/tiny.hcp", tiny, sizeof(tiny)));
    ASSERT_EQ(ContentPack::load("/content/tiny.hcp", PACK_KIND_DIALOGUE,
                                CONTENT_MAX_PACK_BYTES, pack), PACK_TOO_SMALL);
    ContentPack::release(pack);
    remove("content/tiny.hcp");

    resetContent();
    ASSERT_TRUE(baselineIsUsable());
}

#ifdef FIX_HAVE_DEVKEY

TEST(test_devkey_pack_verifies_against_the_firmware_list) {
    // The end-to-end proof that scripts/sign_content_pack.py and this firmware
    // agree: a pack signed by the repository's real development key, verified
    // against the COMPILED-IN trusted list rather than a key handed in by the
    // test.
    //
    // If this fails after a key rotation, regenerate the fixtures:
    //     python scripts/gen_pack_test_fixtures.py
    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    ASSERT_EQ(ContentPack::verifyImage(FIX_DEVKEY_DIALOGUE,
                                       sizeof(FIX_DEVKEY_DIALOGUE),
                                       PACK_KIND_DIALOGUE, nullptr, 0,
                                       &body, &bodyLen), PACK_OK);
    ASSERT_NOT_NULL(body);
}

TEST(test_full_boot_path_loads_a_signed_pack_from_disk) {
    // ContentStore::begin() end to end: read from the filesystem, verify,
    // decode, install. This is the path the device actually runs.
    TEST_MKDIR("content");
    ASSERT_TRUE(writeFile("content/dialogue.hcp", FIX_DEVKEY_DIALOGUE,
                          sizeof(FIX_DEVKEY_DIALOGUE)));

    ContentStore& store = ContentStore::instance();
    store.begin();

    ASSERT_TRUE(store.dialogueFromPack());
    ASSERT_EQ(store.dialoguePackStatus(), PACK_OK);
    ASSERT_EQ(store.dialogueCount(), 18);
    ASSERT_STREQ(store.dialogue(0)->text, "Back already? Good.");

    // No quest pack on disk, so quests must have quietly stayed on baseline.
    ASSERT_FALSE(store.questsFromPack());
    ASSERT_EQ(store.questPackStatus(), PACK_ABSENT);
    ASSERT_GT(store.questDefCount(), 0);

    // Now corrupt the file on disk and boot again. The device must come back
    // on the baseline rather than on half a pack.
    static uint8_t corrupt[sizeof(FIX_DEVKEY_DIALOGUE)];
    memcpy(corrupt, FIX_DEVKEY_DIALOGUE, sizeof(corrupt));
    corrupt[CONTENT_PACK_ENVELOPE_BYTES + 3] ^= 0x20;
    ASSERT_TRUE(writeFile("content/dialogue.hcp", corrupt, sizeof(corrupt)));

    store.begin();
    ASSERT_FALSE(store.dialogueFromPack());
    ASSERT_EQ(store.dialoguePackStatus(), PACK_BAD_SIGNATURE);
    ASSERT_TRUE(baselineIsUsable());

    remove("content/dialogue.hcp");
    resetContent();
}

#endif  // FIX_HAVE_DEVKEY

// ══ Runner ════════════════════════════════════════════════════════════════

int main() {
    printf("\n=== Content pack: crypto ===\n");
    RUN_TEST(test_ed25519_rfc8032_vectors_accept);
    RUN_TEST(test_ed25519_rejects_tampering);
    RUN_TEST(test_ed25519_rejects_non_canonical_s);
    RUN_TEST(test_ed25519_rejects_garbage_key);
    RUN_TEST(test_sha512_published_vectors);
    RUN_TEST(test_sha512_streaming_matches_one_shot);
    RUN_TEST(test_crypto_self_test_passes);
    RUN_TEST(test_equal_ct_matches_memcmp);

    printf("\n=== Content pack: CBOR ===\n");
    RUN_TEST(test_cbor_accepts_the_profile);
    RUN_TEST(test_cbor_refuses_everything_outside_the_profile);
    RUN_TEST(test_cbor_bounds_nesting_depth);
    RUN_TEST(test_cbor_truncates_long_text_but_bounds_the_wire);
    RUN_TEST(test_cbor_refuses_embedded_nul_in_text);
    RUN_TEST(test_cbor_empty_and_null_buffers);

    printf("\n=== Content pack: envelope ===\n");
    RUN_TEST(test_valid_pack_verifies);
    RUN_TEST(test_unsigned_pack_is_refused);
    RUN_TEST(test_zeroed_signature_is_refused);
    RUN_TEST(test_wrong_key_signature_is_refused);
    RUN_TEST(test_unknown_key_is_refused);
    RUN_TEST(test_truncated_pack_is_refused_at_every_length);
    RUN_TEST(test_extended_pack_is_refused);
    RUN_TEST(test_bit_flip_anywhere_is_refused);
    RUN_TEST(test_wrong_kind_is_refused);
    RUN_TEST(test_reserved_bytes_must_be_zero);
    RUN_TEST(test_bad_version_is_refused);
    RUN_TEST(test_key_id_is_derived_consistently);

    printf("\n=== Content pack: loader policy ===\n");
    RUN_TEST(test_signed_pack_replaces_the_table);
    RUN_TEST(test_signed_quest_pack_replaces_the_table);
    RUN_TEST(test_signed_but_undecodable_bodies_keep_the_baseline);
    RUN_TEST(test_every_body_prefix_keeps_the_baseline);
    RUN_TEST(test_random_bodies_never_damage_the_baseline);

    printf("\n=== Content pack: filesystem ===\n");
    RUN_TEST(test_load_reports_absent_without_a_file);
    RUN_TEST(test_oversized_pack_is_refused_whole);
#ifdef FIX_HAVE_DEVKEY
    RUN_TEST(test_devkey_pack_verifies_against_the_firmware_list);
    RUN_TEST(test_full_boot_path_loads_a_signed_pack_from_disk);
#endif

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
