#pragma once

#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

class BatteryModule {
public:
    static BatteryModule& instance();

    void init();
    void update();

    bool isAvailable() const { return _available; }
    bool hasReading() const { return _millivolts > 0; }
    uint16_t millivolts() const { return _millivolts; }
    float voltage() const { return _millivolts / 1000.0f; }
    int percent() const;

private:
    BatteryModule() = default;

    bool _available = false;
    uint16_t _millivolts = 0;
    uint32_t _lastSampleAt = 0;
    // Reported percentage, deadbanded. -1 until the first sample lands.
    // Kept separate from _millivolts so ADC noise cannot reach the UI: a
    // gauge that flickers by a point is not just ugly, it makes every screen
    // that folds the battery reading into its "stats changed" test repaint
    // itself continuously.
    int16_t _percent = -1;
};
