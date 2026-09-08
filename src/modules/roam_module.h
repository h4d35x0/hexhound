#pragma once

#include <stdint.h>
#include "../config.h"
#include "../board/capabilities.h"

// ── HexHound - Roam Mode ─────────────────────────────────────────
//
// A roam is an expedition: you carry the pet somewhere and it tells you what
// it found on the way back. Two earners run during a session, and they are
// kept strictly apart. Conflating them is the one mistake this feature is not
// allowed to make.
//
//   1. EXPLORATION POINTS  ->  PetState::roamPoints  ->  EVERY board.
//      Derived from how much the radio environment CHANGED between samples.
//      This is a heuristic, not a measurement. A bus ride past forty access
//      points earns more of it than a mile walked down an empty lane, and
//      that is acceptable exactly as long as nothing ever claims otherwise.
//      It is therefore NEVER labelled steps, distance, metres, or anything
//      else that asserts a physical quantity - not in the UI, not in a
//      journal entry, not in a field name.
//
//   2. STEPS  ->  PetState::stepCount  ->  boards with an IMU ONLY.
//      A real peak detector over accelerometer magnitude, gated on
//      Caps::canMeasureMotion(). On a board with no IMU, stepCount stays
//      exactly 0 forever. It is never estimated, inferred, or scaled from
//      exploration points. A pet that cannot count steps says nothing about
//      steps at all, which is the honest answer and also the only one that
//      stays true when someone moves their save to a board that can.
//
// Nothing here allocates. The fingerprint buffers, the report and the
// detector state are all fixed-size members of the singleton, because the
// T-Dongle S3 has no PSRAM and a growable structure on a fragmenting heap
// fails somewhere unrelated hours later.

// ══════════════════════════════════════════════════════════════════════════
// TUNING CONSTANTS
//
// EVERY value in this block is a first estimate derived from published
// pedometer literature and bench reasoning, NOT from a human walking with
// this hardware. They REQUIRE ON-HARDWARE CALIBRATION before the step count
// can be trusted as a number a person would recognise. docs/p1w4-wiring.md
// lists which ones to move first and in which direction.
// ══════════════════════════════════════════════════════════════════════════

