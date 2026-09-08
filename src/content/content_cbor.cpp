#include "content_cbor.h"

#include <string.h>

// ── HexHound - Strict Bounded CBOR Reader Implementation ─────────

namespace {

const uint8_t MAJOR_UINT  = 0;
const uint8_t MAJOR_TEXT  = 3;
const uint8_t MAJOR_ARRAY = 4;
const uint8_t MAJOR_MAP   = 5;
const uint8_t MAJOR_SIMPLE = 7;

const uint8_t SIMPLE_FALSE = 20;
const uint8_t SIMPLE_TRUE  = 21;
const uint8_t SIMPLE_NULL  = 22;

// Decodes one item head at `pos` WITHOUT touching the reader, so peekType can
// use it too. On success advances `pos` past the head.
//
// Returns false for every encoding outside the accepted profile, which is
// where most of this file's value is: an attacker's first move against a
// decoder is an item shape the author did not think about.
bool decodeHead(const uint8_t* buf, size_t len, size_t& pos,
                uint8_t& major, uint32_t& value) {
    if (pos >= len) return false;

    const uint8_t initial = buf[pos];
    major = (uint8_t)(initial >> 5);
    const uint8_t info = (uint8_t)(initial & 0x1f);
    size_t p = pos + 1;

    if (info < 24) {
        value = info;
    } else if (info == 24) {
        if (p + 1 > len) return false;
        value = buf[p];
        p += 1;
        // Minimal-encoding rule: anything below 24 has a shorter spelling.
        if (value < 24) return false;
    } else if (info == 25) {
        if (p + 2 > len) return false;
        value = ((uint32_t)buf[p] << 8) | (uint32_t)buf[p + 1];
        p += 2;
        if (value <= 0xFF) return false;
    } else if (info == 26) {
        if (p + 4 > len) return false;
        value = ((uint32_t)buf[p] << 24) | ((uint32_t)buf[p + 1] << 16) |
                ((uint32_t)buf[p + 2] << 8) | (uint32_t)buf[p + 3];
        p += 4;
        if (value <= 0xFFFF) return false;
    } else {
        // 27 is a 64-bit length, 28-30 are reserved, 31 is indefinite length
        // or the break code. None of them can appear in a HexHound pack.
        return false;
    }

    // Simple values are the only major-7 items allowed, and only three of
    // them. Floats live at info 25/26/27 in major 7, so this check has to come
    // after the head is decoded rather than instead of it.
    if (major == MAJOR_SIMPLE) {
        if (info >= 24) return false;   // no float, no 1-byte simple value
        if (value != SIMPLE_FALSE && value != SIMPLE_TRUE && value != SIMPLE_NULL) {
            return false;
        }
    }

    // Majors 1 (negative int), 2 (byte string) and 6 (tag) are refused
    // outright. Content has no use for any of them, and a tag in particular is
    // an instruction to reinterpret the item that follows.
    if (major == 1 || major == 2 || major == 6) return false;

    pos = p;
    return true;
}

// One value, discarded, with an explicit depth budget. The only recursion in
// this file, and the reason CBOR_MAX_DEPTH exists.
//
// `depth` is the nesting level of the value being read, counting CONTAINERS
// only: the top-level value is level 1, and a scalar inside four nested arrays
// is not a fifth level. So CBOR_MAX_DEPTH == 4 permits exactly four nested
// containers, which is one more than any HexHound pack needs.
bool skipAt(CborReader& r, uint8_t depth) {
    uint8_t  major;
    uint32_t value;
    if (!decodeHead(r.buf, r.len, r.pos, major, value)) return false;

    switch (major) {
        case MAJOR_UINT:
        case MAJOR_SIMPLE:
            return true;

        case MAJOR_TEXT:
            if (value > CBOR_MAX_STR_BYTES) return false;
            if (value > r.len - r.pos) return false;
            r.pos += value;
            return true;

        case MAJOR_ARRAY:
        case MAJOR_MAP: {
            if (depth > CBOR_MAX_DEPTH) return false;
            if (value > CBOR_MAX_ITEMS) return false;
            // Every remaining item costs at least one byte, and a map pair
            // costs at least two. A count that cannot fit is a truncated or
            // hostile pack, and saying so here beats discovering it halfway
            // through the walk.
            const uint64_t minBytes =
                (major == MAJOR_MAP) ? (uint64_t)value * 2u : (uint64_t)value;
            if (minBytes > (uint64_t)(r.len - r.pos)) return false;

            const uint32_t items = (major == MAJOR_MAP) ? value * 2u : value;
            for (uint32_t i = 0; i < items; i++) {
                if (!skipAt(r, (uint8_t)(depth + 1))) return false;
            }
            return true;
        }

        default:
            return false;
    }
}

// Consumes a head and insists on a particular major type.
bool takeHead(CborReader& r, uint8_t wantMajor, uint32_t& value) {
    if (r.failed) return false;
    uint8_t major;
    size_t  pos = r.pos;
    if (!decodeHead(r.buf, r.len, pos, major, value) || major != wantMajor) {
        r.failed = true;
        return false;
    }
    r.pos = pos;
    return true;
}

}  // namespace

