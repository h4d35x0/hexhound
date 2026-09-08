#pragma once

#include <stdint.h>
#include <stddef.h>

#include "ota_session.h"

// ── HexHound - OTA over USB Serial ──────────────────────────────
//
// The wire that actually carries a firmware image to the device, and the thin
// service around OtaSession that main.cpp and the update screen both talk to.
//
// ── Why framed binary, and why no base64 ──────────────────────────────────
//
// Serial is full duplex. The image travels host to device; the firmware's
// ordinary logging travels device to host. They do not share a buffer and they
// do not collide, so the host-to-device direction has no reason to avoid any
// byte value. Escaping or base64 would cost 33% of a 1.2 MB transfer and buy
// nothing, because there is no byte that needs protecting from anything.
//
// The one real problem is the other direction: the host has to find response
// frames amongst ordinary [BOOT]/[Loop] log lines. That is solved by giving
// the device-to-host frames a magic whose first byte is not ASCII, so no log
// line the firmware can print will ever contain it.
//
// ── Frame layout, both directions ─────────────────────────────────────────
//
//     off  len  field
//     0    4    magic
//     4    1    type
//     5    1    flags, reserved, must be 0
//     6    2    payload length, little-endian, <= HEXHOUND_OTA_CHUNK_MAX
//     8    N    payload
//     8+N  4    CRC32 little-endian over bytes [4, 8+N)
//
// Twelve bytes of overhead on a 4096-byte chunk, which is 0.3%.
//
// ── Why two different magics ──────────────────────────────────────────────
//
// A5 48 58 44 host to device, 5A 48 58 48 device to host. If both directions
// shared one magic then a loopback adapter, an echoing terminal, or a host
// that accidentally opened its own output would look exactly like a peer, and
// the failure would present as a device that mysteriously answers itself. Two
// magics make that case fail immediately and obviously instead. The lead bytes
// are 0xA5 and 0x5A: non-ASCII, so neither can occur in the firmware's log
// text, and complements of each other, so a stuck or inverted line does not
// turn one into the other.
//
// ── Why a CRC when the image is already signed ────────────────────────────
//
// The CRC is not security. The Ed25519 signature is, and nothing here weakens
// or substitutes for it. The CRC exists so that a DESYNCHRONISED STREAM FAILS
// LOUDLY AND IMMEDIATELY. Without it, a dropped byte turns into a frame that
// parses, a chunk that writes, and a digest mismatch several seconds and a
// megabyte later, at which point the reported cause is "the image did not
// arrive intact" and the actual cause was a framing bug three thousand chunks
// earlier. Naming the failure where it happens is worth four bytes a frame.
//
// The CRC covers from the type byte, not from the magic. The magic is matched
// exactly by the reader before anything else is read, so it is already proven;
// what the CRC has to protect is the fields the reader is about to trust.
//
// ── There is no ARM verb, and that is the point ───────────────────────────
//
// Every host-to-device frame type is listed below and none of them arms the
// session. arm() is reachable only from the update screen, in response to a
// person holding the button on the device. A host can ask what state the
// device is in and it can send an image once a person has already said yes; it
// has no way to say yes on their behalf. "No automatic updates and no silent
// ones" is implemented by that absence, so anything added to this enum has to
// be checked against it.

// Bumped only when the frame layout or the meaning of a type changes. The host
// refuses to talk to a device that does not answer with the version it knows,
// because a half-understood update protocol is worse than none.
#define OTA_WIRE_VERSION       1

#define OTA_FRAME_MAGIC_BYTES  4
#define OTA_FRAME_HEAD_BYTES   8    // magic + type + flags + length
#define OTA_FRAME_CRC_BYTES    4
#define OTA_FRAME_OVERHEAD     (OTA_FRAME_HEAD_BYTES + OTA_FRAME_CRC_BYTES)

// One chunk is the largest thing that ever crosses this wire, in either
// direction, which is what keeps the receive buffer to a single 4 KB static
// array on a board with no PSRAM.
#define OTA_FRAME_MAX_PAYLOAD  HEXHOUND_OTA_CHUNK_MAX

