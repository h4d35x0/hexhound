#include "ota_session.h"

#include <string.h>

#include "../content/content_crypto.h"

// ── HexHound - OTA Session Implementation ───────────────────────

const char* OtaSession::stateName(State s) {
    switch (s) {
        case IDLE:      return "idle";
        case ARMED:     return "armed";
        case RECEIVING: return "receiving";
        case VERIFYING: return "verifying";
        case READY:     return "ready";
        case FAILED:    return "failed";
    }
    return "unknown";
}

void OtaSession::fail(const char* why, OtaImage::Verdict v) {
    // Abort the target on the way out, unconditionally. This is the call that
    // guarantees a failed update leaves the device booting what it already
    // boots, and it must happen on EVERY failure path, which is why every
    // failure path routes through this one function rather than setting the
    // state inline.
    if (_target != nullptr) {
        _target->abort();
    }
    _state      = FAILED;
    _verdict    = v;
    _failReason = why;
    _received   = 0;
}

bool OtaSession::arm(uint32_t nowMs, uint32_t windowMs) {
    // ── Refusing to re-arm from READY ─────────────────────────────────────
    //
    // From READY, setBootPartition() has already run and otadata names the
    // passive slot as the next boot target. A second arm() followed by an
    // offerHeader() would reach _target->begin(), which on hardware is
    // esp_ota_begin(), which ERASES that same slot. The device would then be
    // pointed at an erased partition: it is not merely a lost update, it is a
    // device that does not come back.
    //
    // Two ways to make that safe. Revert the boot pointer to the running
    // partition first, or refuse until the owner reboots. This takes the
    // refusal, because reverting means a new irreversible operation on the
    // target interface, only exercisable on real hardware, added for the sake
    // of a case where the owner already has a perfectly good move available:
    // restart, which is what they were going to do anyway. Adding an untested
    // write path to otadata in order to protect otadata is the wrong trade on
    // a device with no recovery path.
    //
    // Checked before the on-trial gate below only so a refusal here cannot
    // overwrite the verdict that describes the image already staged. Neither
    // refusal proceeds, so the order between them cannot make either less safe.
    if (_state == READY) {
        _failReason = "already_staged";
        return false;
    }

    // See setRunningImageOnTrial(). The slot a new update would erase is the
    // previous, known-good firmware while this image is still proving itself,
    // and erasing it would leave the device running something unconfirmed with
    // nothing behind it. A trial lasts seconds; this refusal is cheap and it
    // expires on its own.
    if (_runningOnTrial) {
        _failReason = "running_image_on_trial";
        _verdict    = OtaImage::UPDATE_ON_TRIAL;
        return false;
    }

    // Re-arming from a terminal state is how the owner retries after a failed
    // attempt without power-cycling. Re-arming mid-transfer is not: it would
    // let a second host interrupt the first, so the in-flight attempt is torn
    // down first and the target aborted.
    if (_state == RECEIVING || _state == VERIFYING) {
        fail("rearmed_during_transfer", OtaImage::UPDATE_ABANDONED);
    }

    _state      = ARMED;
    _armedAtMs  = nowMs;
    _windowMs   = windowMs;
    _received   = 0;
    _verdict    = OtaImage::ACCEPTED;
    _failReason = "";
    memset(&_header, 0, sizeof(_header));
    return true;
}

bool OtaSession::cancel(const char* why) {
    if (_state == IDLE) {
        return true;
    }

    // The same one-way door as arm(). From READY the boot pointer has moved
    // and this function cannot undo that, so returning to IDLE here would put
    // the session in a state that says no update is staged while the device is
    // still going to restart into one. An owner who is told an update was
    // cancelled and then watches it install anyway has been lied to by the one
    // control they were given.
    if (_state == READY) {
        _failReason = "already_staged";
        return false;
    }

    fail(why != nullptr ? why : "cancelled", OtaImage::UPDATE_ABANDONED);
    _state = IDLE;
    return true;
}

void OtaSession::tick(uint32_t nowMs) {
    if (_state == ARMED) {
        // Unsigned arithmetic, so this stays correct across the millis()
        // rollover at 49.7 days rather than arming forever once.
        if ((uint32_t)(nowMs - _armedAtMs) >= _windowMs) {
            _state      = IDLE;
            _failReason = "arm_window_expired";
        }
        return;
    }

    if (_state == RECEIVING) {
        if ((uint32_t)(nowMs - _lastRxMs) >= OTA_RX_STALL_MS) {
            fail("transfer_stalled", OtaImage::TRANSFER_STALLED);
        }
    }
}

