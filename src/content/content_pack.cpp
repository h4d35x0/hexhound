#include "content_pack.h"
#include "content_pubkey.h"

#include <string.h>
#include <stdlib.h>

#if !defined(SIMULATOR_BUILD)
#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#elif defined(UNIT_TEST)
#include <cstdio>
#else
#include <cstdio>
#include "../hal/tft_compat.h"
#endif

// ── HexHound - Signed Content Pack Envelope Implementation ───────

namespace {

// Reads a whole file into a fresh allocation. ONE copy of this now, where
// there used to be a near-identical one in content_store.cpp and another in
// pet_inventory.cpp.
//
// Three builds, one contract: return the allocation and set len, or return
// nullptr and set status. The size cap is applied to the file's OWN reported
// size before a byte is read, so an oversized pack never gets to allocate.

#if !defined(SIMULATOR_BUILD)

uint8_t* readWholeFile(const char* path, size_t maxBytes, size_t& len,
                       ContentPackStatus& status) {
    len = 0;
    if (!SPIFFS.exists(path)) {
        status = PACK_ABSENT;
        return nullptr;
    }

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        status = PACK_READ_FAILED;
        return nullptr;
    }

    const size_t size = f.size();
    if (size == 0 || size < CONTENT_PACK_ENVELOPE_BYTES) {
        f.close();
        status = PACK_TOO_SMALL;
        return nullptr;
    }
    if (size > maxBytes) {
        f.close();
        status = PACK_TOO_LARGE;
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        f.close();
        status = PACK_NO_MEMORY;
        return nullptr;
    }

    const size_t got = f.read(buf, size);
    f.close();
    if (got != size) {
        free(buf);
        status = PACK_READ_FAILED;
        return nullptr;
    }

    len = size;
    status = PACK_OK;
    return buf;
}

#else

uint8_t* readWholeFile(const char* path, size_t maxBytes, size_t& len,
                       ContentPackStatus& status) {
    // The simulator and the native tests read packs from the working
    // directory, so content can be signed and reviewed on a desktop before it
    // reaches a device. "/content/x.hcp" is "content/x.hcp" beside the binary.
    len = 0;
    const char* rel = (path[0] == '/') ? path + 1 : path;

    FILE* f = fopen(rel, "rb");
    if (!f) {
        status = PACK_ABSENT;
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || (size_t)size < CONTENT_PACK_ENVELOPE_BYTES) {
        fclose(f);
        status = PACK_TOO_SMALL;
        return nullptr;
    }
    if ((size_t)size > maxBytes) {
        fclose(f);
        status = PACK_TOO_LARGE;
        return nullptr;
    }

    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        status = PACK_NO_MEMORY;
        return nullptr;
    }

    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        status = PACK_READ_FAILED;
        return nullptr;
    }

    len = (size_t)size;
    status = PACK_OK;
    return buf;
}

#endif

uint32_t readLE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