namespace roamtune {

// ── Step detector ─────────────────────────────────────────────────────────
// Units: milli-g for acceleration, milliseconds for time.

// A human cannot physically step twice inside this window. Two threshold
// crossings closer together than this are the same footfall ringing, a bump,
// or a vibration, and neither of them counts. Raise it if a slow deliberate
// walk double-counts; lower it if a jog under-counts. 250 ms is a 240
// step/min ceiling, which is already a run rather than a walk.
#ifndef HEXHOUND_TUNE_STEP_REFRACTORY_MS
#define HEXHOUND_TUNE_STEP_REFRACTORY_MS 250
#endif
constexpr uint32_t STEP_REFRACTORY_MS   = HEXHOUND_TUNE_STEP_REFRACTORY_MS;
// Gap after which the cadence chain is considered broken and the next peak
// starts a fresh run rather than continuing the old one. This is what makes
// an isolated bump (picking the device up, setting it down) cost nothing.
#ifndef HEXHOUND_TUNE_STEP_MAX_INTERVAL_MS
#define HEXHOUND_TUNE_STEP_MAX_INTERVAL_MS 2000
#endif
constexpr uint32_t STEP_MAX_INTERVAL_MS = HEXHOUND_TUNE_STEP_MAX_INTERVAL_MS;
// The detector threshold is dynamic: it tracks the observed noise floor so a
// pocket, a bag and a hand in a coat all end up with their own baseline. These
// two clamp it. The floor is what stops a still device on a desk counting its
// own ADC noise as walking; the ceiling stops a noisy carry from raising the
// bar so high that real footfalls stop registering.
#ifndef HEXHOUND_TUNE_STEP_THRESHOLD_MIN_MG
#define HEXHOUND_TUNE_STEP_THRESHOLD_MIN_MG 120
#endif
constexpr int32_t STEP_THRESHOLD_MIN_MG = HEXHOUND_TUNE_STEP_THRESHOLD_MIN_MG;
#ifndef HEXHOUND_TUNE_STEP_THRESHOLD_MAX_MG
#define HEXHOUND_TUNE_STEP_THRESHOLD_MAX_MG 900
#endif
constexpr int32_t STEP_THRESHOLD_MAX_MG = HEXHOUND_TUNE_STEP_THRESHOLD_MAX_MG;
// Deviation above which the motion is not walking. A shaken keychain hits
// several g; a footfall through a trouser pocket is a few hundred milli-g.
// Exceeding this arms a lockout instead of counting, so shaking the pet to
// wake it up cannot also farm steps.
#ifndef HEXHOUND_TUNE_STEP_SHAKE_MG
#define HEXHOUND_TUNE_STEP_SHAKE_MG 1400
#endif
constexpr int32_t STEP_SHAKE_MG        = HEXHOUND_TUNE_STEP_SHAKE_MG;
#ifndef HEXHOUND_TUNE_STEP_SHAKE_LOCKOUT_MS
#define HEXHOUND_TUNE_STEP_SHAKE_LOCKOUT_MS 1500
#endif
constexpr uint32_t STEP_SHAKE_LOCKOUT_MS = HEXHOUND_TUNE_STEP_SHAKE_LOCKOUT_MS;
// EMA rates as right-shift divisors. _env tracks the resting magnitude
// (gravity plus sensor bias, whatever orientation the board is in) at 1/16 per
// sample; _noise tracks the mean absolute deviation at 1/32. The noise
// follower is deliberately the slower of the two so a threshold cannot chase
// a single loud sample upward and miss the footfall right behind it.
#ifndef HEXHOUND_TUNE_STEP_ENV_DIV
#define HEXHOUND_TUNE_STEP_ENV_DIV 16
#endif
constexpr int32_t STEP_ENV_DIV   = HEXHOUND_TUNE_STEP_ENV_DIV;
#ifndef HEXHOUND_TUNE_STEP_NOISE_DIV
#define HEXHOUND_TUNE_STEP_NOISE_DIV 32
#endif
constexpr int32_t STEP_NOISE_DIV = HEXHOUND_TUNE_STEP_NOISE_DIV;
// threshold = noise floor * NUM / DEN, then clamped.
//
// 1.5x is the figure usually quoted for a magnitude pedometer, and it is too
// tight here: the noise follower is a mean-ABSOLUTE-deviation, and for a
// smooth oscillation of amplitude A that settles at 2A/pi = 0.64A, so a 1.5x
// threshold sits at 0.96A, a hair under the peak. Real gait is spikier than a
// smooth oscillation and clears it, but only just, and the synthetic sweep in
// test_roam lost a third of a brisk 2.6 Hz walk to exactly that margin.
//
// 9/8 = 1.125x, from the SECOND real counted walk (walk2_fixture.h, 164 steps).
// The first walk could not decide this value at all: its noise floor stayed
// under 96 mg throughout, so STEP_THRESHOLD_MIN_MG clamped the threshold and
// the ratio never applied - 4/4 and 5/4 replay that walk to the same 117. The
// second walk had a louder floor, the ratio decided every crossing, and the
// shipping 1.25x found 153 of 164.
//
// Measured on walk 2, walking phase only:
//     1.375x  148     1.25x  153 (was shipping)     1.1875x  158
//     1.125x  160     1.0625x  159                  1.0x     163
//
// 1.0x scores best and is NOT taken, for the same reason the mg floor was left
// alone after the first walk. Ten sampling stalls cost that capture 2.3 s
// inside the walking phase, and measuring each hole against the local gait
// interval says 3-4 footfalls are not in the file at all. 160 + 3.1 unsampled
// accounts for the counted 164; 163 + 3.1 does not, so 1.0x is buying its extra
// steps by inventing them. A fit is only allowed up to what the data can
// physically contain. 1.125x is the smallest step that reaches that ceiling and
// it still keeps an eighth of the amplitude in hand, where 1.0x - a threshold
// sitting exactly on the mean deviation - keeps none.
//
// This does NOT weaken the still-device case: a device on a desk has a noise
// floor around 5 mg, so its threshold is decided by STEP_THRESHOLD_MIN_MG and
// not by this ratio at all. Both real walks commit zero false positives while
// stationary at 1.125x.
#ifndef HEXHOUND_TUNE_STEP_THRESHOLD_NUM
#define HEXHOUND_TUNE_STEP_THRESHOLD_NUM 9
#endif
constexpr int32_t STEP_THRESHOLD_NUM = HEXHOUND_TUNE_STEP_THRESHOLD_NUM;
#ifndef HEXHOUND_TUNE_STEP_THRESHOLD_DEN
#define HEXHOUND_TUNE_STEP_THRESHOLD_DEN 8
#endif
constexpr int32_t STEP_THRESHOLD_DEN = HEXHOUND_TUNE_STEP_THRESHOLD_DEN;
// Samples discarded while the two EMAs settle. At the module's 50 ms poll this
// is 600 ms, so at most one real step is lost at the start of a session.
#ifndef HEXHOUND_TUNE_STEP_WARMUP_SAMPLES
#define HEXHOUND_TUNE_STEP_WARMUP_SAMPLES 12
#endif
constexpr uint16_t STEP_WARMUP_SAMPLES = HEXHOUND_TUNE_STEP_WARMUP_SAMPLES;
// Peaks that must arrive in cadence before any of them count. Two is the
// minimum that still rejects a single bump. The run's opening peak is paid
// retroactively when the second arrives, so N in-cadence peaks yield N steps.
#ifndef HEXHOUND_TUNE_STEP_CADENCE_MIN
#define HEXHOUND_TUNE_STEP_CADENCE_MIN 3
#endif
constexpr uint8_t STEP_CADENCE_MIN = HEXHOUND_TUNE_STEP_CADENCE_MIN;
// How often the module feeds the detector. Deliberately SLOWER than
// IMUModule's own 40 ms poll so every read is a fresh sample rather than
// occasionally the same one twice. 20 Hz against a 1-3 Hz walking cadence is
// comfortably above Nyquist.
#ifndef HEXHOUND_TUNE_STEP_POLL_MS
#define HEXHOUND_TUNE_STEP_POLL_MS 50
#endif
constexpr uint32_t STEP_POLL_MS = HEXHOUND_TUNE_STEP_POLL_MS;
// ── Exploration points ────────────────────────────────────────────────────

// Seconds between radio samples. Every sample costs a scan, and a scan costs
// battery, so this is the main power dial for the whole feature.
constexpr uint32_t ROAM_SAMPLE_INTERVAL_MS = 20000;

// One sample in every N listens on BLE instead of Wi-Fi. Sequential on
// purpose: the round board has 2 MB of quad PSRAM and running a Wi-Fi scan
// and a NimBLE scan at the same time is not a coexistence problem worth
// inviting into a background feature.
constexpr uint8_t  ROAM_BLE_EVERY_N = 3;

// Fraction of the union of two consecutive samples that must have changed
// before any churn is credited. THIS IS THE "same room for an hour" DEFENCE:
// a stationary device in a busy building still watches two or three APs at
// the edge of its sensitivity flicker in and out forever, and without a floor
// that flicker is a points fountain for a device that has not moved an inch.
constexpr uint8_t  ROAM_CHURN_FLOOR_PCT = 25;

// Per-sample caps. A train platform can churn thirty identifiers in one step
// of the cycle; that is a genuinely interesting place, but it should not be
// worth ten minutes of walking in one hit.
constexpr uint8_t  ROAM_CHURN_CAP = 12;
constexpr uint8_t  ROAM_NEW_CAP   = 8;

constexpr uint16_t ROAM_POINTS_PER_CHANGE   = 2;
constexpr uint16_t ROAM_POINTS_NEW_TO_PET   = 5;
constexpr uint16_t ROAM_POINTS_CAP_PER_SAMPLE = 60;

// ── Expedition length ─────────────────────────────────────────────────────
// Only consulted on boards that can actually measure charge. A board that
// cannot is left UNBOUNDED rather than given a made-up ceiling: inventing
// "about forty minutes of battery" for hardware with no fuel gauge is a guess
// presented as a fact, and the user ending the roam themselves is a perfectly
// good stopping rule.
constexpr int      ROAM_BATTERY_FLOOR_PCT   = 15;
constexpr uint32_t ROAM_MS_PER_BATTERY_PCT  = 60000;      // 1 min per point
constexpr uint32_t ROAM_MAX_DURATION_MS     = 45UL * 60UL * 1000UL;

// ── Materials ─────────────────────────────────────────────────────────────
// Deterministic conversion rates, no RNG: what an expedition yields should be
// explainable from what it did.
constexpr uint32_t ROAM_POINTS_PER_DUST    = 25;
constexpr uint16_t ROAM_NEW_PER_GLASS      = 4;
constexpr uint16_t ROAM_BLE_NEW_PER_SHARD  = 3;
constexpr uint32_t ROAM_STEPS_PER_ALLOY    = 500;

} // namespace roamtune

