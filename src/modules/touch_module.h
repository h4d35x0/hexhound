#pragma once

#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// For HEXHOUND_HAS_TOUCH. Needed in the HEADER, not just the .cpp: the read
// counters below have to be compiled out on a board with no touch panel, or
// they cost 16 bytes of bss on all six of them - measured, and exactly the trap
// this project has now hit four times.
#include "../board/board_profile.h"

struct TouchPoint {
    uint16_t x = 0;
    uint16_t y = 0;
};

// Two controllers are in the fleet and they disagree about both their address
// and their touch-data layout, so the chip is detected at init rather than
// assumed from the board macro.
enum TouchChip : uint8_t {
    TOUCH_CHIP_NONE = 0,
    TOUCH_CHIP_AXS5106L,   // 0x63 - Waveshare ESP32-S3-Touch-LCD-1.47
    TOUCH_CHIP_FT3267      // 0x38 - LilyGo T-RGB
};

class TouchModule {
public:
    static TouchModule& instance();

    void init();
    void update();
    void setDisplayRotation(uint8_t rotation, uint16_t width, uint16_t height);

    bool isAvailable() const { return _available; }
    bool isPressed() const { return _pressed; }
    const TouchPoint& point() const { return _point; }
    TouchChip chip() const { return _chip; }

private:
    TouchModule() = default;

    // One I2C register read, no retry. Kept separate so readRegister() can
    // count how often the FIRST attempt fails, which is the health of the bus
    // rather than the health of the driver.
    bool readRegisterOnce(uint8_t reg, uint8_t* data, size_t len);
    bool readRegister(uint8_t reg, uint8_t* data, size_t len);
    bool probe();
    void applyRotation(uint16_t rawX, uint16_t rawY, TouchPoint& out) const;

    TouchChip _chip = TOUCH_CHIP_NONE;
    uint8_t _addr = 0;
    bool _available = false;
    bool _pressed = false;
    TouchPoint _point;
    uint8_t _rotation = 1;
    uint16_t _width = 0;
    uint16_t _height = 0;
    uint32_t _lastPollAt = 0;

#if defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    // ── Read health ───────────────────────────────────────────────────────
    // Counted rather than inferred. The failure rate was previously worked out
    // by dividing an error count from the serial log by a poll interval read
    // out of this file, which is a rate with an invented denominator. These
    // give the real one.
    uint32_t _reads      = 0;   // register reads attempted
    uint32_t _firstFail  = 0;   // first attempt failed (the bus's health)
    uint32_t _hardFail   = 0;   // still failed after the retry (what the app sees)
    uint32_t _lastStatAt = 0;
#endif
};