OtaImage::Verdict OtaSession::offerHeader(const uint8_t* buf, size_t len, uint32_t nowMs) {
    // The owner gate. A host cannot reach any of the code below without a
    // person having confirmed on the device first. This check is the entire
    // implementation of "no automatic updates and no silent ones", so it is
    // the first thing in the function and there is no path around it.
    if (_state != ARMED) {
        // NOT_ARMED, not BAD_FLAGS. The owner reads these: BAD_FLAGS renders as
        // "this update uses options this firmware does not know", which would
        // send somebody off to find a different update file to fix a problem
        // that is entirely about not having pressed confirm on the device.
        _failReason = "not_armed";
        return OtaImage::NOT_ARMED;
    }

    // The second safety gate, and the reason it is checked here as well as in
    // arm(): arming can legitimately precede the trial resolving, and what
    // must never happen is not "arming during a trial" but WRITING during one.
    // This is the last point before the target is touched, so this is where the
    // guarantee is actually made. See setRunningImageOnTrial().
    if (_runningOnTrial) {
        fail("running_image_on_trial", OtaImage::UPDATE_ON_TRIAL);
        return OtaImage::UPDATE_ON_TRIAL;
    }

    if (_target == nullptr) {
        fail("no_target", OtaImage::NO_UPDATE_SLOT);
        return OtaImage::NO_UPDATE_SLOT;
    }

    // The real partition size, from the target, not a compiled-in guess. A
    // bound checked against an assumption is decoration.
    const uint32_t slot = _target->slotBytes();

    // Asked before the header is looked at, because the answer does not depend
    // on the header. A device with no usable slot refuses every image, and
    // telling the owner "this update is too big" (which is what a zero slot
    // size makes checkFit say) sends them looking for a smaller file that does
    // not exist. checkFit still refuses a zero slot on its own; it is a public
    // function and callers other than this one exist.
    if (slot == 0) {
        fail("no_update_slot", OtaImage::NO_UPDATE_SLOT);
        return OtaImage::NO_UPDATE_SLOT;
    }

    // _trusted is OtaImage::firmwareKeys() unless a test substituted it. See
    // setTrustedKeys(); nothing on the device calls that.
    OtaImage::Header parsed;
    const OtaImage::Verdict v = OtaImage::accept(buf, len, slot, parsed, _trusted);
    if (v != OtaImage::ACCEPTED) {
        // Note what has NOT happened at this point: the target's begin() was
        // never called, so the passive slot has not been erased and not one
        // byte has been written. An unsigned or wrong-key image never gets as
        // far as touching flash, let alone as far as being marked bootable.
        fail(OtaImage::verdictName(v), v);
        return v;
    }

    _header = parsed;

    // Everything about the image has now been proven. If begin() still refuses,
    // the problem is this device's flash, not the update, and the verdict says
    // so rather than blaming the file.
    if (!_target->begin(_header.imageLen)) {
        fail("begin_failed", OtaImage::DEVICE_FLASH_FAILED);
        return OtaImage::DEVICE_FLASH_FAILED;
    }

    _received = 0;
    _lastRxMs = nowMs;
    _state    = RECEIVING;
    return OtaImage::ACCEPTED;
}

bool OtaSession::offerChunk(const uint8_t* data, size_t len, uint32_t nowMs) {
    if (_state != RECEIVING) {
        _failReason = "not_receiving";
        return false;
    }

    if (data == nullptr) {
        fail("null_chunk", OtaImage::BAD_CHUNK);
        return false;
    }

    // A zero-length chunk is a no-op, not an error. It refreshes the stall
    // timer, which lets a slow host hold the session open honestly rather than
    // by padding the image.
    if (len == 0) {
        _lastRxMs = nowMs;
        return true;
    }

    if (len > HEXHOUND_OTA_CHUNK_MAX) {
        fail("chunk_too_large", OtaImage::BAD_CHUNK);
        return false;
    }

    // The overrun refusal. The remaining count comes from the SIGNED image
    // length, so a host cannot talk its way past it. Refusing outright rather
    // than clamping to the remaining bytes is deliberate: a host that is wrong
    // about the total length has already sent bytes that cannot be trusted to
    // be the right ones, and finishing the transfer would produce an image
    // that fails the digest check anyway, several seconds later, having erased
    // the slot for nothing.
    if ((uint64_t)_received + len > (uint64_t)_header.imageLen) {
        fail("overrun", OtaImage::TRANSFER_OVERRUN);
        return false;
    }

    if (!_target->write(data, len)) {
        fail("write_failed", OtaImage::DEVICE_FLASH_FAILED);
        return false;
    }

    _received += (uint32_t)len;
    _lastRxMs  = nowMs;
    return true;
}