// Identifiers retained per radio sample. MAX_SSIDS is 32, but the far tail of
// a scan is the weak-signal noise that a fingerprint should not be chasing
// anyway, and 24 keeps the three buffers under 150 bytes.
#define ROAM_FP_MAX      24
#define ROAM_MAX_FINDS   6

// ══════════════════════════════════════════════════════════════════════════
// Step detector
// ══════════════════════════════════════════════════════════════════════════
//
// Pure and header-only: no Arduino, no I2C, no clock of its own. Every input
// arrives as an argument, which is the whole point - a unit test can drive it
// with a synthetic accelerometer stream (a walking waveform, a still device, a
// shaken device, a device carried at an angle) and assert the counts. Nobody
// can validate a pedometer without walking, so at minimum it must be possible
// to prove it does not count things that are obviously not steps.

// Integer square root, bit-by-bit. Deterministic, allocation-free, and bounded
// at 16 iterations, so it behaves identically on the ESP32 and in a test.
inline uint32_t roamIsqrt(uint32_t n) {
    uint32_t op = n;
    uint32_t res = 0;
    uint32_t one = 1UL << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res += one << 1;
        }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

class StepDetector {
public:
    void reset() {
        _env = 0; _noise = 0; _lastDev = 0; _lastThr = 0;
        _samples = 0; _steps = 0; _run = 0;
        _above = false; _haveCandidate = false; _haveEdge = false;
        _shakeArmed = false;
        _lastCandidateMs = 0; _lastEdgeMs = 0; _shakeUntilMs = 0;
    }

