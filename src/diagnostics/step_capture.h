#pragma once

// ── HexHound - Step detector capture ────────────────────────────
//
// Records the RAW accelerometer stream the step detector is fed, so the detector
// can be calibrated against a real counted walk instead of a synthetic waveform.
//
// Why raw input rather than the detector's own verdict: capturing the input means
// candidate tuning constants can be swept OFFLINE, replaying the same walk
// through the real StepDetector as many times as needed. Capturing only a step
// count would cost one walk per candidate, which is the wrong thing to spend a
// person's afternoon on. One walk, then the fitting is free and repeatable.
//
// Everything here compiles to nothing unless HEXHOUND_STEP_CAPTURE is defined,
// so no shipping image carries it. Build the dedicated env:
//
//     pio run -e waveshare-esp32-s3-lcd-128-stepcap -t upload --upload-port COMxx
//
// Protocol:
//   1. Flash this build. On boot it dumps any capture already in SPIFFS, then
//      leaves it alone.
//   2. Start a roam from the menu, walk a COUNTED number of steps, end the roam.
//      The buffer is held in RAM while walking and written ONCE at roam end, so
//      nothing touches flash mid-walk and the sample timing stays clean.
//   3. Plug in and reset. The CSV comes out over serial between markers.
//
// The written file survives a reset, which matters: on a UART-bridge board like
// this one, opening the serial port resets the chip and would lose a RAM-only
// buffer.

#include <stdint.h>

namespace stepcap {

#if defined(HEXHOUND_STEP_CAPTURE) && HEXHOUND_STEP_CAPTURE

// 20 Hz poll x 4000 samples = 200 s of walking, which is comfortably more than
// the ~90 s a 100-step count takes. 8 bytes per sample, so 32 KB of RAM.
constexpr uint16_t CAPACITY = 4000;

void begin();                                   // roam started, arm the buffer
void record(uint32_t nowMs, int16_t ax, int16_t ay, int16_t az);
void save(uint32_t detectedSteps);              // roam ended, persist once
void dumpIfPresent();                           // boot: print any stored capture

#else

// Compiled out entirely. Inline no-ops so call sites need no guards of their own.
inline void begin() {}
inline void record(uint32_t, int16_t, int16_t, int16_t) {}
inline void save(uint32_t) {}
inline void dumpIfPresent() {}

#endif

}  // namespace stepcap
