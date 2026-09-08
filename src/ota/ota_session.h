#pragma once

#include <stdint.h>
#include <stddef.h>

#include "ota_image.h"
#include "ota_target.h"

// ── HexHound - OTA Session State Machine ────────────────────────
//
// The order of operations for installing firmware, with every bound in it,
// written as portable code that a desktop test can drive through all of its
// failure paths.
//
// This is the file to read if you want to know whether the device can brick.
//
// ── The rule the whole design serves ──────────────────────────────────────
//
// HexHound is a giveaway device. There is no support channel, no RMA, and the
// person holding it is not going to open a serial console. A device that will
// not boot is not a bug report, it is a piece of electronic waste with someone's
// pet on it. So every decision below resolves the same way: when in doubt, do
// nothing and keep running the firmware that already works.
//
// ── Sequence ──────────────────────────────────────────────────────────────
//
//   IDLE
//     The device refuses every update verb. This is where it sits.
//
//   ARMED            <- requires the OWNER, on the device
//     arm() is called only from the confirmation screen. Nothing arms itself,
//     nothing arms on a timer, and no host command can reach this state. The
//     window expires on its own so a device left armed on a desk does not stay
//     that way. No automatic updates and no silent ones means the update
//     cannot even BEGIN without a person, not merely that it asks first.
//
//   HEADER REJECTED  <- terminal, and nothing was written
//     offerHeader() parses, bounds-checks and Ed25519-verifies the 192-byte
//     header. An unsigned image, a wrong-key image, an image for another board
//     and an oversized image all die here, and the target's begin() is never
//     called, so the passive slot is not even erased. "Verify before commit"
//     is not the strongest claim available; "verify before WRITE" is, and this
//     is that.
//
//   RECEIVING
//     Chunks stream in. Each is bounded against HEXHOUND_OTA_CHUNK_MAX and
//     against the remaining byte count from the SIGNED length. A host that
//     sends one byte too many is refused; the session does not truncate the
//     overrun and carry on, because a host that is wrong about the length is a
//     host whose earlier bytes are also suspect.
//
//   VERIFYING
//     The image is read BACK OUT of the target and hashed, then compared
//     against the digest the signature vouched for. Nothing that arrived on
//     the wire is trusted here; only what is durably on flash is.
//
//   READY
//     Verified. setBootPartition() has marked the new slot PENDING. The device
//     still boots the OLD firmware until it restarts, and the restart is the
//     owner's to trigger.
//
//     READY IS A ONE-WAY DOOR. It is the only state this session cannot be
//     talked out of. arm() and cancel() both refuse from here, and the reason
//     is that otadata now points at the passive slot: a second arm() followed
//     by offerHeader() would call begin(), which calls esp_ota_begin(), which
//     ERASES the partition the bootloader has been told to boot next. The
//     device would then be pointed at an erased slot. The owner's way out is
//     the restart they were already going to do, and the trial-and-rollback
//     path in ota_health.h is what makes that restart safe.
//
//   FAILED
//     Terminal for this attempt. The target has been aborted, the device is
//     unchanged, and it still boots what it always booted.
//
// ── Where the bricks would have been ──────────────────────────────────────
//
//   * Writing before verifying. Handled: the signature gate is upstream of
//     begin(), not downstream of end().
//   * Marking the image bootable and then checking it. Handled:
//     setBootPartition() is the last call in the flow, after the on-flash
//     digest matched.
//   * Trusting the length from the wire. Handled: the length comes out of the
//     signed header and is checked against the real partition size.
//   * A truncated transfer being committed. Handled: end() requires the exact
//     signed byte count, and a session that stalls is aborted, not finished.
//   * Erasing the slot otadata already points at. Handled: READY refuses
//     arm() and cancel(), so nothing can call begin() on a partition the
//     bootloader has been told to boot next. This one was live in the first
//     draft of this file.
//   * Erasing the only known-good image. Handled: while the RUNNING image is
//     itself on trial, the "next" partition is not spare space, it is the
//     PREVIOUS firmware and the only thing there is to roll back to. Starting
//     an update then would leave the device running an unconfirmed image with
//     nothing behind it. See setRunningImageOnTrial() below. This one was also
//     live in the first draft, and it is the worse of the two.
//   * A verified-but-broken image booting forever. NOT handled here. That is
//     the rollback path, in ota_health.h, and it is the other half of the
//     guarantee.

