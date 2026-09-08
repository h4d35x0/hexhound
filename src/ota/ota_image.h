#pragma once

#include <stdint.h>
#include <stddef.h>

#include "ota_identity.h"

// ── HexHound - Signed Firmware Envelope ─────────────────────────
//
// A HexHound update file (.hexfw) is exactly:
//
//     [ 192 byte header ][ raw ESP32 application image ]
//
// and this header is the only thing that decides whether those image bytes are
// ever allowed near a boot partition.
//
// ── Why a fixed binary header and not JSON ────────────────────────────────
//
// Because the header is SIGNED, and JSON is a bad thing to sign. Key order,
// whitespace, integer formatting and unicode escaping all vary between
// producers, so "the same document" is not the same bytes, and a verifier
// either re-serialises (and now the parser is part of the trust boundary) or
// signs the received text (and now a semantically different document can carry
// a valid signature). A fixed-offset little-endian record has exactly one
// byte-level representation. There is nothing to canonicalise and nothing to
// disagree about.
//
// The companion app may still ship a human-readable JSON sidecar for its own
// listing UI. That sidecar is decoration. The device verifies these bytes.
//
// ── Layout, little-endian ─────────────────────────────────────────────────
//
//     off  len  field
//     0    8    magic "HEXHOTA1"
//     8    2    headerVersion, must be 1
//     10   2    flags, reserved, must be 0
//     12   4    imageLen
//     16   32   boardId, NUL-padded ASCII
//     48   4    fwVersion, major<<16 | minor<<8 | patch
//     52   4    buildNumber
//     56   4    keyId, selects which trusted key to try
//     60   64   imageDigest, SHA-512 of the image bytes
//     124  4    reserved, must be 0
//     128  64   Ed25519 signature
//
// ── What the signature covers, and why it is not the image ────────────────
//
//     signature = Ed25519( "HEXHOUND-FW-v1\0" (16B, NUL padded) || header[0..128) )
//
// The signature is computed over the 128-byte prefix, not over the megabyte of
// image, and the image is bound to it because imageDigest and imageLen are
// both inside that prefix. This matters for three reasons:
//
//   1. One verify, over 144 bytes. Ed25519 here is a portable software
//      implementation on a 240 MHz Xtensa; it costs on the order of 100 ms.
//      Paying that once for a fixed-size blob is fine. Paying it over an
//      image, or per chunk, would not be.
//   2. The header can be verified BEFORE a single byte is written. The device
//      knows the update is authentic while the flash is still untouched, which
//      is the difference between refusing an update and half-installing one.
//   3. Length and digest are signed together, so truncation is not a separate
//      case to handle. A short image fails imageLen; an image that is the
//      right length but different fails the digest; an attacker who edits
//      either field breaks the signature.
//
// ── Domain separation ─────────────────────────────────────────────────────
//
// The 16-byte tag is not decoration. Without it the signed message is a bare
// structure that some other signing path might one day be induced to produce.
// With it, a signature from this key is only ever a statement about a HexHound
// firmware image, and a v2 header format gets a different tag so a v1 signature
// can never be replayed onto it.
//
// ── What this header deliberately does NOT do ─────────────────────────────
//
// There is no minimum-version field and no downgrade block. Refusing older
// firmware would remove the owner's only escape route from a bad release, on a
// device with no support channel and no remote management. The owner is
// allowed to go backwards. See docs/p3w2-wiring.md for the save-compatibility
// warning that comes with that, which is real and is the companion app's job
// to surface.

#define OTA_HEADER_BYTES         192
#define OTA_SIGNED_PREFIX_BYTES  128
#define OTA_MAGIC                "HEXHOTA1"
#define OTA_MAGIC_BYTES          8
#define OTA_HEADER_VERSION       1
#define OTA_BOARD_ID_BYTES       32
#define OTA_DIGEST_BYTES         64
#define OTA_SIG_BYTES            64
#define OTA_KEYID_BYTES          4
#define OTA_PUBKEY_BYTES         32

