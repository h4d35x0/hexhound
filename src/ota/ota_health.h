#pragma once

#include <stdint.h>

// ── HexHound - Post-Update Health Check and Rollback ────────────
//
// The other half of the never-brick guarantee. ota_session.cpp makes sure a
// bad image is never INSTALLED. This file makes sure an image that installed
// fine but does not actually work does not become permanent.
//
// ── How ESP-IDF rollback actually behaves ─────────────────────────────────
//
// This is worth stating precisely because the mental model most people have is
// wrong, and the wrong model produces code that looks correct and protects
// nothing.
//
//   1. esp_ota_set_boot_partition() marks the new slot ESP_OTA_IMG_NEW.
//   2. The bootloader, at the next boot, promotes it to PENDING_VERIFY and
//      boots it. This requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, which
//      IS set in the Arduino ESP32-S3 SDK this project builds against. Without
//      it, every call below is a no-op and there is no rollback at all, so
//      that config is a load-bearing dependency and not an incidental one.
//   3. While PENDING_VERIFY, the app must call
//      esp_ota_mark_app_valid_cancel_rollback(). If it does, the image becomes
//      VALID and permanent.
//   4. If the device reboots while still PENDING_VERIFY, for ANY reason, the
//      bootloader marks that slot ABORTED and boots the previous one instead.
//
// ── The trap in step 4 ────────────────────────────────────────────────────
//
// Rollback is triggered by a REBOOT, not by unhappiness. A new image that
// crashes, panics or watchdogs gets rolled back automatically because those
// all end in a reset. A new image that boots and then WEDGES FOREVER, without
// resetting, is never rolled back. It sits there, pending, bricked in every
// way that matters to the person holding it.
//
// So a hang has to be converted into a reset, and that is what the task
// watchdog below is for. It is armed ONLY during the pending-verify window and
// only on the loop task, and its timeout is set comfortably above the health
// deadline so the ordinary path is a clean, deliberate rollback and the
// watchdog is strictly the backstop for a true hang.
//
// ── What feeds it, and why that is report() and not only update() ─────────
//
// The watchdog is armed in begin(), which runs early in setup(), and update()
// is not called until loop(). Feeding it only from update() would leave the
// whole of setup() as unfed watchdog time, and setup() on this device is long:
// panel init, a SPIFFS mount that may FORMAT on a fresh or damaged filesystem,
// the pet save load and migration, identity derivation, and several UI
// subsystems. A trial boot that is merely slow would be reset by its own
// safety net, and the bootloader would then roll back a firmware that was
// perfectly fine. That fails in the safe direction, so it is not a brick, but
// it presents as "updates randomly refuse to stick" and would be blamed on the
// signature path for a week.
//
// So report() feeds the watchdog too. A milestone is exactly the evidence of
// forward progress a watchdog wants, so the call that already means "setup is
// getting somewhere" is the call that keeps the trial alive, and there is no
// esp_task_wdt_reset() sprinkled through main.cpp to forget or to mis-place.
//
// The consequence, which is the intended behaviour and not a gap: the timeout
// now has to exceed the longest gap BETWEEN two milestones rather than the
// length of the whole of setup(). A genuine hang between two of them still
// trips it, which is the entire point of arming it.
//
// ── Why the health check is not just "we booted" ──────────────────────────
//
// Because this product has already shipped a build that ran perfectly with
// nothing on the screen. The HID firmware was based on the TFT_eSPI backend
// that black-screens on the T-Dongle S3, and because HID mode has no serial
// console, the failure was completely invisible: the device booted, typed
// missions correctly, and showed a dead panel. See docs/bugfix_log.md.
//
// A liveness-only health check would have confirmed that image as healthy. The
// pet being visible is the entire product, so the display is a required
// milestone, not a nice-to-have.
//
// ── Which way to fail ─────────────────────────────────────────────────────
//
// A health check that is too strict rolls back to the previous firmware, which
// is known to work, because it is the image that was running when the update
// started. The owner keeps a working device and a working pet, and loses only
// the update. A health check that is too lax makes a broken image permanent
// on a device with no recovery path. Those costs are not close, so every
// ambiguous case here resolves toward rollback.

// Milestones the firmware reports as it comes up. A new image must reach all
// of the REQUIRED ones to be confirmed.
enum OtaHealthMilestone : uint8_t {
    OTA_HEALTH_DISPLAY = 1 << 0,  // panel initialised and drawing
    OTA_HEALTH_STORAGE = 1 << 1,  // filesystem mounted
    OTA_HEALTH_PET     = 1 << 2,  // pet state loaded or legitimately absent
    OTA_HEALTH_CRYPTO  = 1 << 3,  // crypto self-tests passed
    OTA_HEALTH_LOOP    = 1 << 4   // setup() returned and loop() is running
};

#define OTA_HEALTH_REQUIRED  (OTA_HEALTH_DISPLAY | OTA_HEALTH_STORAGE | \
                              OTA_HEALTH_PET     | OTA_HEALTH_CRYPTO  | \
                              OTA_HEALTH_LOOP)

// Milliseconds of healthy running before a pending image is confirmed. Not
// zero: an image that reaches every milestone and then immediately crashes
// should not have been confirmed in between. Long enough to cover a reset
// loop, short enough that the owner is not waiting.
#define OTA_HEALTH_SETTLE_MS    8000u

