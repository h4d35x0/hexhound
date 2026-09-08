#include "ota_serial.h"

#include <string.h>

#include "ota_health.h"
#include "ota_identity.h"
#include "ota_target.h"

#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)
#include <stdio.h>
#else
#include <Arduino.h>
#endif

// ── HexHound - OTA Serial Transport Implementation ──────────────

const uint8_t OTA_MAGIC_H2D[OTA_FRAME_MAGIC_BYTES] = { 0xA5, 0x48, 0x58, 0x44 };
const uint8_t OTA_MAGIC_D2H[OTA_FRAME_MAGIC_BYTES] = { 0x5A, 0x48, 0x58, 0x48 };

// ── CRC32 ─────────────────────────────────────────────────────────────────
//
// Nibble table rather than the usual 256-entry byte table. The byte table is
// 1 KB of flash for roughly a 2x speedup on a check that is already far from
// being the bottleneck: the transfer is bounded by flash erase and write, not
// by arithmetic. Sixty-four bytes is the better trade on a board with no PSRAM.
//
// Bit-exact with zlib and with Python's binascii.crc32, which is the point: the
// host does not carry a second implementation of this and therefore cannot
// carry a second implementation that disagrees.
namespace {

const uint32_t CRC32_NIBBLE[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

}  // namespace

uint32_t otaCrc32(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0Fu];
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0Fu];
    }
    return crc;
}

// ── The reader ────────────────────────────────────────────────────────────

OtaFrameReader::Event OtaFrameReader::feed(uint8_t b) {
    switch (_state) {

    case SYNC:
        // ── Resynchronising after garbage ─────────────────────────────────
        //
        // The naive version of this drops back to "matched zero" on a
        // mismatch, which loses a magic that began inside the garbage: feed it
        // A5 A5 48 58 44 and it never sees the frame, because the second A5 is
        // consumed as a failed second byte. So a mismatch re-tests the CURRENT
        // byte against the first magic byte rather than discarding it.
        //
        // That is exactly KMP, and it is complete here rather than merely
        // better, because all four magic bytes are distinct: no proper prefix
        // of the magic is also a suffix of it, so the failure function is zero
        // everywhere and "restart at one if this byte is the first byte" is the
        // whole algorithm. Keep the magic bytes distinct if it ever changes.
        if (b == OTA_MAGIC_H2D[_matched]) {
            _matched++;
            if (_matched == OTA_FRAME_MAGIC_BYTES) {
                _matched = 0;
                _state   = TYPE;
            }
        } else {
            _matched = (b == OTA_MAGIC_H2D[0]) ? 1 : 0;
        }
        return NONE;

    case TYPE:
        _type    = b;
        _crcCalc = otaCrc32Init();
        _crcCalc = otaCrc32(_crcCalc, &b, 1);
        _state   = FLAGS;
        return NONE;

    case FLAGS:
        _crcCalc = otaCrc32(_crcCalc, &b, 1);
        if (b != 0) {
            // A reserved bit this build does not understand. Refusing rather
            // than ignoring it means a future flag that changes what a frame
            // MEANS cannot be silently misread by an old device as the frame it
            // used to be.
            reset();
            return BAD_FLAGS;
        }
        _state = LEN0;
        return NONE;

    case LEN0:
        _crcCalc = otaCrc32(_crcCalc, &b, 1);
        _len     = b;
        _state   = LEN1;
        return NONE;

    case LEN1:
        _crcCalc = otaCrc32(_crcCalc, &b, 1);
        _len     = (uint16_t)(_len | ((uint16_t)b << 8));
        if (_len > OTA_FRAME_MAX_PAYLOAD) {
            // ── Why this resynchronises instead of skipping the payload ────
            //
            // A length larger than any legal frame did not come from a peer
            // that meant it. It came from a desynchronised stream, which means
            // the two length bytes are not a length at all. Trusting them far
            // enough to skip that many bytes would eat up to 64 KB of the
            // stream, and the real frame boundary is somewhere inside what was
            // just discarded. Going straight back to hunting for the magic
            // costs at most one genuine frame and recovers immediately.
            reset();
            return BAD_LENGTH;
        }
        _got     = 0;
        _crcGot  = 0;
        _crcWire = 0;
        _state   = (_len == 0) ? CRC : PAYLOAD;
        return NONE;

    case PAYLOAD:
        // Bounded by the check in LEN1, so this index cannot leave the buffer.
        _buf[_got++] = b;
        _crcCalc     = otaCrc32(_crcCalc, &b, 1);
        if (_got >= _len) {
            _state = CRC;
        }
        return NONE;

    case CRC:
        _crcWire |= ((uint32_t)b) << (8 * _crcGot);
        _crcGot++;
        if (_crcGot < OTA_FRAME_CRC_BYTES) {
            return NONE;
        }
        _state   = SYNC;
        _matched = 0;
        if (_crcWire != otaCrc32Final(_crcCalc)) {
            return BAD_CRC;
        }
        return FRAME;
    }

    reset();
    return NONE;
}

