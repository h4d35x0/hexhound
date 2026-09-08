#include "touch_module.h"
#include "../config.h"

#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
#include <Wire.h>
#endif

namespace {

static constexpr uint8_t AXS5106L_ADDR = 0x63;
static constexpr uint8_t AXS5106L_ID_REG = 0x08;
static constexpr uint8_t AXS5106L_TOUCH_DATA_REG = 0x01;

// FocalTech FT3267 (LilyGo T-RGB). 0xA3 is the chip id; 0x02 is TD_STATUS,
// with the first touch point in the four bytes that follow it.
static constexpr uint8_t FT3267_ADDR = 0x38;
static constexpr uint8_t FT3267_CHIPID_REG = 0xA3;
static constexpr uint8_t FT3267_STATUS_REG = 0x02;

} // namespace

TouchModule& TouchModule::instance() {
    static TouchModule mod;
    return mod;
}

void TouchModule::init() {
#if defined(SIMULATOR_BUILD)
    _available = false;
#elif defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);

#if defined(HEXHOUND_RGB_PANEL)
    // Pin the bus speed rather than inheriting whatever the last caller left.
    //
    // On the T-RGB this is the SECOND Wire.begin() on a bus trgb::begin() has
    // already brought up, and that path deliberately runs at 1 MHz to clock the
    // panel's init table before dropping back to 400 kHz. Leaving the clock to
    // whatever a re-begin() happens to choose makes the speed touch is addressed
    // at depend on init ordering, which is not something that should be left to
    // chance on the one bus the touch controller shares with the panel's
    // expander. FocalTech parts on this board are not reliable above 400 kHz;
    // the panel init comment says so, and it is the reason that path steps back
    // down at all.
    // Scoped to the RGB-panel board on purpose. It is the only one where the
    // panel and the touch controller share a bus, so it is the only one where
    // another subsystem can have moved the clock. Changing the speed on the
    // Waveshare Touch 1.47 - whose AXS5106L owns its bus alone, and which is
    // not hardware-validated - would be an unrelated risk taken for nothing.
    Wire.setClock(400000UL);
#endif

    // PIN_TOUCH_RST < 0 means the reset line is not a GPIO. On T-RGB it is
    // expander pin IO1, and trgb::begin() has already released it.
    if (PIN_TOUCH_RST >= 0) {
        pinMode(PIN_TOUCH_RST, OUTPUT);
        digitalWrite(PIN_TOUCH_RST, LOW);
        delay(20);
        digitalWrite(PIN_TOUCH_RST, HIGH);
        delay(50);
    }

    // A FocalTech part ACKs its address from its bootloader long before its
    // registers read back anything but zero, by roughly 300 ms. Whoever
    // released the reset may have done so moments ago, so poll for readiness
    // rather than assume it, and rather than pay a flat delay when the boot
    // work in between has already covered it.
    const uint32_t deadline = millis() + 400;
    for (;;) {
        _available = probe();
        if (_available || (int32_t)(millis() - deadline) >= 0) {
            break;
        }
        delay(25);
    }

    const char* name = _chip == TOUCH_CHIP_AXS5106L ? "AXS5106L"
                     : _chip == TOUCH_CHIP_FT3267   ? "FT3267"
                                                    : "none";
    Serial.printf("[Touch] %s (0x%02X) %s\n", name, _addr,
                  _available ? "ready" : "unavailable");
#else
    _available = false;
#endif
}

void TouchModule::setDisplayRotation(uint8_t rotation, uint16_t width, uint16_t height) {
    _rotation = rotation & 0x03;
    _width = width;
    _height = height;
}

void TouchModule::update() {
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    if (!_available) {
        return;
    }

    uint32_t now = millis();
    if (_lastPollAt != 0 && (now - _lastPollAt) < 25) {
        return;
    }
    _lastPollAt = now;

    // Health line, every 30 s. Per-mille integer maths on purpose: this runs on
    // the poll path and a float format pulls in a much larger printf.
    if (_lastStatAt == 0) {
        _lastStatAt = now;
    } else if (now - _lastStatAt >= 30000) {
        _lastStatAt = now;
        const uint32_t r = _reads ? _reads : 1;
        Serial.printf("[Touch] reads=%lu firstFail=%lu (%lu.%lu%%) hardFail=%lu (%lu.%lu%%)\n",
                      (unsigned long)_reads,
                      (unsigned long)_firstFail,
                      (unsigned long)(_firstFail * 1000UL / r / 10UL),
                      (unsigned long)(_firstFail * 1000UL / r % 10UL),
                      (unsigned long)_hardFail,
                      (unsigned long)(_hardFail * 1000UL / r / 10UL),
                      (unsigned long)(_hardFail * 1000UL / r % 10UL));
    }

    uint16_t rawX = 0;
    uint16_t rawY = 0;

    if (_chip == TOUCH_CHIP_FT3267) {
        // TD_STATUS, then P1_XH / P1_XL / P1_YH / P1_YL. The high bytes carry
        // the event flag in bits 7:6, so only the low nibble is coordinate.
        uint8_t data[5] = { 0 };
        if (!readRegister(FT3267_STATUS_REG, data, sizeof(data))) {
            _pressed = false;
            return;
        }
        if ((data[0] & 0x0F) == 0) {
            _pressed = false;
            return;
        }
        rawX = (uint16_t)(((data[1] & 0x0F) << 8) | data[2]);
        rawY = (uint16_t)(((data[3] & 0x0F) << 8) | data[4]);
    } else {
        uint8_t data[14] = { 0 };
        if (!readRegister(AXS5106L_TOUCH_DATA_REG, data, sizeof(data))) {
            _pressed = false;
            return;
        }
        if (data[1] == 0) {
            _pressed = false;
            return;
        }
        rawX = (uint16_t)(((data[2] & 0x0F) << 8) | data[3]);
        rawY = (uint16_t)(((data[4] & 0x0F) << 8) | data[5]);
    }

    applyRotation(rawX, rawY, _point);
    _pressed = true;
#endif
}