// Milliseconds after boot by which a pending image must have reached every
// required MILESTONE. Past this, an image that still has not is declared
// unhealthy and rolled back deliberately.
//
// Note what this is NOT: it is not a deadline to have confirmed. evaluate()
// tests CONFIRM before ROLLBACK, so an image that reaches all five milestones
// is confirmed even if it got there slowly, and only an image still MISSING one
// at the deadline is rolled back. That ordering is deliberate.
//
// It is the one place in this subsystem where an ambiguous-looking case
// resolves toward CONFIRM rather than ROLLBACK, so it is worth saying why it is
// not actually ambiguous. "Display up, storage mounted, pet loaded, crypto
// passed, loop running" is a working device by every measure this firmware has.
// Rolling that back because boot took longer than expected would cost the owner
// a functioning update and buy no safety at all, and slow boots are real here:
// a SPIFFS format or a save migration can add seconds. The bias toward rollback
// exists for images we cannot vouch for, and this is not one of them.
//
// A genuinely wedged image is not covered by this constant in either direction.
// It never reaches loop(), so nothing ever calls evaluate(); the trial watchdog
// is what catches that, by turning the hang into the reset the bootloader needs
// to see.
#define OTA_HEALTH_DEADLINE_MS  20000u

// Task watchdog timeout for the pending-verify window only. Comfortably above
// the deadline so a normal rollback is the clean path and this only fires on a
// genuine hang. Also comfortably above the longest blocking operation in the
// firmware, the patrol scan loop, so a busy device is never reset for being
// busy.
//
// Because report() feeds it, the bound this has to clear during startup is the
// longest gap between two consecutive milestones, not the total length of
// setup(). The worst of those is the storage milestone, which covers a SPIFFS
// mount that may format the partition.
#define OTA_HEALTH_WDT_S        30

// ── Portable policy ───────────────────────────────────────────────────────
//
// The decision logic, with no ESP dependency, so every branch including the
// deadline can be tested on a desktop instead of by bricking real hardware to
// find out.
class OtaHealthPolicy {
public:
    enum Decision : uint8_t {
        WAIT = 0,   // still in the trial window, or the decision is already made
        CONFIRM,    // healthy, make it permanent
        ROLLBACK    // unhealthy, go back to the previous image
    };

    void begin(uint32_t bootMs) {
        _bootMs    = bootMs;
        _reached   = 0;
        _decided   = false;
    }

    void reached(uint8_t milestone) { _reached = (uint8_t)(_reached | milestone); }

    uint8_t reachedMask() const { return _reached; }
    bool    allRequired() const { return (_reached & OTA_HEALTH_REQUIRED) == OTA_HEALTH_REQUIRED; }

    // ── The one-shot latch ────────────────────────────────────────────────
    //
    // Set by the caller once it has ACTED on a decision. Both actions on the
    // device side normally end the story, by clearing the pending flag or by
    // rebooting, so on the happy path this changes nothing. It exists for the
    // path where the action itself fails and RETURNS: confirming can fail, and
    // the rollback-and-reboot that follows it can also fail when there is no
    // other image to go to. Without the latch, evaluate() answers CONFIRM again
    // on the very next loop iteration and the device spins there forever,
    // retrying a call that has already told it no and printing two lines a pass
    // for as long as it is switched on.
    //
    // Deciding once is also simply the truth of what a trial is. There is one
    // trial per boot and it has one outcome.
    void markDecided() { _decided = true; }
    bool decided() const { return _decided; }

    Decision evaluate(uint32_t nowMs) const {
        if (_decided) {
            return WAIT;
        }

        const uint32_t elapsed = (uint32_t)(nowMs - _bootMs);

        if (allRequired() && elapsed >= OTA_HEALTH_SETTLE_MS) {
            return CONFIRM;
        }
        if (elapsed >= OTA_HEALTH_DEADLINE_MS) {
            // Reached the deadline without every milestone. Something the
            // product needs did not come up, and this image does not get to be
            // permanent on the strength of merely still executing.
            return ROLLBACK;
        }
        return WAIT;
    }

    // Which required milestones are still missing, for the log line and the
    // companion app. Being told the update was rejected is much less useful
    // than being told the display never came up.
    uint8_t missingMask() const {
        return (uint8_t)(OTA_HEALTH_REQUIRED & ~_reached);
    }

    static const char* milestoneName(uint8_t oneBit);

private:
    uint32_t _bootMs  = 0;
    uint8_t  _reached = 0;
    bool     _decided = false;
};

// ── Device side ───────────────────────────────────────────────────────────
//
// A no-op on the simulator and in tests, where there is no otadata and nothing
// to roll back to. The policy above is what gets tested; this is the thin
// layer that talks to esp_ota_*.
namespace OtaHealth {

// Call early in setup(). Reads whether this boot is a trial run and, if it is,
// arms the watchdog for the trial window.
void begin();

// True when the running image is on trial and has not yet been confirmed.
bool isPending();

// Report a milestone as the firmware comes up. Harmless when not pending.
//
// Also feeds the trial watchdog while the image is pending, which is what
// stops a slow but healthy setup() from being reset as though it had hung.
// Call it as each subsystem comes up, not in a batch at the end of setup():
// the spacing is what carries the information.
void report(uint8_t milestone);

// Call every loop iteration. Confirms or rolls back once the policy decides.
// Does nothing at all when the running image is already permanent, which is
// the overwhelmingly common case, so this is cheap.
void update();

// Human-readable state for the diagnostics screen and the companion app:
// "permanent", "pending", "confirmed" or "rolling_back".
const char* statusName();

// Was THIS boot the result of an automatic rollback, i.e. did a previous
// update get rejected? Worth telling the owner, because otherwise a failed
// update looks like nothing happened at all.
bool bootedAfterRollback();

}  // namespace OtaHealth
