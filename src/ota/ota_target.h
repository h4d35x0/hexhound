#pragma once

#include <stdint.h>
#include <stddef.h>

#include "ota_identity.h"

// ── HexHound - OTA Flash Target ─────────────────────────────────
//
// The narrow seam between the update state machine and whatever is actually
// receiving the bytes. It exists so the state machine, which is where every
// bound and every ordering rule lives, is ordinary portable C++ that can be
// tested exhaustively on a desktop with a fake flash, instead of only being
// exercisable by flashing a real device and hoping.
//
// Deliberately small. Six operations, no state of its own that the session
// cares about, nothing that can be called out of order without the session
// noticing. Everything interesting happens on the other side of this
// interface, which is where it can be tested.
//
// Implementations:
//   * EspOtaTarget   (ota_target.cpp, hardware)  - esp_ota_* on the passive
//                                                  app partition
//   * MemoryOtaTarget (below, simulator + tests) - a plain RAM buffer

class OtaFlashTarget {
public:
    virtual ~OtaFlashTarget() {}

    // Size in bytes of the slot that would receive an image. Zero means there
    // is no usable target, which the session treats as a hard refusal rather
    // than as a zero-length write.
    virtual uint32_t slotBytes() const = 0;

    // Prepare to receive exactly `imageLen` bytes. May erase. Returns false if
    // the target cannot be prepared, in which case nothing has been promised
    // and the device is unchanged.
    virtual bool begin(uint32_t imageLen) = 0;

    // Append. The session guarantees it never offers more in total than the
    // imageLen passed to begin(), and never more than HEXHOUND_OTA_CHUNK_MAX
    // in one call, but an implementation must still refuse rather than
    // overrun if it is ever called otherwise.
    virtual bool write(const uint8_t* data, size_t len) = 0;

    // Finalise the write. On hardware this also runs the SDK's own structural
    // validation of the image. Does NOT make the image bootable.
    virtual bool end() = 0;

    // Give up. Must leave the device booting exactly what it was booting
    // before begin() was called. Safe to call at any point, including twice.
    virtual void abort() = 0;

    // Read back what was written. This is how the image is hashed for the
    // digest check, so it must read the durable bytes and not a cached copy of
    // what was handed to write().
    virtual bool read(uint32_t offset, uint8_t* out, size_t len) const = 0;

    // Make the freshly written slot the boot target, as a PENDING image that
    // must confirm itself after it boots or be rolled back automatically.
    //
    // The session calls this exactly once, only after the signature verified
    // and only after the on-flash digest matched. It is the single irreversible
    // step in the whole flow and it is deliberately the last one.
    virtual bool setBootPartition() = 0;
};

// ── In-memory target ──────────────────────────────────────────────────────
//
// Used by the simulator and by the native tests. Not a mock in the sense of
// "records calls and asserts on them": it is a real, if simple, implementation
// with real bounds, so the state machine under test is the same state machine
// that runs on the device.
//
// It also models the failure modes that matter. A test can make begin(),
// write(), end(), read() or setBootPartition() fail on demand, and can corrupt
// the stored bytes after the fact, which is how the "written bytes are not the
// signed bytes" path gets exercised without a flaky cable.
class MemoryOtaTarget : public OtaFlashTarget {
public:
    MemoryOtaTarget(uint8_t* storage, uint32_t capacity)
        : _buf(storage), _cap(capacity) {}

    uint32_t slotBytes() const override { return _failSlotSize ? 0 : _cap; }

    bool begin(uint32_t imageLen) override {
        _beginCalls++;
        if (_failBegin || imageLen > _cap) return false;
        _expected = imageLen;
        _written  = 0;
        _open     = true;
        // _booted is deliberately NOT cleared here. esp_ota_begin() erases the
        // PARTITION; it does not touch otadata, and only setBootPartition()
        // ever does. Clearing it made this fake kinder than the hardware in the
        // one direction that mattered: it could not represent "the boot pointer
        // names a slot that has just been erased", which is the exact state a
        // re-arm from READY used to produce, so a test written against it would
        // have watched the brick happen and called it clean.
        return true;
    }

