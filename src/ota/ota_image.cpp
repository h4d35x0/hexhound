#include "ota_image.h"
#include "ota_pubkey.h"

#include <string.h>

#include "../content/content_crypto.h"

// ── HexHound - Signed Firmware Envelope Implementation ──────────
//
// No crypto is implemented here. The Ed25519 curve arithmetic and the SHA-512
// both come from ContentCrypto, which is the single copy of each in the whole
// firmware. mbedtls does not ship EdDSA at any version ESP-IDF carries, so the
// curve code is one portable implementation compiled on every target; a second
// copy would be both a flash cost and, worse, the copy nobody audits.

// The KeyRing rows are sized by OTA_PUBKEY_BYTES and are handed straight to
// ContentCrypto::verify(), which reads ED25519_PUBKEY_BYTES. If those two ever
// disagreed the verifier would read past a key row, so they are checked here
// rather than left to agree by coincidence.
static_assert(OTA_PUBKEY_BYTES == ED25519_PUBKEY_BYTES,
              "OTA key row width must match the Ed25519 public key size");
static_assert(OTA_SIG_BYTES == ED25519_SIG_BYTES,
              "OTA signature field must match the Ed25519 signature size");
static_assert(OTA_DIGEST_BYTES == CONTENT_SHA512_BYTES,
              "OTA image digest field must match the SHA-512 output size");

namespace {

// Little-endian readers. Explicit byte assembly rather than a memcpy into a
// uint32_t, because the wire format is defined as little-endian and this code
// must give the same answer on a host that is not.
inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Field offsets, named once so the parser and the signing script cannot drift
// apart silently. scripts/sign_ota_image.py carries the same table.
enum : size_t {
    OFF_MAGIC        = 0,
    OFF_HDR_VERSION  = 8,
    OFF_FLAGS        = 10,
    OFF_IMAGE_LEN    = 12,
    OFF_BOARD_ID     = 16,
    OFF_FW_VERSION   = 48,
    OFF_BUILD_NUMBER = 52,
    OFF_KEY_ID       = 56,
    OFF_DIGEST       = 60,
    OFF_RESERVED     = 124,
    OFF_SIG          = 128
};

}  // namespace