namespace ContentPack {

uint8_t trustedKeyCount() {
    return (uint8_t)CONTENT_TRUSTED_KEY_COUNT;
}

void keyIdFor(const uint8_t pubkey[ED25519_PUBKEY_BYTES], uint8_t out[4]) {
    uint8_t digest[CONTENT_SHA512_BYTES];
    ContentCrypto::sha512(pubkey, ED25519_PUBKEY_BYTES,
                          nullptr, 0, nullptr, 0, digest);
    memcpy(out, digest, 4);
}

ContentPackStatus verifyImage(const uint8_t* image, size_t len, uint8_t kind,
                              const uint8_t (*keys)[ED25519_PUBKEY_BYTES],
                              uint8_t keyCount,
                              const uint8_t** body, size_t* bodyLen) {
    if (body)    *body = nullptr;
    if (bodyLen) *bodyLen = 0;

    if (keys == nullptr) {
        keys     = CONTENT_TRUSTED_KEYS;
        keyCount = (uint8_t)CONTENT_TRUSTED_KEY_COUNT;
    }

    if (image == nullptr) return PACK_READ_FAILED;
    if (len < CONTENT_PACK_ENVELOPE_BYTES) return PACK_TOO_SMALL;

    // ── Fixed header fields ───────────────────────────────────────────────
    // Cheap, total-order checks first. None of them look at anything the
    // signature has not yet covered; they only decide whether it is worth
    // spending a hundred milliseconds on the curve.
    if (image[0] != CONTENT_PACK_MAGIC_0 || image[1] != CONTENT_PACK_MAGIC_1 ||
        image[2] != CONTENT_PACK_MAGIC_2 || image[3] != CONTENT_PACK_MAGIC_3) {
        return PACK_BAD_MAGIC;
    }
    if (image[4] != CONTENT_PACK_VERSION) return PACK_BAD_VERSION;
    if (image[5] != kind)                 return PACK_WRONG_KIND;
    if (image[6] != 0 || image[7] != 0)   return PACK_RESERVED_SET;

    // The declared body length must account for the file EXACTLY. Trailing
    // bytes are refused rather than ignored: they are covered by nothing, and
    // "ignored" is how a second payload rides along.
    const uint32_t declared = readLE32(image + 12);
    if ((uint64_t)declared + CONTENT_PACK_ENVELOPE_BYTES != (uint64_t)len) {
        return PACK_LENGTH_MISMATCH;
    }
    if (declared == 0) return PACK_TOO_SMALL;

    // ── Key selection ─────────────────────────────────────────────────────
    const uint8_t* wantId = image + 8;
    const uint8_t* key = nullptr;
    for (uint8_t i = 0; i < keyCount; i++) {
        uint8_t id[4];
        keyIdFor(keys[i], id);
        if (ContentCrypto::equalCT(id, wantId, 4)) {
            key = keys[i];
            break;
        }
    }
    if (key == nullptr) return PACK_UNKNOWN_KEY;

    // ── Signature, over header and body, before anything is parsed ────────
    const uint8_t* sig     = image + CONTENT_PACK_HEADER_BYTES;
    const uint8_t* payload = image + CONTENT_PACK_ENVELOPE_BYTES;

    // The signed message is header || body, which is not contiguous in the
    // file because the signature sits between them. ContentCrypto::sha512
    // takes the parts separately for exactly this reason, so no second buffer
    // the size of the pack is needed to check it.
    if (!ContentCrypto::verify2(image, CONTENT_PACK_HEADER_BYTES,
                                payload, declared, sig, key)) {
        return PACK_BAD_SIGNATURE;
    }

    if (body)    *body = payload;
    if (bodyLen) *bodyLen = declared;
    return PACK_OK;
}

ContentPackStatus load(const char* path, uint8_t kind, size_t maxBytes,
                       ContentPackFile& out) {
    out.raw = nullptr;
    out.rawLen = 0;
    out.body = nullptr;
    out.bodyLen = 0;

    if (path == nullptr) return PACK_READ_FAILED;
    if (maxBytes < CONTENT_PACK_ENVELOPE_BYTES) return PACK_TOO_LARGE;

    size_t len = 0;
    ContentPackStatus status = PACK_READ_FAILED;
    uint8_t* raw = readWholeFile(path, maxBytes, len, status);
    if (raw == nullptr) return status;

    const uint8_t* body = nullptr;
    size_t bodyLen = 0;
    status = verifyImage(raw, len, kind, nullptr, 0, &body, &bodyLen);
    if (status != PACK_OK) {
        free(raw);
        return status;
    }

    out.raw     = raw;
    out.rawLen  = len;
    out.body    = body;
    out.bodyLen = bodyLen;
    return PACK_OK;
}

void release(ContentPackFile& out) {
    if (out.raw) free(out.raw);
    out.raw = nullptr;
    out.rawLen = 0;
    out.body = nullptr;
    out.bodyLen = 0;
}

const char* statusName(ContentPackStatus status) {
    switch (status) {
        case PACK_OK:              return "ok";
        case PACK_ABSENT:          return "absent";
        case PACK_READ_FAILED:     return "read failed";
        case PACK_TOO_SMALL:       return "too small";
        case PACK_TOO_LARGE:       return "too large";
        case PACK_NO_MEMORY:       return "out of memory";
        case PACK_BAD_MAGIC:       return "not a content pack";
        case PACK_BAD_VERSION:     return "unsupported format version";
        case PACK_RESERVED_SET:    return "reserved field set";
        case PACK_WRONG_KIND:      return "wrong pack kind";
        case PACK_LENGTH_MISMATCH: return "length mismatch";
        case PACK_UNKNOWN_KEY:     return "signed by an unknown key";
        case PACK_BAD_SIGNATURE:   return "bad signature";
        default:                   return "rejected";
    }
}

}  // namespace ContentPack
