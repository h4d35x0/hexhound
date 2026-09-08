#pragma once

#include "../board/board_profile.h"

#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// ── HexHound - QMI8658 6-axis IMU ───────────────────────────────
//
// Present on the Waveshare ESP32-S3-LCD-1.28 round board, which has no touch
// panel and only the awkwardly-placed BOOT button. Motion becomes the natural
// input for a keychain pet: shake it to get its attention, tilt to page
// through a list.
//
// Compiles to a no-op stub unless the board profile sets HEXHOUND_HAS_IMU, so
// every other board is unaffected.
//
// The module publishes EVENT_SHAKE / EVENT_TILT_LEFT / EVENT_TILT_RIGHT onto
// the event bus and also exposes edge-triggered accessors, which is what
// main.cpp's button state machine consumes.

class IMUModule {
public:
    static IMUModule& instance();

    void init();
    void update();

    bool isAvailable() const { return _available; }

    // Edge-triggered: returns true once per gesture, then clears.
    bool takeShake();
    bool takeTiltLeft();
    bool takeTiltRight();

    // Raw accelerometer in milli-g, for diagnostics.
    int16_t ax() const { return _ax; }
    int16_t ay() const { return _ay; }
    int16_t az() const { return _az; }

private:
    IMUModule() = default;

    bool readRegister(uint8_t reg, uint8_t* data, size_t len);
    bool writeRegister(uint8_t reg, uint8_t value);

    bool _available = false;

    int16_t _ax = 0, _ay = 0, _az = 0;   // milli-g

    uint32_t _lastPollAt   = 0;
    uint32_t _shakeArmedAt = 0;   // cooldown so one shake is not many events
    uint32_t _tiltArmedAt  = 0;

    // True while the board is held past the tilt threshold and has already
    // fired. Cleared only when it returns near level, so one tilt is one
    // event no matter how long it is held.
    bool _tiltLatched      = false;

    bool _shakePending     = false;
    bool _tiltLeftPending  = false;
    bool _tiltRightPending = false;
};