// ══════════════════════════════════════════════════════════════════════════
//  The service
// ══════════════════════════════════════════════════════════════════════════

namespace {

OtaSession s_session;

// When the current arm window opened. Only meaningful while the session says
// ARMED; see armMsRemaining().
uint32_t s_armedAtMs = 0;

// True only where there is both a Serial to read and a flash target to write.
#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)
const bool s_hasWire = false;
#else
const bool s_hasWire = true;
OtaFrameReader s_reader;

// Last state reported to the host, so a change can be announced exactly once
// rather than every loop iteration. The host waiting for the owner to press
// confirm gets told the moment it happens; a host that missed the frame can
// still poll with HELLO.
//
// Initialised to IDLE, which is what a fresh session already is, so an ordinary
// boot emits NO frame. The serial monitor is the main debugging surface on this
// project and spraying binary into it at every power-up, on every device,
// forever, to announce that nothing is happening would be a poor trade for
// information the host can ask for whenever it wants.
uint8_t s_announced = (uint8_t)OtaSession::IDLE;

// ── Why the pump is bounded ───────────────────────────────────────────────
//
// A host that floods the port must not be able to hold the loop hostage. The
// screen is drawing a progress bar out of the same loop, and a device that
// stops repainting during the one operation the owner is watching looks hung
// even when it is working perfectly. Sixteen kilobytes is four full chunks per
// iteration, which is far more throughput than the flash write behind it can
// absorb, so this bound never limits a well-behaved transfer.
const uint32_t PUMP_MAX_BYTES = 16384;

// ── Sending ───────────────────────────────────────────────────────────────
//
// Header and CRC are built on the stack; the payload is written straight from
// the caller's buffer. Nothing here copies a chunk, and the largest thing this
// function ever holds is twelve bytes.
void sendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
    uint8_t head[OTA_FRAME_HEAD_BYTES];
    memcpy(head, OTA_MAGIC_D2H, OTA_FRAME_MAGIC_BYTES);
    head[4] = type;
    head[5] = 0;
    head[6] = (uint8_t)(len & 0xFF);
    head[7] = (uint8_t)(len >> 8);

    uint32_t crc = otaCrc32Init();
    crc = otaCrc32(crc, head + OTA_FRAME_MAGIC_BYTES,
                   OTA_FRAME_HEAD_BYTES - OTA_FRAME_MAGIC_BYTES);
    if (payload != nullptr && len > 0) {
        crc = otaCrc32(crc, payload, len);
    }
    crc = otaCrc32Final(crc);

    const uint8_t tail[OTA_FRAME_CRC_BYTES] = {
        (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF),
        (uint8_t)((crc >> 16) & 0xFF), (uint8_t)((crc >> 24) & 0xFF)
    };

    Serial.write(head, sizeof(head));
    if (payload != nullptr && len > 0) {
        Serial.write(payload, len);
    }
    Serial.write(tail, sizeof(tail));
}

void putU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void sendIdent() {
    uint8_t p[OTA_IDENT_BYTES];
    memset(p, 0, sizeof(p));

    uint8_t flags = 0;
    if (s_session.runningImageOnTrial())  flags |= OTA_IDENT_FLAG_ON_TRIAL;
    if (OtaHealth::bootedAfterRollback()) flags |= OTA_IDENT_FLAG_AFTER_ROLLBACK;
    if (s_hasWire)                        flags |= OTA_IDENT_FLAG_HAS_TARGET;

    p[0] = OTA_WIRE_VERSION;
    p[1] = (uint8_t)s_session.state();
    p[2] = (uint8_t)s_session.verdict();
    p[3] = flags;
    putU32(p + 4, HEXHOUND_FW_VERSION);

    // The REAL partition size, straight from the target, not the compile-time
    // constant. A host that bounds its own file against a guess is checking
    // nothing; this is the number the device will actually enforce.
    OtaFlashTarget* t = espOtaTarget();
    putU32(p + 8, t != nullptr ? t->slotBytes() : 0);

    p[12] = (uint8_t)(OTA_FRAME_MAX_PAYLOAD & 0xFF);
    p[13] = (uint8_t)(OTA_FRAME_MAX_PAYLOAD >> 8);

    // strncpy over a zeroed field: the longest board id in ota_identity.h is
    // exactly 32 characters and therefore carries no terminator at all, which
    // is legal and is why the field is read as a fixed 32 rather than as a
    // C string.
    strncpy((char*)(p + 16), HEXHOUND_OTA_BOARD_ID, OTA_BOARD_ID_BYTES);

    sendFrame(OTA_F_IDENT, p, sizeof(p));
    s_announced = (uint8_t)s_session.state();
}

