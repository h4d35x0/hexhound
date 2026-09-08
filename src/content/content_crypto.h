#pragma once

#include <stdint.h>
#include <stddef.h>

// ── HexHound - Content Pack Crypto Primitives ────────────────────
//
// SHA-512 and Ed25519 signature VERIFICATION. No signing, ever: a device that
// can sign a content pack is a device that can mint content for every other
// device, so the private half of the key never exists on this side of the
// wire and no code here could use it if it did.
//
// ── Two backends, one answer (and one honest caveat) ──────────────────────
//
// hexpass_crypto.h sets the pattern this file follows: mbedtls on target,
// a compact portable implementation off-target, both pinned to the same
// PUBLISHED test vectors rather than to each other.
//
// The caveat, stated plainly because it changes what "mbedtls on target"
// means here: mbedtls does NOT implement Ed25519. It has Curve25519 for ECDH
// and it has ECDSA over the NIST curves, but EdDSA is not in the library at
// any version ESP-IDF ships. So the split in this file is NOT the same split
// as hexpass_crypto's:
//
//   * SHA-512 has two backends. mbedtls on target, portable off-target.
//   * The Ed25519 group arithmetic is ONE implementation, compiled on every
//     target. There is no vendor alternative to fall back to.
//
// That means the vector tests carry more weight here than they do for
// HMAC-SHA256, because they are the only thing standing behind the curve
// code. They are RFC 8032 section 7.1 vectors, run against the portable path
// in the native suite and against the compiled-in path by selfTest() on the
// device itself.
//
// Nothing in here allocates. Peak stack for verify() is roughly 3 KB, which
// is the reason the caller is documented as boot-time only.

#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)
#define CONTENT_PORTABLE_SHA512 1
#else
#define CONTENT_PORTABLE_SHA512 0
#endif

#define ED25519_PUBKEY_BYTES  32
#define ED25519_SIG_BYTES     64
#define CONTENT_SHA512_BYTES  64

#if !CONTENT_PORTABLE_SHA512
#include <mbedtls/sha512.h>
#endif

namespace ContentCrypto {

// ── Incremental SHA-512 ───────────────────────────────────────────────────
//
// Exists because a caller may have to hash something far larger than RAM. The
// OTA path hashes a 1.2 MB firmware image back out of flash in 4 KB reads on a
// device with 250 KB of RAM and no PSRAM; there is no version of that which
// holds the message in memory.
//
// No allocation, no destructor requirement. Safe to hold as a stack local or
// as a member. Abandoning a context without calling finish leaks nothing.
//
// update() accepts any length including zero, and buffers across calls, so the
// split between calls has no effect on the answer. That property is not
// assumed: the one-shot sha512() below is IMPLEMENTED on top of this context
// rather than being a second copy of the algorithm, so the two cannot drift,
// and a native test still asserts a split input matches a whole one because
// the failure mode (bad buffering across an update boundary) is silent.
//
// On target this is mbedtls, which ESP-IDF backs with the ESP32-S3 hardware
// SHA accelerator.
struct Sha512Ctx {
#if CONTENT_PORTABLE_SHA512
    uint64_t h[8];
    uint64_t byteLen;
    uint8_t  buf[128];
    size_t   bufLen;
#else
    mbedtls_sha512_context md;
#endif
};

void sha512Init(Sha512Ctx& ctx);
void sha512Update(Sha512Ctx& ctx, const uint8_t* data, size_t len);
void sha512Finish(Sha512Ctx& ctx, uint8_t out[CONTENT_SHA512_BYTES]);

// Hash of a list of parts, hashed in order without being concatenated first.
// Ed25519 needs SHA-512(R || A || M) where M is the whole pack, and building
// that concatenation would mean a second buffer as large as the pack. Parts
// may be null/zero-length.
void sha512n(const uint8_t* const* parts, const size_t* lens, size_t count,
             uint8_t out[CONTENT_SHA512_BYTES]);

// Three-part convenience form.
void sha512(const uint8_t* a, size_t aLen,
            const uint8_t* b, size_t bLen,
            const uint8_t* c, size_t cLen,
            uint8_t out[CONTENT_SHA512_BYTES]);

// Comparison that does not return early on the first differing byte.
bool equalCT(const uint8_t* a, const uint8_t* b, size_t len);

// Ed25519 verification, RFC 8032 section 5.1.7, with two strictness rules
// added on top of the minimum:
//
//   * S is rejected unless it is canonically reduced (S < L). Without this a
//     third party can maul a valid signature into a different, also-valid
//     one. A content pack is identified to the user by its signature in the
//     tooling, so two signatures for one pack is a lie waiting to be told.
//   * The public key is rejected unless its y coordinate is canonical
//     (y < 2^255 - 19). Non-canonical encodings are a way to hand two
//     different byte strings that name the same key, which defeats key
//     pinning by keyId.
//
// Returns false for every malformed input. Never partially succeeds.
bool verify(const uint8_t* msg, size_t msgLen,
            const uint8_t sig[ED25519_SIG_BYTES],
            const uint8_t pubkey[ED25519_PUBKEY_BYTES]);

// Same, for a message that arrives in two pieces. A signed content pack is a
// header, then the signature, then the body, so the signed message is not
// contiguous on disk. Concatenating it first would mean a second allocation
// the size of the pack on a device with 250 KB of RAM and no PSRAM, so the
// split is carried through the hash instead.
bool verify2(const uint8_t* msg1, size_t msg1Len,
             const uint8_t* msg2, size_t msg2Len,
             const uint8_t sig[ED25519_SIG_BYTES],
             const uint8_t pubkey[ED25519_PUBKEY_BYTES]);

// True when the compiled-in code reproduces RFC 8032 TEST 2 (a one-byte
// message) and rejects that same signature against a flipped bit. Cheap
// enough to call at boot, and the only proof the DEVICE has that its curve
// code and its SHA-512 backend are both intact.
bool selfTest();

// Which SHA-512 backend was compiled in, for the diagnostics screen.
const char* backendName();

}  // namespace ContentCrypto
