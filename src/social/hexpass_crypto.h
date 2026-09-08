#pragma once

#include <stdint.h>
#include <stddef.h>

// ── HexHound - HexPass Crypto Primitives ─────────────────────────
//
// HMAC-SHA256, and nothing else. Every HexPass identifier is a truncated HMAC,
// so this is the one place a mistake would silently weaken all of them.
//
// ── Two backends, one answer ──────────────────────────────────────────────
//
// On an ESP32 the SDK already ships mbedtls, so there is no reason to carry a
// second SHA-256 into flash. Off-target (desktop simulator, native unit tests)
// there is no mbedtls, so a compact portable implementation stands in.
//
// The rule that matters: the two backends MUST produce identical bytes. They
// are not compared against each other directly, because no single build has
// both. They are each compared against the SAME published answers, RFC 4231's
// HMAC-SHA256 test vectors, which is a stronger check than agreeing with one
// another (two backends can agree and both be wrong).
//
//   * The native test suite runs the RFC vectors against the portable backend.
//   * selfTest() runs one RFC vector against WHICHEVER backend was compiled in,
//     so the hardware build can prove the same property on the device itself
//     rather than inheriting it from a test that never ran there.

// Which backend this translation unit gets. The simulator and the test runner
// are ordinary desktop programs with no ESP-IDF, so they take the portable
// path; everything else is an ESP32 build with mbedtls already linked.
#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)
#define HEXPASS_PORTABLE_CRYPTO 1
#else
#define HEXPASS_PORTABLE_CRYPTO 0
#endif

#define HEXPASS_SHA256_BYTES 32

namespace HexPassCrypto {

// out MUST have room for 32 bytes. Key and message may be any length,
// including zero.
void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t out[HEXPASS_SHA256_BYTES]);

// Comparison that does not return early on the first differing byte. Used for
// tag verification: a length-correlated reject leaks which prefix was right,
// which is the whole game when an attacker can retry.
bool equalCT(const uint8_t* a, const uint8_t* b, size_t len);

// True when the compiled-in backend reproduces RFC 4231 test case 2. Cheap
// enough to call at boot; the point is that the mbedtls path is verified on
// the device, not assumed from a desktop test run.
bool selfTest();

// Name of the backend actually compiled in, for the diagnostics screen. A user
// who cannot see what is running cannot audit it.
const char* backendName();

}  // namespace HexPassCrypto