OtaImage::Verdict OtaSession::finish() {
    if (_state != RECEIVING) {
        // A sequence error, not an integrity failure. DIGEST_MISMATCH here
        // would tell the owner their update did not arrive intact, when in
        // fact no image was ever offered.
        _failReason = "not_receiving";
        return OtaImage::OUT_OF_SEQUENCE;
    }

    // The truncation refusal. A short transfer is not finished early, it is
    // failed. There is no "close enough" for a boot image.
    if (_received != _header.imageLen) {
        fail("truncated", OtaImage::DIGEST_MISMATCH);
        return OtaImage::DIGEST_MISMATCH;
    }

    _state = VERIFYING;

    // end() also runs the SDK's own structural check of the image on hardware.
    // That is a genuinely independent check from the digest below: it asks
    // "is this a well-formed application" where the digest asks "is this the
    // application that was signed". Both are worth having.
    if (!_target->end()) {
        fail("image_invalid", OtaImage::DIGEST_MISMATCH);
        return OtaImage::DIGEST_MISMATCH;
    }

    // ── Hash what is ON FLASH, not what came off the wire ─────────────────
    //
    // Accumulating the digest from the incoming stream would attest to a copy
    // of the image that no longer exists anywhere. The thing that will boot is
    // the thing sitting in the partition, and a flash write that silently did
    // not stick is exactly the failure this check has to catch. So the bytes
    // are read back out and hashed from there.
    //
    // Streamed in HEXHOUND_OTA_CHUNK_MAX reads through a shared incremental
    // SHA-512. Nothing here holds more than one chunk, which is what makes a
    // 1.2 MB image checkable on a board with 250 KB of RAM and no PSRAM.
    ContentCrypto::Sha512Ctx ctx;
    ContentCrypto::sha512Init(ctx);

    uint8_t buf[HEXHOUND_OTA_CHUNK_MAX];
    uint32_t off = 0;
    while (off < _header.imageLen) {
        uint32_t n = _header.imageLen - off;
        if (n > HEXHOUND_OTA_CHUNK_MAX) {
            n = HEXHOUND_OTA_CHUNK_MAX;
        }

        // Unreachable today, and deliberately here anyway. `off < imageLen`
        // guarantees the subtraction above is at least 1, and clamping only
        // ever lowers it to 4096, so nothing can currently make this fire.
        //
        // What it buys is the failure MODE if that ever stops being true. This
        // loop advances only by `n`, so an arithmetic slip that produced zero
        // would not compute a wrong digest, it would spin forever inside
        // finish(), and there is no backstop for that: the trial watchdog is
        // armed only during the pending-verify window after a reboot, never
        // during an update. The owner would be holding a device that stopped
        // responding partway through an update, needing a power cycle, with no
        // idea whether it was safe to unplug. It is not a brick, since the boot
        // pointer has not moved, but it is the worst-feeling five minutes this
        // firmware could hand somebody.
        //
        // A hang is the one failure this file cannot report, so it is worth ten
        // bytes to convert it into one that can be. Same reasoning as the
        // duplicated bounds on write(): two independent checks on the one
        // operation that can corrupt a partition is not redundancy worth
        // removing.
        if (n == 0) {
            fail("readback_zero_length", OtaImage::DEVICE_FLASH_FAILED);
            return OtaImage::DEVICE_FLASH_FAILED;
        }

        if (!_target->read(off, buf, n)) {
            // The flash would not give back what it was given. That is this
            // device's storage failing, not the update being wrong, and the
            // two send the owner in completely different directions.
            fail("readback_failed", OtaImage::DEVICE_FLASH_FAILED);
            return OtaImage::DEVICE_FLASH_FAILED;
        }
        ContentCrypto::sha512Update(ctx, buf, n);
        off += n;
    }

    uint8_t flashDigest[OTA_DIGEST_BYTES];
    ContentCrypto::sha512Finish(ctx, flashDigest);

    const OtaImage::Verdict dv = OtaImage::checkFlashDigest(_header, flashDigest);
    if (dv != OtaImage::ACCEPTED) {
        fail("digest_mismatch", dv);
        return dv;
    }

    // ── The one irreversible step, and it is last ─────────────────────────
    //
    // Everything above this line can be abandoned with no consequence. This
    // call is the first thing that changes what the device will boot, and by
    // the time it runs the image has been proven authentic, proven to be for
    // this board, proven to fit, and proven to be intact on the flash it will
    // boot from.
    //
    // Even so it is only PENDING. The bootloader gives the new image exactly
    // one chance to confirm itself; see ota_health.h.
    //
    // And note what stops being true the moment it succeeds. Below this line
    // the session is no longer abandonable, because abandoning it now would
    // mean erasing the slot otadata points at. That is why READY refuses both
    // arm() and cancel(): the only honest way out of this state is the restart
    // the owner is about to perform.
    if (!_target->setBootPartition()) {
        // The image was good and the device could not switch to it. Saying
        // "digest mismatch" here would have blamed a perfectly valid update
        // file for a failure that happened entirely on this side.
        fail("set_boot_failed", OtaImage::SET_BOOT_FAILED);
        return OtaImage::SET_BOOT_FAILED;
    }

    _state   = READY;
    _verdict = OtaImage::ACCEPTED;
    return OtaImage::ACCEPTED;
}
