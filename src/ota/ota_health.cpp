#include "ota_health.h"

#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)
#include <stdio.h>
#else
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#endif

// ── HexHound - Health Check and Rollback Implementation ─────────

const char* OtaHealthPolicy::milestoneName(uint8_t oneBit) {
    switch (oneBit) {
        case OTA_HEALTH_DISPLAY: return "display";
        case OTA_HEALTH_STORAGE: return "storage";
        case OTA_HEALTH_PET:     return "pet";
        case OTA_HEALTH_CRYPTO:  return "crypto";
        case OTA_HEALTH_LOOP:    return "loop";
        default:                 return "unknown";
    }
}

namespace {

OtaHealthPolicy s_policy;
bool s_pending      = false;
bool s_afterRollback = false;
bool s_confirmed    = false;
bool s_rollingBack  = false;
bool s_wdtArmed     = false;

}  // namespace

namespace OtaHealth {

#if defined(SIMULATOR_BUILD) || defined(UNIT_TEST)

// ── Off-target ────────────────────────────────────────────────────────────
//
// There is no otadata, no second app partition and no bootloader here, so
// there is nothing to be pending on and nothing to roll back to. Reporting
// "permanent" is the honest answer rather than a simulated trial window that
// would give false confidence in a path that only exists on hardware.
//
// The decision logic itself is OtaHealthPolicy, which is fully portable and is
// what the native suite exercises.

void begin() { s_pending = false; s_afterRollback = false; }
bool isPending() { return false; }
void report(uint8_t milestone) { s_policy.reached(milestone); }
void update() {}
const char* statusName() { return "permanent"; }
bool bootedAfterRollback() { return false; }

#else

// ── On target ─────────────────────────────────────────────────────────────

void begin() {
    s_pending       = false;
    s_afterRollback = false;
    s_confirmed     = false;
    s_rollingBack   = false;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        return;
    }

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) {
        // No otadata entry for this slot. That is the normal state of a device
        // that has only ever been USB-flashed, and it is not pending.
        return;
    }

    // If the OTHER slot is marked ABORTED, the bootloader rejected an update
    // and fell back to this image. Worth surfacing: otherwise a failed update
    // looks to the owner like nothing happened at all.
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (other != nullptr) {
        esp_ota_img_states_t ost;
        if (esp_ota_get_state_partition(other, &ost) == ESP_OK &&
            ost == ESP_OTA_IMG_ABORTED) {
            s_afterRollback = true;
        }
    }

    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        Serial.printf("[OTA] Running image is permanent (state %d)\n", (int)st);
        return;
    }

    s_pending = true;
    s_policy.begin(millis());
    Serial.println("[OTA] This image is ON TRIAL. It must confirm itself or it "
                   "will be rolled back automatically.");

    // Arm the watchdog for the trial window only.
    //
    // Without this, an image that boots and then hangs forever is never rolled
    // back, because the bootloader only reverts a pending image on the NEXT
    // reboot and a hang never reboots. Converting a hang into a reset is what
    // closes that hole, and it is the difference between "usually recovers"
    // and "always recovers".
    //
    // Disarmed again the moment the image is confirmed, so the ordinary
    // long-running firmware, which has blocking patrol scans, is never subject
    // to it.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdtCfg = {};
    wdtCfg.timeout_ms    = OTA_HEALTH_WDT_S * 1000;
    wdtCfg.idle_core_mask = 0;
    wdtCfg.trigger_panic = true;
    // Already-initialised is fine and expected; reconfigure and carry on.
    if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) {
        esp_task_wdt_init(&wdtCfg);
    }
#else
    esp_task_wdt_init(OTA_HEALTH_WDT_S, true);
#endif
    if (esp_task_wdt_add(nullptr) == ESP_OK) {
        s_wdtArmed = true;
    } else {
        // Not fatal. The deadline path below still produces a deliberate
        // rollback for anything that is merely unhealthy; only a true hard
        // hang would be uncovered, and saying so is better than pretending.
        Serial.println("[OTA] WARNING: could not arm trial watchdog");
    }
}