// The domain separation tag, padded with NULs to exactly 16 bytes. Changing
// this string invalidates every signature ever produced, which is precisely
// what should happen if the header format changes.
#define OTA_DOMAIN_TAG           "HEXHOUND-FW-v1"
#define OTA_DOMAIN_TAG_BYTES     16

// Smallest thing that could plausibly be a HexHound application image. A real
// build is over a megabyte; this only exists so a trivially tiny payload is
// rejected with a clear reason rather than being handed to esp_ota_begin.
#define OTA_MIN_IMAGE_BYTES      1024

namespace OtaImage {

// Every way an update can be refused. One value per distinct cause, because
// "update failed" on a device with no serial console is not a diagnosis, and
// the companion app renders these to the owner.
//
// ACCEPTED is the only value that permits a write.
//
// These numbers travel on the wire. Values 0 through 12 are the original set
// and their numbers are fixed forever; anything new is APPENDED, never
// inserted, so a companion app built against the old list still renders the
// old causes correctly and shows an unknown code for the rest.
enum Verdict : uint8_t {
    ACCEPTED = 0,
    SHORT_HEADER,      // fewer than 192 bytes offered
    BAD_MAGIC,         // not a HexHound update file at all
    BAD_HEADER_VERSION,// a newer envelope format this build cannot parse
    BAD_FLAGS,         // a flag bit this build does not understand is set
    BAD_RESERVED,      // reserved bytes not zero
    BOARD_MISMATCH,    // an image for different hardware
    IMAGE_TOO_SMALL,   // not plausibly an application image
    IMAGE_TOO_LARGE,   // will not fit the OTA slot
    UNKNOWN_KEY,       // keyId names no key this firmware trusts
    BAD_SIGNATURE,     // signature does not verify under the named key
    DIGEST_MISMATCH,   // written bytes are not the signed bytes
    CRYPTO_SELFTEST_FAILED, // the verifier itself is not trustworthy here

    // ── Session causes, appended ──────────────────────────────────────────
    //
    // The values above are all reasons an IMAGE was refused. The ones below
    // are reasons the SESSION refused, and they exist because the state
    // machine was previously reporting them as ACCEPTED or as an unrelated
    // image fault. A refusal rendered to the owner as "accepted", or as "this
    // update is too big" when the truth was "this device has nowhere to put
    // it", is worse than no message: it sends them to fix the wrong thing.
    NOT_ARMED = 13,        // offered before the owner confirmed on the device
    UPDATE_ON_TRIAL,       // 14 the running image is still proving itself
    NO_UPDATE_SLOT,        // 15 no usable partition to install into
    BAD_CHUNK,             // 16 a chunk the transfer rules do not allow
    TRANSFER_OVERRUN,      // 17 more bytes offered than the signed length
    TRANSFER_STALLED,      // 18 the host stopped sending partway through
    UPDATE_ABANDONED,      // 19 torn down before it finished
    DEVICE_FLASH_FAILED,   // 20 this device's flash would not take it
    SET_BOOT_FAILED,       // 21 verified, but the boot pointer would not move
    OUT_OF_SEQUENCE        // 22 the update steps arrived in the wrong order
};

// Stable short names, safe to send over the wire and to show to an owner.
const char* verdictName(Verdict v);

// A one-line explanation aimed at the person holding the device, not at a
// developer. Kept here so the device, the simulator and the companion app
// cannot describe the same refusal three different ways.
const char* verdictHelp(Verdict v);

// The parsed header. Fixed size, no allocation, safe as a stack local or as a
// member on a device with no heap headroom.
struct Header {
    uint32_t imageLen     = 0;
    uint32_t fwVersion    = 0;
    uint32_t buildNumber  = 0;
    uint16_t headerVersion = 0;
    uint16_t flags        = 0;
    char     boardId[OTA_BOARD_ID_BYTES + 1] = {0};  // always NUL-terminated
    uint8_t  keyId[OTA_KEYID_BYTES]          = {0};
    uint8_t  imageDigest[OTA_DIGEST_BYTES]   = {0};
    uint8_t  sig[OTA_SIG_BYTES]              = {0};

