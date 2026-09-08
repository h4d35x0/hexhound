#pragma once

#include <stdint.h>
#include <stddef.h>

// ── HexHound - Strict Bounded CBOR Reader ────────────────────────
//
// A deliberately incomplete CBOR decoder. It reads the handful of shapes
// HexHound content actually uses and REFUSES everything else, rather than
// being a general RFC 8949 implementation with the unused parts left enabled.
//
// Why not a library: a general decoder has to accept indefinite-length items,
// tags, floats, byte strings, nested containers of arbitrary depth and 64-bit
// lengths, because some caller somewhere needs them. Every one of those is
// code that runs on a buffer an attacker controls, in service of a feature no
// content pack uses. The smallest decoder that can read our content is also
// the smallest decoder that can be read in full before trusting it.
//
// What is accepted:
//   * major 0, unsigned integers, up to 32 bits
//   * major 3, text strings, up to CBOR_MAX_STR_BYTES on the wire
//   * major 4, definite-length arrays
//   * major 5, definite-length maps
//   * major 7, only the simple values false (20), true (21) and null (22)
//
// What is refused, always:
//   * negative integers, byte strings, tags, floats, break codes
//   * every indefinite-length item
//   * 8-byte lengths (additional info 27) and the reserved 28-30
//   * NON-MINIMAL integer encodings. 0x18 0x05 means 5 and so does 0x05; two
//     spellings of one value is a difference that has to be reconciled
//     somewhere, and the place it is cheapest to refuse is here.
//   * any container whose declared element count cannot physically fit in the
//     bytes that remain, which catches a truncated pack at the header rather
//     than after a partial walk
//   * nesting past CBOR_MAX_DEPTH
//   * trailing bytes after the top-level value (see finish())
//
// The reader never allocates, never takes ownership of the buffer, and never
// reads outside [buf, buf + len). Once a call fails the reader latches into a
// failed state and every later call fails too, so a caller that forgets one
// return value still cannot act on garbage.

// Deepest nesting any HexHound pack needs is map -> array -> map -> value,
// which is 3. Four leaves one level of headroom without letting a hostile
// pack recurse.
#define CBOR_MAX_DEPTH       4

// Largest element count a single array or map may declare. Far above the
// biggest table this firmware has (40 dialogue lines), because truncating to
// the table cap is the CONTENT layer's job and it must behave the same way it
// does for JSON. This bound exists to stop absurd counts, not to size tables.
#define CBOR_MAX_ITEMS     256

// Longest text string, in bytes on the wire. CONTENT_MAX_TEXT_LEN is 96, so
// this leaves room for a line that gets truncated on copy exactly as the JSON
// path truncates it, without letting a pack declare a kilobyte of text.
#define CBOR_MAX_STR_BYTES 128

enum CborType : uint8_t {
    CBOR_TYPE_UINT = 0,
    CBOR_TYPE_TEXT,
    CBOR_TYPE_ARRAY,
    CBOR_TYPE_MAP,
    CBOR_TYPE_BOOL,
    CBOR_TYPE_NULL,
    CBOR_TYPE_INVALID
};

struct CborReader {
    const uint8_t* buf   = nullptr;
    size_t         len   = 0;
    size_t         pos   = 0;
    bool           failed = true;   // an uninitialised reader is a failed one
};

namespace Cbor {

void init(CborReader& r, const uint8_t* buf, size_t len);

inline bool failed(const CborReader& r) { return r.failed; }
inline bool atEnd(const CborReader& r)  { return r.pos >= r.len; }

// Type of the next value without consuming it. Returns CBOR_TYPE_INVALID for
// anything outside the accepted profile, and does NOT latch failure: a caller
// is allowed to look and then decide.
CborType peekType(const CborReader& r);

// Each of these consumes exactly one value and returns false on any refusal.
bool readUint(CborReader& r, uint32_t& value);
bool readBool(CborReader& r, bool& value);
bool readNull(CborReader& r);
bool readArray(CborReader& r, uint32_t& count);
bool readMap(CborReader& r, uint32_t& count);

// Consumes a text string. Copies at most outCap-1 bytes and always
// NUL-terminates. A string longer than outCap-1 is TRUNCATED, not refused,
// which is what strlcpy does on the JSON path; the wire-length bound is
// CBOR_MAX_STR_BYTES and that one does refuse.
bool readText(CborReader& r, char* out, size_t outCap);

// Consumes and discards one value of any accepted type, including a whole
// container. Bounded by CBOR_MAX_DEPTH. This is what lets a decoder ignore a
// field it does not know without having to understand it.
bool skipValue(CborReader& r);

// True only when nothing failed AND every byte was consumed. Trailing bytes
// after the top-level value are a refusal: they are signed, so they are not
// an attack by themselves, but they mean the pack is not what the tool that
// signed it thought it was.
bool finish(const CborReader& r);

}  // namespace Cbor