    // Feed one accelerometer sample, in milli-g, with its own timestamp.
    // Returns how many steps THIS sample committed: 0, or 1, or
    // STEP_CADENCE_MIN when a run first qualifies and pays for its opener.
    uint8_t update(uint32_t nowMs, int16_t ax, int16_t ay, int16_t az) {
        // Magnitude is orientation invariant, which is what lets the same
        // detector work for a board upright on a lanyard and one lying at an
        // angle in a pocket. No axis is privileged.
        uint32_t sq = (uint32_t)((int32_t)ax * (int32_t)ax) +
                      (uint32_t)((int32_t)ay * (int32_t)ay) +
                      (uint32_t)((int32_t)az * (int32_t)az);
        int32_t mag = (int32_t)roamIsqrt(sq);

        if (_samples == 0) {
            // Seed the baseline from the first reading rather than from zero,
            // so the settling transient is not itself a 1 g "peak".
            _env = mag;
            _noise = 0;
        }
        if (_samples < 0xFFFF) _samples++;

        int32_t dev = mag - _env;
        _env += dev / roamtune::STEP_ENV_DIV;

        int32_t adev = dev < 0 ? -dev : dev;
        _noise += (adev - _noise) / roamtune::STEP_NOISE_DIV;

        int32_t thr = _noise * roamtune::STEP_THRESHOLD_NUM /
                      roamtune::STEP_THRESHOLD_DEN;
        if (thr < roamtune::STEP_THRESHOLD_MIN_MG) thr = roamtune::STEP_THRESHOLD_MIN_MG;
        if (thr > roamtune::STEP_THRESHOLD_MAX_MG) thr = roamtune::STEP_THRESHOLD_MAX_MG;

        _lastDev = dev;
        _lastThr = thr;

        // Shaking is not walking. Arm a lockout and drop the cadence chain, so
        // a long shake never accumulates and the peak that ends it does not
        // chain onto whatever was happening before it.
        if (adev > roamtune::STEP_SHAKE_MG) {
            _shakeArmed = true;
            _shakeUntilMs = nowMs + roamtune::STEP_SHAKE_LOCKOUT_MS;
            _above = dev > thr;
            _run = 0;
            _haveCandidate = false;
            _haveEdge = false;
            return 0;
        }

        // Rising edge only. A single footfall holds the signal above the
        // threshold for several consecutive samples; counting the crossing
        // rather than the level means the waveform's width does not matter.
        bool above = dev > thr;
        bool rising = above && !_above;
        _above = above;

        if (_samples <= roamtune::STEP_WARMUP_SAMPLES) return 0;
        if (_shakeArmed) {
            if ((int32_t)(nowMs - _shakeUntilMs) < 0) return 0;
            _shakeArmed = false;
        }
        if (!rising) return 0;

        // The refractory rule is applied against the previous RISING EDGE, not
        // against the previous accepted candidate, and the edge clock advances
        // even when the edge is rejected.
        //
        // Measuring from the last accepted candidate instead is the obvious
        // implementation and it has a hole: a 5 Hz vibration (a rough car
        // ride, a hand drumming on a desk) puts an edge every 200 ms, every
        // other one clears a 250 ms window measured from the last ACCEPTED
        // one, and the thing gets counted at a perfectly plausible 2.5 steps
        // per second forever. Rejecting any edge that follows another edge too
        // closely closes that alias, and it still keeps real footfalls: a
        // footfall's own ringing pushes the edge clock forward by 60 ms or so,
        // which the next footfall 500 ms later clears easily.
        uint32_t edgeGap = _haveEdge ? (nowMs - _lastEdgeMs) : 0xFFFFFFFFu;
        _lastEdgeMs = nowMs;
        _haveEdge   = true;
        if (edgeGap < roamtune::STEP_REFRACTORY_MS) return 0;

        if (!_haveCandidate) {
            _haveCandidate = true;
            _lastCandidateMs = nowMs;
            _run = 1;
            return 0;
        }

        uint32_t dt = nowMs - _lastCandidateMs;
        _lastCandidateMs = nowMs;
        if (dt > roamtune::STEP_MAX_INTERVAL_MS) {
            _run = 1;              // chain broken: this opens a new run
            return 0;
        }

        // Saturating, not wrapping. A run of 256 in-cadence footfalls is a
        // four-minute walk, which is the normal case, and a wrap back through
        // STEP_CADENCE_MIN would silently pay the opener bonus a second time.
        if (_run < 0xFF) _run++;
        uint8_t committed = 0;
        if (_run == roamtune::STEP_CADENCE_MIN) {
            committed = roamtune::STEP_CADENCE_MIN;   // pays for the opener too
        } else if (_run > roamtune::STEP_CADENCE_MIN) {
            committed = 1;
        }
        _steps += committed;
        return committed;
    }