// Identifies which controller is on the bus. Both are probed by READING a
// register, not merely by an address ACK: an ACK alone does not mean the
// controller has finished booting, which is exactly how the FT3267 first
// presented as a dead chip returning zeros.
bool TouchModule::probe() {
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    uint8_t buf[3] = { 0 };

    _addr = AXS5106L_ADDR;
    if (readRegister(AXS5106L_ID_REG, buf, sizeof(buf))) {
        _chip = TOUCH_CHIP_AXS5106L;
        return true;
    }

    _addr = FT3267_ADDR;
    if (readRegister(FT3267_CHIPID_REG, buf, 1) && buf[0] != 0x00 && buf[0] != 0xFF) {
        _chip = TOUCH_CHIP_FT3267;
        return true;
    }

    _chip = TOUCH_CHIP_NONE;
    _addr = 0;
    return false;
#else
    return false;
#endif
}

// Retries once, and that single retry is what makes touch usable on this board.
//
// Measured on the LilyGo T-RGB, counted in the driver rather than inferred:
// for the first ~30 s after boot about 12% of first reads fail, and from then
// on about 99.9% of them do while the glass is idle, dropping to roughly 48%
// while a finger is actually on it. The retry succeeds almost every time, so
// what the application sees is 0.3-0.4%.
//
// That shape is the FocalTech controller entering its low-power monitor state:
// it stops answering between contacts, and the first read wakes it. It is not a
// wedged bus, and it is not a fault that gets worse with time - an earlier
// reading that it "died 30 s into every boot" was this transition being
// mistaken for a failure, and its coincidence with TICK_INTERVAL_MS was exactly
// that, a coincidence.
//
// Without the retry, the read most likely to fail is the FIRST read of a new
// contact - which on a menu is invisible, because a contact spans many polls,
// but in a minigame is the one read that decides whether the press registers at
// all. That is why the games felt unresponsive and nothing else did. On a menu that is
// invisible, because a contact spans several polls and the gesture recognizer
// already tolerates a dropped one. In a minigame, where the press is taken on
// the rising edge of contact and every press is a move, it is the difference
// between a game that responds and one that does not.
//
// A retry is what the I2C protocol expects a master to do on a NAK or a lost
// arbitration; it is not papering over the fault. The counters below exist so
// the underlying rate stays visible rather than being hidden by the retry.
bool TouchModule::readRegister(uint8_t reg, uint8_t* data, size_t len) {
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    _reads++;
    if (readRegisterOnce(reg, data, len)) {
        return true;
    }
    _firstFail++;
    if (readRegisterOnce(reg, data, len)) {
        return true;
    }
    _hardFail++;
    return false;
#else
    return readRegisterOnce(reg, data, len);
#endif
}

bool TouchModule::readRegisterOnce(uint8_t reg, uint8_t* data, size_t len) {
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_HAS_TOUCH) && HEXHOUND_HAS_TOUCH
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    size_t readLen = Wire.requestFrom((int)_addr, (int)len);
    if (readLen != len) {
        while (Wire.available()) {
            Wire.read();
        }
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)Wire.read();
    }
    return true;
#else
    (void)reg;
    (void)data;
    (void)len;
    return false;
#endif
}

void TouchModule::applyRotation(uint16_t rawX, uint16_t rawY, TouchPoint& out) const {
    uint16_t width = _width > 0 ? _width : SCREEN_W;
    uint16_t height = _height > 0 ? _height : SCREEN_H;

    switch (_rotation & 0x03) {
    case 1:
        out.y = rawX;
        out.x = rawY;
        break;
    case 2:
        out.x = rawX;
        out.y = (uint16_t)(height - 1 - rawY);
        break;
    case 3:
        out.y = (uint16_t)(height - 1 - rawX);
        out.x = (uint16_t)(width - 1 - rawY);
        break;
    case 0:
    default:
        out.x = (uint16_t)(width - 1 - rawX);
        out.y = rawY;
        break;
    }
}