namespace Cbor {

void init(CborReader& r, const uint8_t* buf, size_t len) {
    r.buf    = buf;
    r.len    = len;
    r.pos    = 0;
    r.failed = (buf == nullptr) || (len == 0);
}

CborType peekType(const CborReader& r) {
    if (r.failed) return CBOR_TYPE_INVALID;

    uint8_t  major;
    uint32_t value;
    size_t   pos = r.pos;
    if (!decodeHead(r.buf, r.len, pos, major, value)) return CBOR_TYPE_INVALID;

    switch (major) {
        case MAJOR_UINT:  return CBOR_TYPE_UINT;
        case MAJOR_TEXT:  return CBOR_TYPE_TEXT;
        case MAJOR_ARRAY: return CBOR_TYPE_ARRAY;
        case MAJOR_MAP:   return CBOR_TYPE_MAP;
        case MAJOR_SIMPLE:
            return (value == SIMPLE_NULL) ? CBOR_TYPE_NULL : CBOR_TYPE_BOOL;
        default:          return CBOR_TYPE_INVALID;
    }
}

bool readUint(CborReader& r, uint32_t& value) {
    return takeHead(r, MAJOR_UINT, value);
}

bool readBool(CborReader& r, bool& value) {
    uint32_t v;
    if (!takeHead(r, MAJOR_SIMPLE, v)) return false;
    if (v != SIMPLE_FALSE && v != SIMPLE_TRUE) {
        r.failed = true;
        return false;
    }
    value = (v == SIMPLE_TRUE);
    return true;
}

bool readNull(CborReader& r) {
    uint32_t v;
    if (!takeHead(r, MAJOR_SIMPLE, v)) return false;
    if (v != SIMPLE_NULL) {
        r.failed = true;
        return false;
    }
    return true;
}

bool readArray(CborReader& r, uint32_t& count) {
    if (!takeHead(r, MAJOR_ARRAY, count)) return false;
    if (count > CBOR_MAX_ITEMS || (uint64_t)count > (uint64_t)(r.len - r.pos)) {
        r.failed = true;
        return false;
    }
    return true;
}

bool readMap(CborReader& r, uint32_t& count) {
    if (!takeHead(r, MAJOR_MAP, count)) return false;
    if (count > CBOR_MAX_ITEMS ||
        (uint64_t)count * 2u > (uint64_t)(r.len - r.pos)) {
        r.failed = true;
        return false;
    }
    return true;
}

bool readText(CborReader& r, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) {
        r.failed = true;
        return false;
    }
    out[0] = '\0';

    uint32_t byteLen;
    if (!takeHead(r, MAJOR_TEXT, byteLen)) return false;

    if (byteLen > CBOR_MAX_STR_BYTES || byteLen > (r.len - r.pos)) {
        r.failed = true;
        return false;
    }

    const size_t copy = (byteLen >= outCap) ? (outCap - 1) : byteLen;
    memcpy(out, r.buf + r.pos, copy);
    out[copy] = '\0';
    r.pos += byteLen;

    // An embedded NUL would make the copied C string shorter than the string
    // the signer measured, which is a way to smuggle bytes past a later
    // comparison. Content has no reason to contain one.
    if (strlen(out) != copy) {
        r.failed = true;
        return false;
    }
    return true;
}

bool skipValue(CborReader& r) {
    if (r.failed) return false;
    if (!skipAt(r, 1)) {
        r.failed = true;
        return false;
    }
    return true;
}

bool finish(const CborReader& r) {
    return !r.failed && r.pos == r.len;
}

}  // namespace Cbor