bool isPending() { return s_pending; }

void report(uint8_t milestone) {
    s_policy.reached(milestone);

    // Feeds the trial watchdog, because this runs during setup() and update()
    // does not. Arming a 30 second watchdog in begin() and then not touching it
    // again until loop() would make the whole of setup() unfed, and a trial
    // boot that merely took a while (a SPIFFS format, say) would reset itself
    // and be rolled back for it. A milestone is proof of forward progress, so
    // it is the right thing to feed on. A real hang between two milestones
    // still trips the watchdog, which is what it is armed for.
    if (s_pending && s_wdtArmed) {
        esp_task_wdt_reset();
    }
}

void update() {
    if (!s_pending) {
        return;
    }

    if (s_wdtArmed) {
        esp_task_wdt_reset();
    }

    switch (s_policy.evaluate(millis())) {
        case OtaHealthPolicy::WAIT:
            return;

        case OtaHealthPolicy::CONFIRM: {
            // Latched before the action, not after, because both actions below
            // can fail and return, and re-deciding on the next loop iteration
            // would retry them forever. One trial per boot, one outcome.
            s_policy.markDecided();
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                Serial.println("[OTA] Health check passed. Update is now permanent.");
                s_pending   = false;
                s_confirmed = true;
                if (s_wdtArmed) {
                    esp_task_wdt_delete(nullptr);
                    s_wdtArmed = false;
                }
            } else {
                // Could not make it permanent. Do NOT keep running as if it
                // had worked: the image is still pending, so the next reboot
                // reverts it, and a device that quietly stayed pending forever
                // would be one power cut away from a surprise downgrade.
                Serial.println("[OTA] Could not confirm image. Rolling back.");
                s_rollingBack = true;
                esp_ota_mark_app_invalid_rollback_and_reboot();
                // Reached only if that could not reboot, which means there is
                // no other image to roll back to, which means the running one
                // is all there is. Say so once and stop. The image stays
                // PENDING, so statusName() keeps reporting "pending", which is
                // the truth: the bootloader will resolve it at the next
                // restart and there is nothing further this code can do.
                Serial.println("[OTA] Rollback unavailable. Staying on this image.");
                s_rollingBack = false;
            }
            return;
        }

        case OtaHealthPolicy::ROLLBACK: {
            const uint8_t missing = s_policy.missingMask();
            s_policy.markDecided();
            Serial.print("[OTA] Health check FAILED, missing:");
            for (uint8_t bit = 1; bit != 0; bit = (uint8_t)(bit << 1)) {
                if (missing & bit) {
                    Serial.print(' ');
                    Serial.print(OtaHealthPolicy::milestoneName(bit));
                }
            }
            Serial.println();
            Serial.println("[OTA] Rolling back to the previous firmware.");

            s_rollingBack = true;
            // Reboots into the previous image and does not return. If it does
            // return, the rollback was not possible, which means there is no
            // other image to go to, which means the running one is all there
            // is. Staying on it beats deliberately restarting into nothing.
            esp_ota_mark_app_invalid_rollback_and_reboot();
            Serial.println("[OTA] Rollback unavailable. Staying on this image.");
            s_rollingBack = false;
            // s_pending deliberately stays true, and this used to clear it.
            // Clearing it made update() return early forever while the trial
            // watchdog was still armed with nothing left to feed it, so the
            // device panicked and reset roughly every OTA_HEALTH_WDT_S seconds
            // for as long as it was switched on. The one-shot latch in the
            // policy is what stops the re-decide now, which leaves this loop
            // feeding the watchdog and the state honestly reported as pending.
            return;
        }
    }
}

const char* statusName() {
    if (s_rollingBack) return "rolling_back";
    if (s_pending)     return "pending";
    if (s_confirmed)   return "confirmed";
    return "permanent";
}

bool bootedAfterRollback() { return s_afterRollback; }

#endif  // SIMULATOR_BUILD || UNIT_TEST

}  // namespace OtaHealth