    uint32_t steps() const     { return _steps; }
    uint16_t samples() const   { return _samples; }
    int32_t  threshold() const { return _lastThr; }
    int32_t  deviation() const { return _lastDev; }
    int32_t  baseline() const  { return _env; }
    bool     shaking() const   { return _shakeArmed; }

private:
    int32_t  _env = 0;              // EMA of magnitude: gravity + bias
    int32_t  _noise = 0;            // EMA of |deviation|: the noise floor
    int32_t  _lastDev = 0;
    int32_t  _lastThr = 0;
    uint32_t _lastCandidateMs = 0;  // last edge that became a cadence candidate
    uint32_t _lastEdgeMs = 0;       // last threshold crossing, accepted or not
    uint32_t _shakeUntilMs = 0;
    uint32_t _steps = 0;
    uint16_t _samples = 0;
    uint8_t  _run = 0;
    bool     _above = false;
    bool     _haveCandidate = false;
    bool     _haveEdge = false;
    bool     _shakeArmed = false;
};

// ══════════════════════════════════════════════════════════════════════════
// Radio-environment change
// ══════════════════════════════════════════════════════════════════════════

namespace roam {

// 16-bit hash of a radio identifier. A fingerprint only ever has to answer
// "was this one here last time", so storing 24 hashes beats storing 24 BSSID
// strings by an order of magnitude in RAM.
inline uint16_t fingerprintOf(const char* s) {
    uint32_t h = 5381;
    while (s && *s) h = ((h << 5) + h) + (uint8_t)*s++;
    uint16_t v = (uint16_t)(h ^ (h >> 16));
    return v == 0 ? 1 : v;   // 0 is reserved for "empty slot"
}

struct Churn {
    uint8_t appeared = 0;   // in this sample, not in the last
    uint8_t vanished = 0;   // in the last sample, not in this one
    uint8_t matched  = 0;
    uint8_t pct      = 0;   // (appeared + vanished) / union, as a percentage
};

// Pure set comparison of two fingerprints. Linear scan rather than a sort:
// n is at most 24, and a sort would need either a scratch buffer or to mutate
// the caller's array.
inline Churn compare(const uint16_t* prev, uint8_t prevN,
                     const uint16_t* cur, uint8_t curN) {
    Churn c;
    for (uint8_t i = 0; i < curN; i++) {
        bool found = false;
        for (uint8_t j = 0; j < prevN; j++) {
            if (prev[j] == cur[i]) { found = true; break; }
        }
        if (found) c.matched++; else c.appeared++;
    }
    c.vanished = (uint8_t)(prevN > c.matched ? prevN - c.matched : 0);
    uint16_t uni = (uint16_t)curN + prevN - c.matched;
    c.pct = uni ? (uint8_t)(((uint16_t)(c.appeared + c.vanished) * 100u) / uni) : 0;
    return c;
}

// Exploration points a single sample earns.
//
// `newToPet` is how many identifiers in this sample the pet had never seen in
// its entire life, taken from PetState::seenWifi / seenBle. That is the term
// that separates "somewhere new" from "same room for an hour": churn alone
// says you moved, lifetime novelty says you moved somewhere you have not been.
//
// `hadPrevious` is false for the first sample of a band in a session. There is
// nothing to compare against yet, so it earns no churn - otherwise every roam
// would open with a free jackpot.
inline uint16_t pointsForSample(const Churn& c, uint8_t newToPet,
                                bool hadPrevious) {
    uint16_t pts = 0;
    if (hadPrevious && c.pct >= roamtune::ROAM_CHURN_FLOOR_PCT) {
        uint16_t changed = (uint16_t)c.appeared + c.vanished;
        if (changed > roamtune::ROAM_CHURN_CAP) changed = roamtune::ROAM_CHURN_CAP;
        pts = (uint16_t)(pts + changed * roamtune::ROAM_POINTS_PER_CHANGE);
    }
    uint16_t nw = newToPet;
    if (nw > roamtune::ROAM_NEW_CAP) nw = roamtune::ROAM_NEW_CAP;
    pts = (uint16_t)(pts + nw * roamtune::ROAM_POINTS_NEW_TO_PET);
    if (pts > roamtune::ROAM_POINTS_CAP_PER_SAMPLE) {
        pts = roamtune::ROAM_POINTS_CAP_PER_SAMPLE;
    }
    return pts;
}

} // namespace roam

