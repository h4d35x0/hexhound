#include "roam_module.h"
#include "../diagnostics/step_capture.h"

#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

#include <string.h>

#include "wifi_module.h"
#include "ble_module.h"
#include "imu_module.h"
#include "battery_module.h"
#include "../pet/pet_core.h"
#include "../pet/pet_inventory.h"
#include "../content/material_yield.h"

// ── HexHound - Roam Mode Implementation ─────────────────────────
//
// One state machine, two earners, and nothing that blocks. See roam_module.h
// for why the two earners never touch each other.

namespace {

// BLE results are drained in small batches. MAX_BLE_DEVICES is 32 and a
// BLEResult is around 48 bytes, so asking for all of them at once would put
// 1.5 KB on the main loop's stack for the duration of a poll. Eight at a time
// costs a few hundred bytes and loops.
constexpr int BLE_DRAIN_BATCH = 8;

} // namespace

RoamModule& RoamModule::instance() {
    static RoamModule mod;
    return mod;
}

void RoamModule::init() {
    _phase = PHASE_IDLE;
    _active = false;
    _report = RoamReport();
    _stepDetector.reset();
    memset(_prevFp, 0, sizeof(_prevFp));
    memset(_prevN, 0, sizeof(_prevN));
}

// ── Expedition length ─────────────────────────────────────────────────────

bool RoamModule::batteryBounded() const {
    // Three things must all be true before a charge reading is allowed to
    // bound anything: the board profile says there is a gauge, the module
    // agrees it came up, and it has actually taken a sample.
    //
    // The last one matters more than it looks. BatteryModule::percent()
    // returns 0 until the first reading lands, and 0 is indistinguishable
    // from flat. Without hasReading() every roam started in the first two
    // seconds after boot would be refused for a low battery that was never
    // measured.
    if (!Caps::has(CAP_BATTERY)) return false;
    const BatteryModule& bat = BatteryModule::instance();
    return bat.isAvailable() && bat.hasReading();
}

void RoamModule::computePlan() {
    if (!batteryBounded()) {
        // No gauge, no ceiling. Guessing a duration for hardware that cannot
        // measure its own charge would be a fabricated number on a screen, and
        // the owner ending the roam is a perfectly good stopping rule.
        _plannedMs = 0;
        return;
    }

    int pct = BatteryModule::instance().percent();
    int usable = pct - roamtune::ROAM_BATTERY_FLOOR_PCT;
    if (usable <= 0) {
        _plannedMs = 0;
        return;
    }

    uint32_t ms = (uint32_t)usable * roamtune::ROAM_MS_PER_BATTERY_PCT;
    if (ms > roamtune::ROAM_MAX_DURATION_MS) ms = roamtune::ROAM_MAX_DURATION_MS;
    _plannedMs = ms;
}

// ── Session ───────────────────────────────────────────────────────────────

bool RoamModule::begin(bool includeBle) {
    if (_active) return true;

    if (batteryBounded() &&
        BatteryModule::instance().percent() <= roamtune::ROAM_BATTERY_FLOOR_PCT) {
        _report = RoamReport();
        _report.stepsMeasured = Caps::canMeasureMotion();
        _report.endReason = ROAM_END_REFUSED;
        return false;
    }

    uint32_t now = millis();

    _report = RoamReport();
    _report.stepsMeasured = Caps::canMeasureMotion();
    _stepDetector.reset();

    // Diagnostic builds only; a no-op inline everywhere else.
    stepcap::begin();

    // The previous session's fingerprints are dropped on purpose. The pet may
    // have been carried across town while it was off, so last week's living
    // room is not a fair "previous sample" for this morning's train.
    memset(_prevFp, 0, sizeof(_prevFp));
    memset(_prevN, 0, sizeof(_prevN));
    _curN = 0;

    _includeBle     = includeBle;
    _startedAt      = now;
    _endedAt        = 0;
    _lastSampleAt   = 0;
    _lastStepPollAt = now;
    _sampleIndex    = 0;
    _lastChurnPct   = 0;
    _active         = true;
    _phase          = PHASE_WAIT;

    computePlan();

    // Take the opening sample immediately rather than after the first
    // interval: it is the baseline everything else is compared against, and
    // twenty seconds of a blank screen at the start reads as a broken feature.
    startNextSample(now);
    return true;
}