void sendProgress() {
    uint8_t p[OTA_PROGRESS_BYTES];
    p[0] = (uint8_t)s_session.state();
    p[1] = (uint8_t)s_session.verdict();
    putU32(p + 2, s_session.bytesWritten());
    putU32(p + 6, s_session.imageLen());
    sendFrame(OTA_F_PROGRESS, p, sizeof(p));
}

// Append a NUL-terminated string, truncating rather than overflowing. These are
// compiled-in constants and cannot realistically overrun, but a RESULT frame is
// the thing the owner is shown when something has ALREADY gone wrong, and a
// buffer overrun in the error path is the classic way a bad day becomes a
// worse one.
size_t appendStr(uint8_t* buf, size_t off, size_t cap, const char* s) {
    if (s == nullptr) s = "";
    while (*s != '\0' && off + 1 < cap) {
        buf[off++] = (uint8_t)*s++;
    }
    if (off < cap) buf[off++] = 0;
    return off;
}

void sendResult() {
    // Fixed part, plus three strings. 192 bytes is comfortably over the longest
    // verdictHelp() in ota_image.cpp plus the other two, and it is a stack
    // local that exists for the length of this call.
    uint8_t p[192];
    p[0] = (uint8_t)s_session.state();
    p[1] = (uint8_t)s_session.verdict();
    putU32(p + 2, s_session.bytesWritten());
    putU32(p + 6, s_session.imageLen());

    size_t off = OTA_RESULT_FIXED_BYTES;
    off = appendStr(p, off, sizeof(p), OtaImage::verdictName(s_session.verdict()));
    off = appendStr(p, off, sizeof(p), OtaImage::verdictHelp(s_session.verdict()));
    off = appendStr(p, off, sizeof(p), s_session.failReason());

    sendFrame(OTA_F_RESULT, p, (uint16_t)off);
    s_announced = (uint8_t)s_session.state();
}

// ── Handling one frame ────────────────────────────────────────────────────
//
// Note what is NOT here. There is no case that calls s_session.arm(). A host
// can ask, offer and abandon; it cannot consent.
void handleFrame(uint32_t nowMs) {
    switch (s_reader.type()) {

    case OTA_F_HELLO:
        // Legal in every state and changes none of them, deliberately. The host
        // needs to be able to ask "what board are you and are you armed yet"
        // without that question itself being an action.
        sendIdent();
        break;

    case OTA_F_BEGIN: {
        const OtaImage::Verdict v =
            s_session.offerHeader(s_reader.payload(), s_reader.length(), nowMs);
        if (v == OtaImage::ACCEPTED) {
            sendProgress();
        } else {
            sendResult();
        }
        break;
    }

    case OTA_F_DATA:
        if (s_session.offerChunk(s_reader.payload(), s_reader.length(), nowMs)) {
            sendProgress();
        } else {
            sendResult();
        }
        break;

    case OTA_F_COMMIT:
        // finish() is the expensive one: it reads the whole image back off
        // flash and hashes it. Seconds, not milliseconds, and the loop is
        // blocked throughout. That is acceptable exactly here and nowhere else,
        // because the alternative is a resumable hash spread across loop
        // iterations that could be interrupted halfway and would then be
        // attesting to a partition somebody else had touched.
        s_session.finish();
        sendResult();
        break;

    case OTA_F_ABORT:
        s_session.cancel("host_abort");
        s_reader.reset();   // bytes still in flight are not the next transfer
        sendResult();
        break;

    default:
        // An unknown type, or a device-to-host type arriving on the host-to-
        // device wire, which means something is echoing. Answer with IDENT
        // rather than silently: a host that gets no reply cannot tell a device
        // that refused from a cable that is not connected.
        sendIdent();
        break;
    }
}
#endif  // !SIMULATOR_BUILD && !UNIT_TEST

}  // namespace