// ══════════════════════════════════════════════════════════════════════════
// Expedition result
// ══════════════════════════════════════════════════════════════════════════

// Materials an expedition can bring back.
//
// These ids are ROAM-LOCAL and are deliberately NOT inventory item ids. The
// inventory work is happening in parallel and this module does not include its
// header, so depending on its numbering would be depending on something that
// does not exist yet. The integrator owns one translation table from these
// six values onto real item ids; see docs/p1w4-wiring.md.
enum RoamMaterial : uint8_t {
    ROAM_MAT_NONE = 0,
    ROAM_MAT_SIGNAL_DUST,     // bulk yield, from exploration points
    ROAM_MAT_DRIFT_GLASS,     // from identifiers the pet had never seen
    ROAM_MAT_BEACON_SHARD,    // from BLE identifiers the pet had never seen
    ROAM_MAT_PACED_ALLOY,     // from REAL steps: IMU boards only, by design
    ROAM_MAT_COUNT
};

inline const char* roamMaterialName(uint8_t id) {
    switch (id) {
        case ROAM_MAT_SIGNAL_DUST:  return "Signal Dust";
        case ROAM_MAT_DRIFT_GLASS:  return "Drift Glass";
        case ROAM_MAT_BEACON_SHARD: return "Beacon Shard";
        case ROAM_MAT_PACED_ALLOY:  return "Paced Alloy";
        default:                    return "?";
    }
}

