#include "battery_module.h"
#include "../config.h"

namespace {

static int clampPercent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

} // namespace

BatteryModule& BatteryModule::instance() {
    static BatteryModule mod;
    return mod;
}

void BatteryModule::init() {
#if defined(SIMULATOR_BUILD)
    _available = false;
#elif defined(HEXHOUND_HAS_BATTERY) && HEXHOUND_HAS_BATTERY
    pinMode(PIN_BATTERY_ADC, INPUT);
    analogReadResolution(12);
    _available = true;
    update();
#else
    _available = false;
#endif
}

void BatteryModule::update() {
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_BATTERY) && HEXHOUND_HAS_BATTERY
    if (!_available) {
        return;
    }

    uint32_t now = millis();
    if (_lastSampleAt != 0 && (now - _lastSampleAt) < 2000) {
        return;
    }
    _lastSampleAt = now;

    uint32_t rawMilliVolts = analogReadMilliVolts(PIN_BATTERY_ADC);
    if (rawMilliVolts == 0) {
        return;
    }

    // The divider ratio belongs to the board, not to this file. See
    // BATTERY_ADC_MULT in board/board_profile.h: this was a hardcoded 3 taken
    // from Waveshare's example and applied to every board, which over-reported
    // the T-Display S3 by 50% and, once percent() clamped, made it report a full
    // battery at every state of charge.
    uint32_t scaledMilliVolts = rawMilliVolts * (uint32_t)BATTERY_ADC_MULT;
    if (scaledMilliVolts > 65535U) {
        scaledMilliVolts = 65535U;
    }

    // Low-pass the raw reading. The ADC behind the divider is noisy, and on a
    // board running from USB with no cell attached the node is barely driven
    // at all, so consecutive samples can swing widely.
    if (_millivolts == 0) {
        _millivolts = (uint16_t)scaledMilliVolts;
    } else {
        _millivolts = (uint16_t)(((uint32_t)_millivolts * 3U + scaledMilliVolts) / 4U);
    }

    static constexpr int kEmptyMv = 3400;
    static constexpr int kFullMv  = 4200;
    int pct = clampPercent(
        (int)((long)(_millivolts - kEmptyMv) * 100L / (kFullMv - kEmptyMv)));

    // Deadband: only move the reported value when it actually moves. Without
    // this a one-point wobble every 2s propagates into the UI's change
    // detection and repaints the whole screen forever.
    if (_percent < 0) {
        _percent = (int16_t)pct;
    } else {
        int delta = pct - _percent;
        if (delta < 0) delta = -delta;
        if (delta >= 2) {
            _percent = (int16_t)pct;
        }
    }
#endif
}

int BatteryModule::percent() const {
    if (_percent < 0) {
        return 0;
    }
    return (int)_percent;
}