namespace OtaService {

void begin() {
#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
    s_session.attach(espOtaTarget());
    s_reader.reset();
    Serial.printf("[OTA] Serial transport ready. Board %s, fw %u.%u.%u, "
                  "wire v%d, chunk %d. Updates must be armed ON THE DEVICE.\n",
                  HEXHOUND_OTA_BOARD_ID,
                  (unsigned)HEXHOUND_FW_VERSION_MAJOR,
                  (unsigned)HEXHOUND_FW_VERSION_MINOR,
                  (unsigned)HEXHOUND_FW_VERSION_PATCH,
                  OTA_WIRE_VERSION, OTA_FRAME_MAX_PAYLOAD);
#endif
}

void setRunningImageOnTrial(bool onTrial) {
    s_session.setRunningImageOnTrial(onTrial);
}

bool transportAvailable() { return s_hasWire; }

bool arm(uint32_t nowMs) {
    const bool ok = s_session.arm(nowMs, OTA_ARM_WINDOW_MS);
    if (ok) s_armedAtMs = nowMs;
#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
    // Tell the host immediately. It has been sitting in a poll loop waiting for
    // exactly this, and the arm window is two minutes: spending several seconds
    // of it waiting for the host's next poll is time the owner is standing
    // there for no reason.
    if (ok) sendIdent();
#endif
    return ok;
}

bool cancel() {
    const bool ok = s_session.cancel("owner_cancelled");
#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
    if (ok) {
        s_reader.reset();
        sendResult();
    }
#endif
    return ok;
}

const OtaSession& session() { return s_session; }

uint32_t armMsRemaining(uint32_t nowMs) {
    // Gated on the SESSION's state, not on this module's own bookkeeping. The
    // session is what actually enforces the window, so asking it first means
    // the countdown on the screen cannot claim time the session has already
    // taken away. s_armedAtMs only answers "how much of it is left", and only
    // while the session agrees there is any.
    if (s_session.state() != OtaSession::ARMED) return 0;
    // Unsigned, so this stays correct across the millis() rollover at 49.7
    // days rather than reading as a window that never ends.
    const uint32_t elapsed = (uint32_t)(nowMs - s_armedAtMs);
    if (elapsed >= OTA_ARM_WINDOW_MS) return 0;
    return OTA_ARM_WINDOW_MS - elapsed;
}

void pump(uint32_t nowMs) {
    // Unconditional and first. The arm window and the stall timeout are only
    // real if something drives them, and a session that stays ARMED because
    // nobody ticked it is a device quietly accepting firmware on a desk.
    s_session.tick(nowMs);

#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
    // A state change nothing on the wire caused: the arm window expiring, or a
    // transfer stalling. The host is waiting on one of those and would
    // otherwise sit there until its own timeout.
    const uint8_t st = (uint8_t)s_session.state();
    if (st != s_announced) {
        s_announced = st;
        sendIdent();
    }

    uint32_t budget = PUMP_MAX_BYTES;
    while (budget > 0 && Serial.available() > 0) {
        budget--;
        const int c = Serial.read();
        if (c < 0) break;

        switch (s_reader.feed((uint8_t)c)) {
        case OtaFrameReader::FRAME:
            handleFrame(nowMs);
            break;

        case OtaFrameReader::BAD_CRC:
        case OtaFrameReader::BAD_LENGTH:
        case OtaFrameReader::BAD_FLAGS:
            // ── Fail the transfer, do not try to carry on ─────────────────
            //
            // A corrupt frame means the stream is no longer trustworthy, and
            // the bytes after it are as suspect as the bytes in it. Continuing
            // would write unknown data into the slot and discover the problem
            // at the digest check a megabyte later, having blamed the image.
            // So the attempt dies here, named, at the point it went wrong.
            //
            // Only a transfer in progress is torn down. Garbage arriving at an
            // IDLE device is just garbage on a serial port, which is normal:
            // any terminal a person opens sends something eventually, and that
            // must not be reportable as a failed update.
            if (s_session.state() == OtaSession::RECEIVING ||
                s_session.state() == OtaSession::ARMED) {
                s_session.cancel("frame_error");
                sendResult();
            }
            s_reader.reset();
            break;

        case OtaFrameReader::NONE:
        default:
            break;
        }
    }
#else
    (void)nowMs;
#endif
}

}  // namespace OtaService