class OtaSession {
public:
    enum State : uint8_t {
        IDLE = 0,
        ARMED,
        RECEIVING,
        VERIFYING,
        READY,
        FAILED
    };

    // Stable wire names for the companion app.
    static const char* stateName(State s);

    // The session does not own the target. On hardware it points at the ESP
    // implementation; in tests it points at a MemoryOtaTarget.
    void attach(OtaFlashTarget* target) { _target = target; }

    // ── The trusted key list ──────────────────────────────────────────────
    //
    // Which keys may authorise replacing this firmware. Defaults to
    // OtaImage::firmwareKeys(), the list compiled in from ota_pubkey.h, and
    // NOTHING ON THE DEVICE CALLS THE SETTER. The default path is the same
    // decision, against the same keys, as before this seam existed.
    //
    // It exists so a native test can drive this state machine with a fixture
    // key. Without it a test has to redefine OTA_TRUSTED_KEYS and
    // OTA_TRUSTED_KEY_IDS with the preprocessor before including
    // ota_image.cpp, which is a test that has to lie about the trust anchor in
    // order to run, and which silently stops matching the code the day
    // firmwareKeys() is built differently. OtaImage::checkSignature() already
    // takes its ring for exactly this reason; this is the same seam, moved one
    // layer out so it actually reaches the code under test.
    //
    // WHAT THIS DOES NOT CHANGE, and the reasoning is the same as the note on
    // OtaImage::KeyRing. There is no path from a host, an update file, or
    // anything else on the wire to this setter. offerHeader() reads the ring
    // and never writes it, and no verb on this class takes a key from its
    // caller. Reaching it means editing firmware source, which is the same bar
    // as editing ota_pubkey.h itself. The seam moves what a TEST can
    // substitute, not what a running device will accept.
    //
    // Held by VALUE. A KeyRing is three words and copying it removes the whole
    // class of bug where the session outlives the ring it was handed. The key
    // arrays it points at are static const in every caller, on the device and
    // in the tests both.
    // COMPILED OUT OF DEVICE BUILDS ENTIRELY. The argument above is sound as
    // far as it goes: nothing on the device calls this today, and reaching it
    // means editing firmware source. But "nothing calls it today" is a fact
    // about the current tree, not a property of the design, and the thing it
    // guards is the most valuable decision in the project: who is allowed to
    // replace this firmware.
    //
    // The stated principle is that this is not data. A runtime setter sitting
    // in the device build is exactly the affordance someone needs to make it
    // data, and the plausible way that happens is not malice: it is a future
    // transport or content path that has a key in hand and a session in front
    // of it, and a setter that looks like it was provided to be used. Deleting
    // it from the build means that mistake cannot be written, rather than
    // being caught in review.
    //
    // The test build keeps it, which is the entire reason it exists. Nothing
    // about the trust decision differs between the two builds; only the
    // ability to substitute the anchor does, and only where there is no device
    // to compromise. `_trusted` itself is unconditional, so the class layout
    // is identical in both builds.
#if defined(UNIT_TEST) || defined(SIMULATOR_BUILD)
    void setTrustedKeys(const OtaImage::KeyRing& keys) { _trusted = keys; }
#endif
    const OtaImage::KeyRing& trustedKeys() const { return _trusted; }

    // ── Is the RUNNING image itself on trial? ─────────────────────────────
    //
    // Injected rather than read from a global, for two reasons. The first is
    // layering: this state machine has no ESP dependency and is not going to
    // acquire one. The second is that a fact this important has to be drivable
    // from both sides in a test, and a native test cannot make a global
    // esp_ota_* call answer "yes".
    //
    // WHY IT MATTERS. esp_ota_get_next_update_partition() returns the slot
    // after the running one. If the running image is PENDING_VERIFY, that slot
    // is not spare space: it holds the PREVIOUS, known-good firmware, and it is
    // the only thing the bootloader has to fall back to. Beginning an update
    // erases it. The device is then running an image that has not yet proven
    // itself, with nothing behind it. Refusing for the few seconds a trial
    // lasts costs the owner nothing; not refusing costs them the safety net at
    // the exact moment it is load-bearing.
    //
    // The caller feeds this from OtaHealth::isPending() (hardware) or sets it
    // directly (tests). WIRING THIS IS NOT OPTIONAL on hardware. The default of
    // false is correct for a device that has only ever been USB-flashed, which
    // is why it is the default, and a stale value can only ever be stale in the
    // safe direction: pending goes true -> false and never back, so a late
    // refresh refuses an update that would have been fine rather than allowing
    // one that would not.
    void setRunningImageOnTrial(bool onTrial) { _runningOnTrial = onTrial; }
    bool runningImageOnTrial() const { return _runningOnTrial; }