extern const uint8_t OTA_MAGIC_H2D[OTA_FRAME_MAGIC_BYTES];
extern const uint8_t OTA_MAGIC_D2H[OTA_FRAME_MAGIC_BYTES];

// Frame types. Host-to-device values are 0x00..0x7F, device-to-host values
// have the high bit set, so a frame that somehow arrived on the wrong wire is
// refused by its type as well as by its magic.
enum OtaFrameType : uint8_t {
    // ── Host to device ────────────────────────────────────────────────────
    OTA_F_HELLO  = 0x01,  // empty payload. "What are you, and what state are
                          // you in?" Legal in every state and changes none of
                          // them. This is how the host learns the board id
                          // before it sends an image built for another board.
    OTA_F_BEGIN  = 0x02,  // payload: the 192-byte signed header. offerHeader().
    OTA_F_DATA   = 0x03,  // payload: up to 4096 image bytes, in order.
    OTA_F_COMMIT = 0x04,  // empty payload. finish().
    OTA_F_ABORT  = 0x05,  // empty payload. cancel().

    // ── Device to host ────────────────────────────────────────────────────
    OTA_F_IDENT    = 0x81,  // identity plus current state. Answer to HELLO, and
                            // sent unsolicited whenever the session state
                            // changes, so a host waiting for the owner to press
                            // confirm does not have to poll.
    OTA_F_PROGRESS = 0x82,  // 10 bytes, sent per accepted chunk. Deliberately
                            // fixed-size and string-free: this one is on the
                            // hot path and is sent three hundred times.
    OTA_F_RESULT   = 0x83   // the end of an attempt, good or bad, with the
                            // verdict and the owner-facing help text.
};

// ── IDENT payload, 48 bytes ───────────────────────────────────────────────
//
//     0   1   wire version
//     1   1   session state (OtaSession::State)
//     2   1   verdict (OtaImage::Verdict) of the last real decision
//     3   1   flags: bit0 running image on trial, bit1 booted after rollback,
//               bit2 a transport is actually attached to a flash target
//     4   4   this firmware's version, packed major<<16|minor<<8|patch
//     8   4   OTA slot size in bytes, as read from the real partition table
//     12  2   maximum chunk this device will accept
//     14  2   reserved, zero
//     16  32  board id, NUL-padded ASCII
#define OTA_IDENT_BYTES        48
#define OTA_IDENT_FLAG_ON_TRIAL       0x01
#define OTA_IDENT_FLAG_AFTER_ROLLBACK 0x02
#define OTA_IDENT_FLAG_HAS_TARGET     0x04

// ── PROGRESS payload, 10 bytes ────────────────────────────────────────────
//
//     0   1   session state
//     1   1   verdict
//     2   4   bytes written so far
//     6   4   total image length from the SIGNED header
#define OTA_PROGRESS_BYTES     10

// ── RESULT payload, variable ──────────────────────────────────────────────
//
//     0   1   session state
//     1   1   verdict
//     2   4   bytes written
//     6   4   image length
//     10  ..  three NUL-terminated strings, in this order:
//               verdictName(), verdictHelp(), failReason()
//
// The help string is carried rather than looked up host-side on purpose. It is
// written for the person holding the device and it lives in ota_image.cpp; a
// host that kept its own copy would drift, and the two would eventually
// describe the same refusal differently. The device is the authority on why it
// said no.
#define OTA_RESULT_FIXED_BYTES 10

// ── CRC32 ─────────────────────────────────────────────────────────────────
//
// Standard reflected CRC-32 (poly 0xEDB88320, init and final xor 0xFFFFFFFF),
// which is the one zlib and Python's binascii.crc32 compute, so the host side
// is a library call and not a second implementation to keep in step.
uint32_t otaCrc32(uint32_t crc, const uint8_t* data, size_t len);
static inline uint32_t otaCrc32Init() { return 0xFFFFFFFFu; }
static inline uint32_t otaCrc32Final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// ── The reader ────────────────────────────────────────────────────────────
//
// A strict byte-at-a-time state machine. It never blocks, never allocates, and
// owns exactly one payload buffer that is reused for every frame, so the
// "never hold more than one chunk" rule is a property of the type rather than
// something the caller has to remember.
//
// Portable, with no Arduino dependency, so its resynchronisation and its CRC
// can be driven from a desktop test instead of by unplugging a cable at
// carefully chosen moments.
class OtaFrameReader {
public:
    enum Event : uint8_t {
        NONE = 0,      // still mid-frame, feed it more
        FRAME,         // a complete, CRC-checked frame is available
        BAD_CRC,       // a frame arrived corrupt; the stream is suspect
        BAD_LENGTH,    // a length no legal frame can carry
        BAD_FLAGS      // a reserved bit this build does not understand
    };