namespace OtaImage {

const char* verdictName(Verdict v) {
    switch (v) {
        case ACCEPTED:               return "accepted";
        case SHORT_HEADER:           return "short_header";
        case BAD_MAGIC:              return "bad_magic";
        case BAD_HEADER_VERSION:     return "bad_header_version";
        case BAD_FLAGS:              return "bad_flags";
        case BAD_RESERVED:           return "bad_reserved";
        case BOARD_MISMATCH:         return "board_mismatch";
        case IMAGE_TOO_SMALL:        return "image_too_small";
        case IMAGE_TOO_LARGE:        return "image_too_large";
        case UNKNOWN_KEY:            return "unknown_key";
        case BAD_SIGNATURE:          return "bad_signature";
        case DIGEST_MISMATCH:        return "digest_mismatch";
        case CRYPTO_SELFTEST_FAILED: return "crypto_selftest_failed";
        case NOT_ARMED:              return "not_armed";
        case UPDATE_ON_TRIAL:        return "update_on_trial";
        case NO_UPDATE_SLOT:         return "no_update_slot";
        case BAD_CHUNK:              return "bad_chunk";
        case TRANSFER_OVERRUN:       return "transfer_overrun";
        case TRANSFER_STALLED:       return "transfer_stalled";
        case UPDATE_ABANDONED:       return "update_abandoned";
        case DEVICE_FLASH_FAILED:    return "device_flash_failed";
        case SET_BOOT_FAILED:        return "set_boot_failed";
        case OUT_OF_SEQUENCE:        return "out_of_sequence";
    }
    return "unknown";
}

const char* verdictHelp(Verdict v) {
    switch (v) {
        case ACCEPTED:
            return "Update accepted.";
        case SHORT_HEADER:
            return "The update file is incomplete.";
        case BAD_MAGIC:
            return "This is not a HexHound update file.";
        case BAD_HEADER_VERSION:
            return "This update needs newer firmware to install.";
        case BAD_FLAGS:
            return "This update uses options this firmware does not know.";
        case BAD_RESERVED:
            return "The update file is malformed.";
        case BOARD_MISMATCH:
            return "This update is for a different HexHound board.";
        case IMAGE_TOO_SMALL:
            return "The update file is too small to be firmware.";
        case IMAGE_TOO_LARGE:
            return "This update is too big to fit on this device.";
        case UNKNOWN_KEY:
            return "This update is signed by a key this device does not trust.";
        case BAD_SIGNATURE:
            return "This update is not correctly signed. It was refused.";
        case DIGEST_MISMATCH:
            return "The update did not arrive intact. Nothing was changed.";
        case CRYPTO_SELFTEST_FAILED:
            return "This device could not verify its own security check.";

        // Session causes. "Nothing was changed" appears on every one of these
        // that can honestly claim it, because the single thing the owner wants
        // to know after a failed update is whether their device and their pet
        // are still all right.
        case NOT_ARMED:
            return "This device was not expecting an update. Start one on the "
                   "device first.";
        case UPDATE_ON_TRIAL:
            return "This device is still checking its last update. Wait a few "
                   "seconds and try again.";
        case NO_UPDATE_SLOT:
            return "This device has nowhere to install an update. Nothing was "
                   "changed.";
        case BAD_CHUNK:
            return "The update was sent in a way this device cannot accept. "
                   "Nothing was changed.";
        case TRANSFER_OVERRUN:
            return "More update data arrived than the update said it would "
                   "send. Nothing was changed.";
        case TRANSFER_STALLED:
            return "The update stopped partway through. Nothing was changed.";
        case UPDATE_ABANDONED:
            return "The update was stopped before it finished. Nothing was "
                   "changed.";
        case DEVICE_FLASH_FAILED:
            return "This device's storage could not take the update. Nothing "
                   "was changed.";
        case SET_BOOT_FAILED:
            return "The update was checked and was good, but this device could "
                   "not switch to it. Nothing was changed.";
        case OUT_OF_SEQUENCE:
            return "The update steps arrived in the wrong order. Nothing was "
                   "changed.";
    }
    return "The update was refused.";
}

Verdict parse(const uint8_t* buf, size_t len, Header& out) {
    if (buf == nullptr || len < OTA_HEADER_BYTES) {
        return SHORT_HEADER;
    }

    if (memcmp(buf + OFF_MAGIC, OTA_MAGIC, OTA_MAGIC_BYTES) != 0) {
        return BAD_MAGIC;
    }

    out.headerVersion = rd16(buf + OFF_HDR_VERSION);
    if (out.headerVersion != OTA_HEADER_VERSION) {
        // Deliberately refuse a NEWER header rather than parsing the fields
        // this build happens to recognise. A future format may move a bound
        // check somewhere this code does not look, and a partial understanding
        // of a security-relevant record is worse than no understanding.
        return BAD_HEADER_VERSION;
    }

    out.flags = rd16(buf + OFF_FLAGS);
    if (out.flags != 0) {
        // Same reasoning. An unknown flag may change what a field means.
        return BAD_FLAGS;
    }

    for (size_t i = 0; i < 4; i++) {
        if (buf[OFF_RESERVED + i] != 0) {
            return BAD_RESERVED;
        }
    }

    out.imageLen    = rd32(buf + OFF_IMAGE_LEN);
    out.fwVersion   = rd32(buf + OFF_FW_VERSION);
    out.buildNumber = rd32(buf + OFF_BUILD_NUMBER);

    // Copied into a buffer one byte longer than the field and zero-filled
    // first, so an unterminated 32-byte board id cannot run off the end when
    // it is later compared or printed.
    memset(out.boardId, 0, sizeof(out.boardId));
    memcpy(out.boardId, buf + OFF_BOARD_ID, OTA_BOARD_ID_BYTES);

    memcpy(out.keyId,        buf + OFF_KEY_ID, OTA_KEYID_BYTES);
    memcpy(out.imageDigest,  buf + OFF_DIGEST, OTA_DIGEST_BYTES);
    memcpy(out.sig,          buf + OFF_SIG,    OTA_SIG_BYTES);
    memcpy(out.signedPrefix, buf,              OTA_SIGNED_PREFIX_BYTES);

    return ACCEPTED;
}

Verdict checkFit(const Header& h, uint32_t slotBytes) {
    if (strcmp(h.boardId, HEXHOUND_OTA_BOARD_ID) != 0) {
        return BOARD_MISMATCH;
    }

    if (h.imageLen < OTA_MIN_IMAGE_BYTES) {
        return IMAGE_TOO_SMALL;
    }

    // The refusal that keeps a partial write from ever happening. The size is
    // known and checked while the flash is still untouched, so an oversized
    // image is declined outright rather than streamed until the partition runs
    // out. A device that ran out of room halfway is a device with a corrupt
    // slot and a story about why that was acceptable.
    if (slotBytes == 0 || h.imageLen > slotBytes) {
        return IMAGE_TOO_LARGE;
    }

    return ACCEPTED;
}

const KeyRing& firmwareKeys() {
    // Built from the generated header, once, and handed out by const reference.
    // Nothing can reseat it and nothing can add a row: this is the firmware's
    // statement about who may replace the firmware, and it is as fixed as the
    // code it is compiled into.
    static const KeyRing ring = {
        OTA_TRUSTED_KEY_IDS,
        OTA_TRUSTED_KEYS,
        (size_t)OTA_TRUSTED_KEY_COUNT
    };
    return ring;
}

Verdict checkSignature(const Header& h, const KeyRing& trusted) {
    // The verifier verifies itself first. ContentCrypto::selfTest() runs an
    // RFC 8032 vector plus two negative cases against whichever backend was
    // actually compiled in, so this is a statement about this silicon and this
    // build, not an inherited claim from a desktop test run.
    //
    // Failing closed here is the whole point: if the check cannot be trusted,
    // no image is installed. The device keeps running what it has.
    if (!ContentCrypto::selfTest()) {
        return CRYPTO_SELFTEST_FAILED;
    }

    // The signed message, materialised once. 144 bytes on the stack, next to
    // the roughly 3 KB the verify itself needs.
    uint8_t msg[OTA_DOMAIN_TAG_BYTES + OTA_SIGNED_PREFIX_BYTES];
    memset(msg, 0, OTA_DOMAIN_TAG_BYTES);
    memcpy(msg, OTA_DOMAIN_TAG, strlen(OTA_DOMAIN_TAG));
    memcpy(msg + OTA_DOMAIN_TAG_BYTES, h.signedPrefix, OTA_SIGNED_PREFIX_BYTES);

    // An empty or malformed ring trusts nobody. Returning UNKNOWN_KEY rather
    // than dereferencing is the same fail-closed rule as everything else here:
    // a verifier that cannot name a key has not verified anything.
    if (trusted.ids == nullptr || trusted.keys == nullptr || trusted.count == 0) {
        return UNKNOWN_KEY;
    }

    bool sawKey = false;

    for (size_t i = 0; i < trusted.count; i++) {
        // keyId is a SELECTOR, not a credential. It says which trusted key to
        // try, so a rotated key produces "unknown key" instead of a baffling
        // "bad signature". Forging it buys an attacker nothing: the signature
        // still has to verify under the key it names, and that key is compiled
        // into this firmware. Nobody should ever treat this value as identity.
        if (memcmp(h.keyId, trusted.ids[i], OTA_KEYID_BYTES) != 0) {
            continue;
        }
        sawKey = true;

        if (ContentCrypto::verify(msg, sizeof(msg), h.sig, trusted.keys[i])) {
            return ACCEPTED;
        }
    }

    return sawKey ? BAD_SIGNATURE : UNKNOWN_KEY;
}

Verdict accept(const uint8_t* buf, size_t len, uint32_t slotBytes, Header& out,
               const KeyRing& trusted) {
    Verdict v = parse(buf, len, out);
    if (v != ACCEPTED) {
        return v;
    }

    v = checkFit(out, slotBytes);
    if (v != ACCEPTED) {
        return v;
    }

    // Last, because it is the expensive one, and obvious garbage should not
    // cost an Ed25519 verify. Still strictly before any caller is permitted to
    // touch flash, which is the property that actually matters.
    return checkSignature(out, trusted);
}

Verdict checkFlashDigest(const Header& h, const uint8_t flashDigest[OTA_DIGEST_BYTES]) {
    if (!ContentCrypto::equalCT(h.imageDigest, flashDigest, OTA_DIGEST_BYTES)) {
        return DIGEST_MISMATCH;
    }
    return ACCEPTED;
}

}  // namespace OtaImage