    // ── Owner-initiated arming ────────────────────────────────────────────
    //
    // `nowMs` and `windowMs` make the expiry testable without sleeping. The
    // caller supplies the clock; this file has no opinion about where time
    // comes from and no way to arm itself.
    //
    // Returns false, and changes NOTHING, when arming is refused: from READY,
    // because otadata already points at the passive slot and re-arming would
    // lead to erasing it, and while the running image is on trial. The caller
    // is expected to look, because "the confirmation screen says armed and the
    // session is not" is its own kind of bug.
    bool arm(uint32_t nowMs, uint32_t windowMs);

    // Drives the arming timeout and the receive stall timeout. Safe and cheap
    // to call every loop iteration.
    void tick(uint32_t nowMs);

    // Give up on the current attempt and leave the device booting what it
    // already boots. Called by the owner, by a host disconnect, or by a stall.
    //
    // Returns false, and changes nothing, from READY. From there the boot
    // pointer has already moved and this call could not honour its own name:
    // it would report "cancelled" while the device still restarted into the
    // new image. Telling the owner an update was cancelled when it was not is
    // not a cosmetic problem, it is the one thing they would act on.
    bool cancel(const char* why);

    // ── The gate ──────────────────────────────────────────────────────────
    //
    // Offer the 192-byte header. Returns ACCEPTED only if the image is
    // authentic, is for this board, and fits this slot. Only on ACCEPTED does
    // the session touch the target at all.
    OtaImage::Verdict offerHeader(const uint8_t* buf, size_t len, uint32_t nowMs);

    // Offer image bytes, in order. Returns false on any bound violation or
    // write failure, and the session is FAILED afterwards.
    bool offerChunk(const uint8_t* data, size_t len, uint32_t nowMs);

    // Called once the expected byte count has arrived. Reads the image back
    // off the target, hashes it, compares against the signed digest, and only
    // then marks the slot bootable.
    OtaImage::Verdict finish();

    // ── Observation ───────────────────────────────────────────────────────
    //
    // The invariant worth stating, because it was broken once: a session in
    // FAILED never carries the verdict ACCEPTED. Every failure path names its
    // own cause. ACCEPTED here means one of exactly two things, an image that
    // passed or a session that has not decided anything yet, and never "an
    // update failed but we kept the success code".
    //
    // Calls made in the wrong state (offering a header while not armed,
    // finishing while not receiving) report through the RETURN VALUE and
    // failReason(). They deliberately do not overwrite _verdict, which
    // describes the last real decision about an image and is what the owner is
    // being shown.
    State             state()        const { return _state; }
    uint32_t          bytesWritten() const { return _received; }
    uint32_t          imageLen()     const { return _header.imageLen; }
    OtaImage::Verdict verdict()      const { return _verdict; }
    const char*       failReason()   const { return _failReason; }
    const OtaImage::Header& header() const { return _header; }

    // True once the image is verified and the boot partition is set. The owner
    // still has to confirm the restart; nothing here reboots anything.
    bool readyToReboot() const { return _state == READY; }

private:
    void fail(const char* why, OtaImage::Verdict v);

    OtaFlashTarget*   _target     = nullptr;
    State             _state      = IDLE;
    OtaImage::Header  _header;
    // Not per-attempt state. arm(), cancel() and fail() must never reset this:
    // it is configuration, and a trust anchor that quietly reverts partway
    // through a session is a trust anchor nobody can reason about.
    OtaImage::KeyRing _trusted    = OtaImage::firmwareKeys();
    OtaImage::Verdict _verdict    = OtaImage::ACCEPTED;
    uint32_t          _received   = 0;
    uint32_t          _armedAtMs  = 0;
    uint32_t          _windowMs   = 0;
    uint32_t          _lastRxMs   = 0;
    const char*       _failReason = "";
    bool              _runningOnTrial = false;
};

// How long an armed device waits for a host before disarming itself. Long
// enough to walk back to a laptop, short enough that a device left on a desk
// does not sit there accepting firmware.
#define OTA_ARM_WINDOW_MS  120000u

// How long a started transfer may stall before the session gives up and
// aborts the slot. A half-written passive partition is harmless, but leaving
// the session open forever would mean the arming window never really ended.
#define OTA_RX_STALL_MS    30000u
