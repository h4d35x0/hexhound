#pragma once

#include <stdint.h>
#include <stddef.h>

#include "content_crypto.h"

// ── HexHound - Signed Content Pack Envelope ──────────────────────
//
// One reader for every content pack in the firmware: dialogue, quests and
// recipes. Before this existed, content_store.cpp and pet_inventory.cpp each
// carried their own near-identical copy of a SPIFFS-or-stdio file reader; the
// duplication was accepted while two workstreams ran in parallel and is
// resolved here.
//
// ── On-disk format ────────────────────────────────────────────────────────
//
//   offset  size  field
//   0       4     magic "HXCP"
//   4       1     format version, must be CONTENT_PACK_VERSION
//   5       1     pack kind, must match what the caller asked for
//   6       2     reserved, must be zero
//   8       4     key id: first 4 bytes of SHA-512(public key)
//   12      4     body length, little-endian uint32
//   16      64    Ed25519 signature over bytes [0,16) followed by the body
//   80      ...   CBOR body
//
// The signature covers the HEADER as well as the body. That is what stops a
// signed dialogue pack being renamed to quests.hcp, and what makes the
// declared body length as trustworthy as the content behind it.
//
// The key id is a selector, not a credential. It says which trusted key to
// try so that a key rotation gives a clear "unknown key" instead of a
// mystifying "bad signature". Forging it buys nothing: the signature still
// has to verify under the key it names.
//
// ── Order of operations, which is the whole point ─────────────────────────
//
//   1. bound the file size, refuse anything oversized WHOLE
//   2. read it
//   3. check the fixed header fields
//   4. verify the signature over header+body
//   5. only then hand the body to the CBOR decoder
//
// Nothing at step 5 runs on bytes that did not survive step 4. Parsing
// attacker-structured data and asking permission afterwards is the failure
// this ordering exists to prevent.
//
// ── What a pack is allowed to be ──────────────────────────────────────────
//
// Data, and only data. A pack names dialogue lines, quest definitions and
// recipes. There is no field for a path, a command, a key, a pin, a URL or a
// HID payload, and no decoder here can produce one. A valid signature buys an
// author the right to change what the pet SAYS and what it ASKS FOR. It does
// not reach recon logs, security settings, the HID subsystem or the radios.

#define CONTENT_PACK_MAGIC_0     'H'
#define CONTENT_PACK_MAGIC_1     'X'
#define CONTENT_PACK_MAGIC_2     'C'
#define CONTENT_PACK_MAGIC_3     'P'

#define CONTENT_PACK_VERSION       1
#define CONTENT_PACK_HEADER_BYTES 16
#define CONTENT_PACK_SIG_BYTES    ED25519_SIG_BYTES
#define CONTENT_PACK_ENVELOPE_BYTES (CONTENT_PACK_HEADER_BYTES + CONTENT_PACK_SIG_BYTES)

enum ContentPackKind : uint8_t {
    PACK_KIND_DIALOGUE = 1,
    PACK_KIND_QUESTS   = 2,
    PACK_KIND_RECIPES  = 3
};

// Every way a pack can be turned away. Distinct values because the serial log
// is the only tool a person has when a pack they just signed does not load,
// and "rejected" on its own sends them looking in the wrong place.
enum ContentPackStatus : uint8_t {
    PACK_OK = 0,
    PACK_ABSENT,            // no such file; the normal first-boot state
    PACK_READ_FAILED,
    PACK_TOO_SMALL,         // smaller than an empty envelope
    PACK_TOO_LARGE,         // over the caller's cap, refused whole
    PACK_NO_MEMORY,
    PACK_BAD_MAGIC,
    PACK_BAD_VERSION,
    PACK_RESERVED_SET,      // a reserved byte was non-zero
    PACK_WRONG_KIND,        // a real pack, but not the one that was asked for
    PACK_LENGTH_MISMATCH,   // declared body length does not match the file
    PACK_UNKNOWN_KEY,       // no trusted key has that key id
    PACK_BAD_SIGNATURE,     // the key is trusted, the signature is not valid
    PACK_STATUS_COUNT
};

// A pack held in RAM. `raw` is the single allocation; `body` points inside it.
// Always pass this to release(), including after a failed load.
struct ContentPackFile {
    uint8_t*       raw     = nullptr;
    size_t         rawLen  = 0;
    const uint8_t* body    = nullptr;
    size_t         bodyLen = 0;
};

namespace ContentPack {

// Reads, bounds and verifies a pack from the filesystem. On PACK_OK the
// caller owns `out` until release(). On anything else `out` is empty and the
// caller must keep whatever content it already had.
//
// maxBytes is the caller's own cap, because the recipe pack is allowed less
// than the dialogue pack. It is applied BEFORE the file is read, so an
// oversized pack costs one stat and nothing else.
//
// Boot-time only. It makes one transient allocation of the whole file, which
// is unavoidable: a signature cannot be checked over bytes that have not all
// been seen. That allocation is freed before the caller returns.
ContentPackStatus load(const char* path, uint8_t kind, size_t maxBytes,
                       ContentPackFile& out);

void release(ContentPackFile& out);

// The verification half of load(), on a buffer that is already in RAM. This is
// what the unit tests drive, and what a future serial or BLE content push
// would call: there must be exactly one implementation of "is this pack
// acceptable", or the paths will drift and the weakest one will be the one
// that matters.
//
// `keys` may be null to use the firmware's compiled-in trusted list.
ContentPackStatus verifyImage(const uint8_t* image, size_t len, uint8_t kind,
                              const uint8_t (*keys)[ED25519_PUBKEY_BYTES],
                              uint8_t keyCount,
                              const uint8_t** body, size_t* bodyLen);

// Number of keys compiled into this firmware, for diagnostics and tests.
uint8_t trustedKeyCount();

// Key id (4 bytes) of a public key. Exposed so a test can build a pack that
// names a key the firmware does not have.
void keyIdFor(const uint8_t pubkey[ED25519_PUBKEY_BYTES], uint8_t out[4]);

const char* statusName(ContentPackStatus status);

}  // namespace ContentPack