const RoamReport& RoamModule::end(RoamEnd reason) {
    if (!_active) return _report;

    _active = false;
    _phase  = PHASE_IDLE;
    _endedAt = millis();

    _report.endReason  = (uint8_t)reason;
    _report.durationMs = _endedAt - _startedAt;

    // Steps are read from the detector and ONLY from the detector. On a board
    // without an IMU the detector was never fed, so this is 0, and no other
    // path exists that could put a number here.
    if (Caps::canMeasureMotion()) {
        _report.stepsMeasured = true;
        _report.stepsTaken    = _stepDetector.steps();
    } else {
        _report.stepsMeasured = false;
        _report.stepsTaken    = 0;
    }

    // ── Materials ─────────────────────────────────────────────────────────
    // The report lists exactly what the kit received. It used to list a second,
    // parallel set of four "roam materials" while eight different craftables
    // were what actually arrived, which was harmless only while roams granted
    // nothing at all. See RoamFind.
    _report.findCount = 0;
    {
        MaterialGrant g[MATERIAL_GRANT_MAX];
        const uint8_t gn = matyield::roamYield(_report.pointsEarned,
                                               _report.newToPet,
                                               _report.newBleToPet,
                                               _report.stepsTaken,
                                               _report.stepsMeasured, g);
        Inventory& inv = Inventory::instance();
        inv.applyGrants(g, gn);

        // Fold the grant table into the report, merging repeats: the roam pays
        // copper twice when it has a step count, and two copper rows would read
        // as a bug rather than as a bonus.
        for (uint8_t i = 0; i < gn; i++) {
            const uint8_t id = inv.idFor(g[i].item);
            if (id == ITEM_ID_NONE) continue;
            bool merged = false;
            for (uint8_t j = 0; j < _report.findCount; j++) {
                if (_report.finds[j].material == id) {
                    const uint32_t sum = (uint32_t)_report.finds[j].count + g[i].qty;
                    _report.finds[j].count = (uint16_t)(sum > 0xFFFF ? 0xFFFF : sum);
                    merged = true;
                    break;
                }
            }
            if (merged || _report.findCount >= ROAM_MAX_FINDS) continue;
            _report.finds[_report.findCount].material = id;
            _report.finds[_report.findCount].count    = g[i].qty;
            _report.findCount++;
        }
    }

    // ── Commit ────────────────────────────────────────────────────────────
    PetState& st = PetCore::instance().state();
    st.roamPoints += _report.pointsEarned;
    if (_report.stepsMeasured) {
        st.stepCount += _report.stepsTaken;
    }
    if (st.roamSessions < 0xFFFF) st.roamSessions++;
    st.dirty = true;

    return _report;
}


// ── Loop ──────────────────────────────────────────────────────────────────

void RoamModule::update() {
    if (!_active) return;

    uint32_t now = millis();

    pollSteps(now);

    // Battery-bounded expeditions stop themselves. Unbounded ones never do:
    // that is the whole meaning of _plannedMs == 0.
    if (_plannedMs != 0 && (now - _startedAt) >= _plannedMs) {
        end(ROAM_END_DURATION);
        return;
    }
    if (batteryBounded() &&
        BatteryModule::instance().percent() <= roamtune::ROAM_BATTERY_FLOOR_PCT) {
        end(ROAM_END_BATTERY);
        return;
    }

    switch (_phase) {
        case PHASE_WAIT:
            if ((now - _lastSampleAt) >= roamtune::ROAM_SAMPLE_INTERVAL_MS) {
                startNextSample(now);
            }
            break;

        case PHASE_WIFI:
            if (WiFiModule::instance().pollScan()) {
                harvestWifi();
                _phase = PHASE_WAIT;
            }
            break;

        case PHASE_BLE:
            if (BLEModule::instance().isAsyncScanDone()) {
                harvestBle();
                _phase = PHASE_WAIT;
            }
            break;

        case PHASE_IDLE:
        default:
            break;
    }
}

void RoamModule::pollSteps(uint32_t now) {
    // Compile-time false on every board without an IMU, so the whole body -
    // and the detector with it - is dead-stripped there. That is also the
    // structural guarantee that stepCount cannot move on those boards: there
    // is no other caller of the detector anywhere in the firmware.
    if (!Caps::canMeasureMotion()) return;

    IMUModule& imu = IMUModule::instance();
    if (!imu.isAvailable()) return;
    if ((now - _lastStepPollAt) < roamtune::STEP_POLL_MS) return;
    _lastStepPollAt = now;

    // Reads the sample IMUModule::update() already fetched. No I2C traffic is
    // added by roaming, which matters because the IMU shares its bus.
    //
    // Captured BEFORE update() so the recording is the detector's exact input,
    // which is what makes an offline replay faithful rather than approximate.
    stepcap::record(now, imu.ax(), imu.ay(), imu.az());
    _stepDetector.update(now, imu.ax(), imu.ay(), imu.az());
}

