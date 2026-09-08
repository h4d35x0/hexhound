// ── HexHound - offline step detector replay ─────────────────────
//
// Feeds a captured accelerometer walk through the REAL StepDetector and prints
// how many steps it counts. Built natively, once per candidate parameter set, by
// scripts/fit_step_detector.py.
//
// It includes the shipping header rather than reimplementing the algorithm. A
// Python or C++ copy of the detector would be a second implementation free to
// drift from the firmware, and then a "calibration" would be tuning the copy.
// Every roamtune step constant is overridable with -DHEXHOUND_TUNE_<NAME>=<v>,
// which is the only reason this file can sweep anything.
//
// Usage:
//     step_replay <capture.csv> [--steps]
// Prints:  steps=<n> samples=<n> span_ms=<n>
//
// --steps additionally prints one "STEP <t_ms> <n>" line per committed step. That
// is what makes it possible to ask WHERE the errors are rather than only how many
// there were: a total can be right because an undercount while walking cancelled
// false positives while sitting still, and that coincidence is worse than an
// honest miscount because it looks like success.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// The detector is header-only and already compiles for the native test env, so
// nothing else from the firmware is needed here.
#include "../src/modules/roam_module.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: step_replay <capture.csv>\n");
        return 2;
    }
    bool showSteps = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--steps") == 0) showSteps = true;
    }

    std::FILE* f = std::fopen(argv[1], "r");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    StepDetector det;
    det.reset();

    char line[256];
    unsigned long steps = 0;
    unsigned long samples = 0;
    unsigned long firstT = 0, lastT = 0;
    bool haveFirst = false;

    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned long t = 0;
        int ax = 0, ay = 0, az = 0;
        if (std::sscanf(line, "%lu,%d,%d,%d", &t, &ax, &ay, &az) != 4) continue;

        if (!haveFirst) { firstT = t; haveFirst = true; }
        lastT = t;

        // update() returns how many steps THIS sample committed, which is how the
        // firmware accumulates them; summing the return value rather than reading
        // an internal counter keeps this honest about the retroactive opener that
        // STEP_CADENCE_MIN pays for.
        const uint8_t committed =
            det.update((uint32_t)t, (int16_t)ax, (int16_t)ay, (int16_t)az);
        if (committed && showSteps) {
            // A run's opening peak is paid retroactively when the second arrives,
            // so one sample can commit STEP_CADENCE_MIN steps at once. Print the
            // count so that is visible rather than looking like a lost step.
            std::printf("STEP %lu %u\n", t, (unsigned)committed);
        }
        steps += committed;
        samples++;
    }
    std::fclose(f);

    std::printf("steps=%lu samples=%lu span_ms=%lu detector_steps=%lu\n",
                steps, samples, haveFirst ? (lastT - firstT) : 0UL,
                (unsigned long)det.steps());
    return 0;
}