    // The signed prefix, kept verbatim. Re-serialising the parsed fields to
    // check a signature would make the serialiser part of the trust boundary
    // and would silently drop anything this build does not know about. Keep
    // and verify the bytes that actually arrived.
    uint8_t  signedPrefix[OTA_SIGNED_PREFIX_BYTES] = {0};
};

// ── The trusted key list, as a value ──────────────────────────────────────
//
// The set of keys allowed to authorise a firmware replacement. On the device
// this is always firmwareKeys(), which is the list compiled in from
// ota_pubkey.h, and every device call site takes the default.
//
// It is a parameter only so a native test can verify against a FIXTURE key.
// Without the seam a test has to shadow ota_pubkey.h on the include path or
// #define both arrays before including the .cpp, and a test that has to lie
// about the trust anchor to run is a test nobody can read. The content pack
// verifier already takes its key explicitly for the same reason.
//
// WHAT THIS DOES NOT CHANGE. Which keys the DEVICE trusts is still a property
// of the firmware image and nothing else. There is no path by which a host, an
// update file, or anything else on the wire reaches this parameter: the only
// caller in the firmware is OtaSession, it passes nothing, and widening the
// list would mean editing firmware source, which is the same bar as editing
// ota_pubkey.h itself. The seam moves what a TEST can substitute, not what a
// running device will accept.
struct KeyRing {
    const uint8_t (*ids)[OTA_KEYID_BYTES];    // count rows of 4
    const uint8_t (*keys)[OTA_PUBKEY_BYTES];  // count rows of 32, same order
    size_t        count;
};

// This firmware's own trusted list, from ota_pubkey.h. The default everywhere.
const KeyRing& firmwareKeys();

// Structural parse only. Fills `out` and checks magic, version, flags and
// reserved bytes. Does NOT check the board, the size or the signature, so a
// success here means "this is a well-formed HexHound update header", nothing
// more. Never treat a parse success as permission to write.
Verdict parse(const uint8_t* buf, size_t len, Header& out);

// Is this image for THIS hardware, and will it fit THIS slot?
//
// `slotBytes` must be the real size of the partition that would receive the
// image, read from the partition table on hardware. Passing a guess here is
// how a bounds check becomes decoration.
Verdict checkFit(const Header& h, uint32_t slotBytes);

// Ed25519 over OTA_DOMAIN_TAG || h.signedPrefix, against the trusted key named
// by h.keyId. This is the authenticity gate and it is the expensive call.
//
// It runs ContentCrypto::selfTest() first and returns CRYPTO_SELFTEST_FAILED
// if that does not pass. A verifier whose own primitives were never exercised
// on this silicon can only fail open, and failing open here means installing
// an unsigned image.
//
// `trusted` defaults to firmwareKeys(), so the device path is exactly what it
// was before the parameter existed. See KeyRing above.
Verdict checkSignature(const Header& h, const KeyRing& trusted = firmwareKeys());

// The whole gate, in the order the device must apply it: parse, then fit, then
// signature. Cheap structural checks first so obvious garbage never costs an
// Ed25519 verify; authenticity last but still strictly before any write.
//
// A caller that gets ACCEPTED from this function, and only such a caller, may
// begin writing to a boot partition.
Verdict accept(const uint8_t* buf, size_t len, uint32_t slotBytes, Header& out,
               const KeyRing& trusted = firmwareKeys());

// Does what was actually written match what the signature vouched for?
//
// `flashDigest` must be a SHA-512 computed by reading the bytes BACK OUT of
// the partition, not accumulated from the stream on the way in. Hashing the
// incoming stream attests to a copy that no longer exists; the thing that will
// boot is the thing on flash, and only that is worth checking. This is the
// easiest way to build an OTA verifier that passes every test and still bricks
// a device.
//
// Constant-time compare, via the shared primitive.
Verdict checkFlashDigest(const Header& h, const uint8_t flashDigest[OTA_DIGEST_BYTES]);

}  // namespace OtaImage