void RoamModule::startNextSample(uint32_t now) {
    _lastSampleAt = now;

    bool wantBle = _includeBle &&
                   ((_sampleIndex % roamtune::ROAM_BLE_EVERY_N) ==
                    (roamtune::ROAM_BLE_EVERY_N - 1));
    _sampleIndex++;

    if (wantBle) {
        if (BLEModule::instance().startAsyncScan()) {
            _phase = PHASE_BLE;
            return;
        }
        // BLE refused (already scanning, or unavailable). Fall through to
        // Wi-Fi rather than losing the slot entirely.
    }

    if (WiFiModule::instance().startScan()) {
        _phase = PHASE_WIFI;
    } else {
        // A scan is already in flight from somewhere else. Wait it out; the
        // next interval will try again. Nothing is lost but one slot.
        _phase = PHASE_WAIT;
    }
}

void RoamModule::harvestWifi() {
    WiFiModule& wifi = WiFiModule::instance();
    PetCore& pet = PetCore::instance();

    _curN = 0;
    uint8_t newToPet = 0;

    int n = wifi.resultCount();
    const WiFiResult* r = wifi.results();
    for (int i = 0; i < n; i++) {
        // Fingerprint the BSSID, not the SSID: two access points sharing an
        // ESSID are two different places, and a campus-wide SSID would
        // otherwise make an entire building look like one room.
        if (_curN < ROAM_FP_MAX) {
            _curFp[_curN++] = roam::fingerprintOf(r[i].bssid);
        }
        // Lifetime novelty comes from the pet's own capture set, which is
        // keyed by SSID. Different key, different question: "have we ever been
        // anywhere with this network" rather than "is this the same radio".
        if (r[i].ssid[0] != '\0' && pet.markWifiSeen(r[i].ssid)) {
            if (newToPet < 0xFF) newToPet++;
        }
    }

    score(BAND_WIFI, newToPet);
}

void RoamModule::harvestBle() {
    BLEModule& ble = BLEModule::instance();
    PetCore& pet = PetCore::instance();

    _curN = 0;
    uint8_t newToPet = 0;

    BLEResult batch[BLE_DRAIN_BATCH];
    int got = 0;
    // Bounded drain: MAX_BLE_DEVICES / BLE_DRAIN_BATCH rounds at most, so a
    // misbehaving scanner cannot hold the main loop.
    for (int round = 0; round < (MAX_BLE_DEVICES / BLE_DRAIN_BATCH) + 1; round++) {
        got = ble.pollNewDevices(batch, BLE_DRAIN_BATCH);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (_curN < ROAM_FP_MAX) {
                _curFp[_curN++] = roam::fingerprintOf(batch[i].addr);
            }
            if (batch[i].addr[0] != '\0' && pet.markBleSeen(batch[i].addr)) {
                if (newToPet < 0xFF) newToPet++;
            }
        }
    }

    _report.newBleToPet = (uint16_t)(_report.newBleToPet + newToPet);
    score(BAND_BLE, newToPet);
}

void RoamModule::score(uint8_t band, uint8_t newToPet) {
    bool hadPrevious = _prevN[band] > 0;

    roam::Churn c = roam::compare(_prevFp[band], _prevN[band], _curFp, _curN);
    uint16_t pts = roam::pointsForSample(c, newToPet, hadPrevious);

    _report.pointsEarned += pts;
    _report.newToPet = (uint16_t)(_report.newToPet + newToPet);
    if (_report.samples < 0xFFFF) _report.samples++;
    _lastChurnPct = c.pct;

    // Fixed-size copy of at most 48 bytes. Not an allocation.
    memcpy(_prevFp[band], _curFp, (size_t)_curN * sizeof(uint16_t));
    _prevN[band] = _curN;
}

// ── Live accessors ────────────────────────────────────────────────────────

uint32_t RoamModule::elapsedMs() const {
    if (_active) return millis() - _startedAt;
    if (_endedAt >= _startedAt) return _endedAt - _startedAt;
    return 0;
}

int RoamModule::progressPct() const {
    if (_plannedMs == 0) return -1;   // unbounded, not "zero progress"
    uint32_t el = elapsedMs();
    if (el >= _plannedMs) return 100;
    return (int)((uint64_t)el * 100u / _plannedMs);
}

const char* RoamModule::statusLine() const {
    switch (_phase) {
        case PHASE_WIFI: return "sniffing air";
        case PHASE_BLE:  return "listening in";
        case PHASE_WAIT: return _report.samples == 0 ? "setting off" : "wandering";
        default:         return "resting";
    }
}
