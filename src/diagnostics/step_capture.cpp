#include "step_capture.h"

#if defined(HEXHOUND_STEP_CAPTURE) && HEXHOUND_STEP_CAPTURE

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

namespace stepcap {
namespace {

// Delta-encoded time so a sample fits in 8 bytes. The poll interval is nominally
// STEP_POLL_MS (50 ms) but the real spacing is what matters for cadence work, so
// it is recorded rather than assumed.
struct Sample {
    uint16_t dtMs;
    int16_t  ax, ay, az;
};

Sample   g_buf[CAPACITY];
uint16_t g_count    = 0;
uint32_t g_lastMs   = 0;
bool     g_armed    = false;
bool     g_truncated = false;

const char* kPath = "/stepcap.csv";

}  // namespace

void begin() {
    g_count = 0;
    g_lastMs = 0;
    g_armed = true;
    g_truncated = false;
    Serial.printf("[STEPCAP] armed, capacity %u samples (~%u s at 20 Hz)\n",
                  (unsigned)CAPACITY, (unsigned)(CAPACITY / 20));
}

void record(uint32_t nowMs, int16_t ax, int16_t ay, int16_t az) {
    if (!g_armed) return;
    if (g_count >= CAPACITY) {
        if (!g_truncated) {
            g_truncated = true;
            // Say so rather than silently keeping the first 200 s: a walk longer
            // than the buffer would otherwise look like a complete capture with a
            // step count that does not match what was counted out loud.
            Serial.println("[STEPCAP] BUFFER FULL - capture truncated, later "
                           "steps are NOT in this file");
        }
        return;
    }

    uint32_t dt = (g_count == 0) ? 0 : (nowMs - g_lastMs);
    if (dt > 65535u) dt = 65535u;
    g_lastMs = nowMs;

    g_buf[g_count].dtMs = (uint16_t)dt;
    g_buf[g_count].ax   = ax;
    g_buf[g_count].ay   = ay;
    g_buf[g_count].az   = az;
    g_count++;
}

void save(uint32_t detectedSteps) {
    if (!g_armed) return;
    g_armed = false;

    if (g_count == 0) {
        Serial.println("[STEPCAP] nothing recorded, not writing");
        return;
    }

    File f = SPIFFS.open(kPath, FILE_WRITE);
    if (!f) {
        Serial.println("[STEPCAP] SPIFFS open FAILED, capture lost");
        return;
    }

    // Header carries everything the offline fitter needs to interpret the rows
    // AND what the on-device detector made of them, so a candidate parameter set
    // can be checked against the shipping behaviour as well as against the
    // counted truth.
    f.printf("# hexhound step capture v1\n");
    f.printf("# samples=%u truncated=%d device_detected_steps=%lu\n",
             (unsigned)g_count, g_truncated ? 1 : 0,
             (unsigned long)detectedSteps);
    f.printf("# columns: t_ms,ax_mg,ay_mg,az_mg\n");

    uint32_t t = 0;
    for (uint16_t i = 0; i < g_count; ++i) {
        t += g_buf[i].dtMs;
        f.printf("%lu,%d,%d,%d\n", (unsigned long)t,
                 (int)g_buf[i].ax, (int)g_buf[i].ay, (int)g_buf[i].az);
    }
    f.close();

    Serial.printf("[STEPCAP] saved %u samples to %s (device counted %lu steps)\n",
                  (unsigned)g_count, kPath, (unsigned long)detectedSteps);
    Serial.println("[STEPCAP] plug in and reset to dump it");
}

void dumpIfPresent() {
    if (!SPIFFS.exists(kPath)) {
        Serial.println("[STEPCAP] no capture stored yet");
        return;
    }
    File f = SPIFFS.open(kPath, FILE_READ);
    if (!f) {
        Serial.println("[STEPCAP] capture exists but will not open");
        return;
    }

    // Markers so the dump can be sliced out of a log mechanically. The file is
    // NOT deleted: a capture is expensive to produce (someone had to go for a
    // walk) and re-reading it must never depend on getting the copy right first
    // time.
    Serial.println("[STEPCAP] BEGIN");
    while (f.available()) {
        Serial.write(f.read());
    }
    f.close();
    Serial.println("[STEPCAP] END");
}

}  // namespace stepcap

#endif  // HEXHOUND_STEP_CAPTURE