    bool write(const uint8_t* data, size_t len) override {
        if (!_open || _failWrite) return false;
        // Refuses rather than overruns even though the session already bounds
        // this. Two independent bounds on the one operation that can corrupt
        // a partition is not redundancy worth removing.
        if (len > HEXHOUND_OTA_CHUNK_MAX) return false;
        if ((uint64_t)_written + len > _expected) return false;
        for (size_t i = 0; i < len; i++) _buf[_written + i] = data[i];
        _written += (uint32_t)len;
        return true;
    }

    bool end() override {
        if (!_open || _failEnd) return false;
        if (_written != _expected) return false;
        _open = false;
        return true;
    }

    void abort() override { _abortCalls++; _open = false; _written = 0; }

    bool read(uint32_t offset, uint8_t* out, size_t len) const override {
        if (out == nullptr) return false;
        // Flash that took the write and will not give it back. This is not a
        // theoretical mode: it is the whole reason the digest is computed by
        // reading the partition rather than by accumulating the wire stream,
        // and without this switch it is the one failure branch in the session
        // that nothing can reach.
        if (_failRead) return false;
        // Bounded against what was WRITTEN, not against the capacity. Reading
        // past the write mark used to succeed and hand back whatever the
        // caller's storage array happened to contain, which is uninitialised
        // memory: a digest computed over it is a different number on different
        // runs, and a test built on that fails at random and gets blamed on the
        // crypto. A real partition has erased 0xFF out there and this one has
        // nothing defined at all, so refusing is the only answer that is not a
        // guess.
        if ((uint64_t)offset + len > _written) return false;
        for (size_t i = 0; i < len; i++) out[i] = _buf[offset + i];
        return true;
    }

    bool setBootPartition() override {
        _setBootCalls++;
        if (_failSetBoot) return false;
        _booted = true;
        return true;
    }

    // ── Test controls ─────────────────────────────────────────────────────
    bool     bootPartitionSet() const { return _booted; }
    uint32_t bytesWritten()     const { return _written; }
    uint8_t* data()             const { return _buf; }

    // ── Pure observation ──────────────────────────────────────────────────
    //
    // Counters only. They change no behaviour and nothing in this class reads
    // them; they exist because the single most important property of the
    // session is a NEGATIVE one, "a rejected header never called begin()", and
    // a negative cannot be proved by looking at the resulting bytes.
    //
    // In particular begin() deliberately does not scrub _buf (see the note
    // there), so "the staged image is still in the buffer" is NOT evidence
    // that the slot was left alone. On hardware begin() erases. beginCalls()
    // is the only thing that distinguishes those two worlds, which is exactly
    // the distinction the re-arm-from-READY brick turned on.
    uint32_t beginCalls()   const { return _beginCalls; }
    uint32_t abortCalls()   const { return _abortCalls; }
    uint32_t setBootCalls() const { return _setBootCalls; }

    void failBegin(bool v)    { _failBegin = v; }
    void failWrite(bool v)    { _failWrite = v; }
    void failEnd(bool v)      { _failEnd = v; }
    void failRead(bool v)     { _failRead = v; }
    void failSetBoot(bool v)  { _failSetBoot = v; }
    void failSlotSize(bool v) { _failSlotSize = v; }

    // Flip a bit in the stored image after it was written, to model flash that
    // did not durably hold what it was given. The digest check exists for
    // exactly this and cannot be shown to work without it.
    void corrupt(uint32_t offset) {
        if (offset < _cap) _buf[offset] ^= 0x01;
    }

private:
    uint8_t* _buf;
    uint32_t _cap;
    uint32_t _expected     = 0;
    uint32_t _written      = 0;
    bool     _open         = false;
    bool     _booted       = false;
    bool     _failBegin    = false;
    bool     _failWrite    = false;
    bool     _failEnd      = false;
    bool     _failRead     = false;
    bool     _failSetBoot  = false;
    bool     _failSlotSize = false;
    uint32_t _beginCalls   = 0;
    uint32_t _abortCalls   = 0;
    uint32_t _setBootCalls = 0;
};

#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
// The real one. Defined in ota_target.cpp, hardware builds only.
OtaFlashTarget* espOtaTarget();
#endif