    // Feed one received byte. Returns what that byte completed, if anything.
    Event feed(uint8_t b);

    // Valid only immediately after feed() returned FRAME.
    uint8_t        type()    const { return _type; }
    uint16_t       length()  const { return _len; }
    const uint8_t* payload() const { return _buf; }

    // Drop any partial frame and hunt for the magic again. Called when a
    // session ends, so bytes still in flight from an abandoned transfer cannot
    // be mistaken for the start of the next one.
    void reset() { _state = SYNC; _matched = 0; }

private:
    enum State : uint8_t { SYNC = 0, TYPE, FLAGS, LEN0, LEN1, PAYLOAD, CRC };

    State    _state   = SYNC;
    uint8_t  _matched = 0;   // magic bytes matched so far
    uint8_t  _type    = 0;
    uint16_t _len     = 0;
    uint16_t _got     = 0;   // payload bytes received
    uint8_t  _crcGot  = 0;   // CRC bytes received
    uint32_t _crcWire = 0;   // CRC as it arrived, little-endian
    uint32_t _crcCalc = 0;   // CRC as computed over what arrived

    uint8_t  _buf[OTA_FRAME_MAX_PAYLOAD];
};

// ── The service ───────────────────────────────────────────────────────────
//
// One owner of the session, so there is exactly one place that can arm it and
// exactly one place that pumps it. main.cpp and ui_update.cpp both go through
// here rather than holding an OtaSession of their own.
//
// This namespace exists on EVERY build, including the simulator and the native
// tests, and the update screen is written against it unconditionally. On a
// build with no flash target the session simply has none attached: it still
// arms, still expires, and still reports state, and an image offered to it is
// refused with NO_UPDATE_SLOT. That means the screen has one code path
// everywhere and the simulator renders the real thing rather than a mock of
// it, which is the difference between a screenshot that proves something and a
// screenshot that proves it compiled.
namespace OtaService {

// Attach the flash target and reset the wire. Call once from setup().
void begin();

// Call every loop iteration. Ticks the session so the arm window and the stall
// timeout genuinely expire, then drains whatever the host has sent. Cheap when
// nothing is happening: the common case is one Serial.available() that returns
// zero.
void pump(uint32_t nowMs);

// Feed the session OtaHealth::isPending(). Call this next to OtaHealth::update()
// and read it AFTER that call, so a confirmation that just happened is visible
// immediately. See OtaSession::setRunningImageOnTrial() for what it protects:
// without this line the guard against erasing the last known-good image is
// present in the code and absent on the device.
void setRunningImageOnTrial(bool onTrial);

// True when this build has a real wire and a real flash target, i.e. an update
// could actually arrive. False on the simulator. The screen says so rather than
// offering an arm that could never complete.
bool transportAvailable();

// ── The owner gate ────────────────────────────────────────────────────────
//
// The ONLY route to OtaSession::arm() in the entire firmware. It is called from
// exactly one place, the update screen's confirm action, and nothing that
// arrives on the wire can reach it. Returns false when the session refuses, and
// the caller is expected to look: "the screen says armed and the session is
// not" is its own kind of bug.
bool arm(uint32_t nowMs);

// The owner changing their mind. Refused from READY, where the boot pointer has
// already moved and this call could not honour its own name.
bool cancel();

// Read-only view for the screen. Everything the owner is shown comes from here.
const OtaSession& session();

// How long is left on the arm window, in milliseconds, or 0 when not armed.
// The screen counts this down so an owner who wandered off knows the device did
// not stay open forever.
uint32_t armMsRemaining(uint32_t nowMs);

}  // namespace OtaService