// Short forms for the 160x80 panel, where a row is 26 characters total.
inline const char* roamMaterialShort(uint8_t id) {
    switch (id) {
        case ROAM_MAT_SIGNAL_DUST:  return "Dust";
        case ROAM_MAT_DRIFT_GLASS:  return "Glass";
        case ROAM_MAT_BEACON_SHARD: return "Shard";
        case ROAM_MAT_PACED_ALLOY:  return "Alloy";
        default:                    return "?";
    }
}

struct RoamFind {
    // An ITEM id from ITEM_DEFS - the same thing that went into the kit.
    //
    // This used to be a RoamMaterial from the enum below, which meant the
    // report named four things ("Signal Dust", "Drift Glass"...) while an
    // entirely different set of eight materials was what you actually received.
    // Once roams started granting anything at all, that second vocabulary
    // stopped being flavour and became a report that lied about its own
    // outcome. One list now, and it is the one the kit received.
    uint8_t  material = 0xFF;            // ITEM_ID_NONE
    uint16_t count    = 0;
};

enum RoamEnd : uint8_t {
    ROAM_END_NONE = 0,
    ROAM_END_USER,        // the button was held on the roam screen
    ROAM_END_DURATION,    // the battery-derived budget ran out
    ROAM_END_BATTERY,     // charge fell to the floor mid-expedition
    ROAM_END_REFUSED      // never started: too little charge
};

// Why an expedition ended, in the owner's words.
//
// This exists because the field could not answer it. An expedition ended in a
// trouser pocket and was found sitting on its report with a count well short of
// the walk, and there was no way to tell a button pressed by a battery lead from
// a charge floor from a spent time budget - the report did not say, nothing was
// journalled, and by then the state that would have decided it was gone. The
// value was being set carefully by four paths and read by none.
//
// ROAM_END_USER is deliberately NOT worded as "you stopped it". The button being
// held is the only thing the firmware observes, and in a pocket that is not the
// same statement as the owner deciding to stop.
inline const char* roamEndReasonName(uint8_t reason) {
    switch (reason) {
        case ROAM_END_USER:     return "button held";
        case ROAM_END_DURATION: return "time budget";
        case ROAM_END_BATTERY:  return "battery low";
        case ROAM_END_REFUSED:  return "no charge";
        default:                return "ended";
    }
}

struct RoamReport {
    uint32_t pointsEarned = 0;   // exploration. NEVER a distance.
    uint32_t stepsTaken   = 0;   // real steps, 0 unless stepsMeasured
    bool     stepsMeasured = false;  // false => this board CANNOT count steps
    uint16_t samples      = 0;
    uint16_t newToPet     = 0;   // identifiers never seen before, any band
    uint16_t newBleToPet  = 0;
    uint32_t durationMs   = 0;
    uint8_t  endReason    = ROAM_END_NONE;
    uint8_t  findCount    = 0;
    RoamFind finds[ROAM_MAX_FINDS];
};

// ══════════════════════════════════════════════════════════════════════════
// The module
// ══════════════════════════════════════════════════════════════════════════

class RoamModule {
public:
    static RoamModule& instance();

    void init();

    // Start an expedition. Returns false and leaves the module idle when the
    // board can measure charge and there is not enough of it; report()
    // .endReason is then ROAM_END_REFUSED.
    //
    // includeBle is the caller's decision, exactly as with patrol: BLE is
    // stage-gated content, and this module does not reach into the stage rules
    // to second-guess it.
    bool begin(bool includeBle = false);

    // Non-blocking. Call from the main loop. Must run AFTER
    // IMUModule::update(), whose latest sample it reads; it never touches the
    // I2C bus itself, so it adds no traffic.
    void update();

    // Finalise, convert to materials, commit to PetState, and return the
    // report. Safe to call on an idle module; returns the last report.
    const RoamReport& end(RoamEnd reason = ROAM_END_USER);

    bool active() const { return _active; }

    // ── Live accessors for SCREEN_ROAM ────────────────────────────────────
    uint32_t points() const  { return _report.pointsEarned; }
    uint32_t steps() const   { return Caps::canMeasureMotion() ? _stepDetector.steps() : 0; }
    // Whether this board can count steps at all. A screen must use this to
    // decide whether to show a step row, NOT `steps() > 0` - a board with an
    // IMU that has not moved yet is a different statement from a board that
    // will never know.
    static bool stepsMeasured() { return Caps::canMeasureMotion(); }
    uint16_t sampleCount() const { return _report.samples; }
    uint32_t elapsedMs() const;
    // 0 means unbounded: this board cannot measure charge, so no ceiling was
    // invented for it.
    uint32_t plannedMs() const { return _plannedMs; }
    // -1 when unbounded, so a progress widget can tell "no progress yet" from
    // "no such thing as progress here".
    int      progressPct() const;
    const char* statusLine() const;
    uint8_t  lastChurnPct() const { return _lastChurnPct; }

    const RoamReport& report() const { return _report; }

    // Exposed for the wiring doc's diagnostics, and so a test harness can
    // drive the detector through the module without a real IMU.
    const StepDetector& detector() const { return _stepDetector; }

private:
    RoamModule() = default;

    enum Phase : uint8_t { PHASE_IDLE, PHASE_WAIT, PHASE_WIFI, PHASE_BLE };
    enum Band  : uint8_t { BAND_WIFI = 0, BAND_BLE = 1, BAND_COUNT };

    void startNextSample(uint32_t now);
    void harvestWifi();
    void harvestBle();
    void score(uint8_t band, uint8_t newToPet);
    void pollSteps(uint32_t now);
    bool batteryBounded() const;
    void computePlan();

    StepDetector _stepDetector;
    RoamReport   _report;

    uint16_t _prevFp[BAND_COUNT][ROAM_FP_MAX] = {};
    uint8_t  _prevN[BAND_COUNT] = {};
    uint16_t _curFp[ROAM_FP_MAX] = {};
    uint8_t  _curN = 0;

    uint32_t _startedAt      = 0;
    uint32_t _endedAt        = 0;
    uint32_t _lastSampleAt   = 0;
    uint32_t _lastStepPollAt = 0;
    uint32_t _plannedMs      = 0;    // 0 = unbounded
    uint16_t _sampleIndex    = 0;
    uint8_t  _lastChurnPct   = 0;
    Phase    _phase          = PHASE_IDLE;
    bool     _active         = false;
    bool     _includeBle     = false;
};
