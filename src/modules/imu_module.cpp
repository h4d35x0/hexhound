#include "imu_module.h"
#include "../config.h"
#include "../events/event_bus.h"

#if !defined(SIMULATOR_BUILD) && HEXHOUND_HAS_IMU
#include <Wire.h>
#endif

namespace {

// QMI8658 (Waveshare ESP32-S3-LCD-1.28). SA0 is tied low on this board, so the
// device answers at 0x6B; 0x6A is the SA0-high alternative and is probed as a
// fallback rather than assumed.
constexpr uint8_t QMI8658_ADDR_PRIMARY = 0x6B;
constexpr uint8_t QMI8658_ADDR_ALT     = 0x6A;

constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t REG_CTRL1    = 0x02;   // serial interface config
constexpr uint8_t REG_CTRL2    = 0x03;   // accelerometer config
constexpr uint8_t REG_CTRL7    = 0x08;   // sensor enable
constexpr uint8_t REG_AX_L     = 0x35;   // accel data, 6 bytes little-endian

constexpr uint8_t WHO_AM_I_VALUE = 0x05;

// CTRL1: address auto-increment for burst reads, little-endian.
constexpr uint8_t CTRL1_VALUE = 0x60;
// CTRL2: aFS = +/-8g (010), aODR = 500Hz-class setting (0100).
constexpr uint8_t CTRL2_VALUE = 0x24;
// CTRL7: accelerometer enable (gyro left off - it costs power and a pet does
// not need angular rate for shake/tilt).
constexpr uint8_t CTRL7_VALUE = 0x01;

// +/-8g full scale over a signed 16-bit range -> milli-g per LSB.
constexpr float MILLI_G_PER_LSB = 8000.0f / 32768.0f;

// Gesture thresholds, in milli-g.
// 2.2g. A deliberate shake clears this easily; setting the board down on a
// desk does not. Started at 1.9g, raised after a stray shake on the bench
// opened the menu on its own - a false long-press is more disruptive than a
// missed one, so this errs high.
constexpr int SHAKE_THRESHOLD_MG = 2200;
constexpr int TILT_THRESHOLD_MG  = 550;    // ~33 degrees off level
constexpr int TILT_RELEASE_MG    = 300;    // hysteresis back to neutral

constexpr uint32_t POLL_INTERVAL_MS   = 40;
constexpr uint32_t SHAKE_COOLDOWN_MS  = 700;
constexpr uint32_t TILT_COOLDOWN_MS   = 450;

uint8_t g_addr = QMI8658_ADDR_PRIMARY;

} // namespace

IMUModule& IMUModule::instance() {
    static IMUModule mod;
    return mod;
}

void IMUModule::init() {
#if defined(SIMULATOR_BUILD) || !HEXHOUND_HAS_IMU
    _available = false;
#else
    // The touch controller (on boards that have one) shares this bus, so only
    // begin it if nothing else already did.
    Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL);
    Wire.setClock(400000);

    uint8_t id = 0;
    bool found = false;
    for (uint8_t addr : { QMI8658_ADDR_PRIMARY, QMI8658_ADDR_ALT }) {
        g_addr = addr;
        if (readRegister(REG_WHO_AM_I, &id, 1) && id == WHO_AM_I_VALUE) {
            found = true;
            break;
        }
    }

    if (!found) {
        _available = false;
        Serial.println("[IMU] QMI8658 not found");
        return;
    }

    writeRegister(REG_CTRL1, CTRL1_VALUE);
    writeRegister(REG_CTRL2, CTRL2_VALUE);
    writeRegister(REG_CTRL7, CTRL7_VALUE);

    _available = true;
    Serial.printf("[IMU] QMI8658 ready at 0x%02X\n", g_addr);
#endif
}

void IMUModule::update() {
#if !defined(SIMULATOR_BUILD) && HEXHOUND_HAS_IMU
    if (!_available) return;

    uint32_t now = millis();
    if (_lastPollAt != 0 && (now - _lastPollAt) < POLL_INTERVAL_MS) {
        return;
    }
    _lastPollAt = now;

    uint8_t raw[6] = { 0 };
    if (!readRegister(REG_AX_L, raw, sizeof(raw))) {
        return;
    }

    int16_t rx = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ry = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t rz = (int16_t)((raw[5] << 8) | raw[4]);

    _ax = (int16_t)(rx * MILLI_G_PER_LSB);
    _ay = (int16_t)(ry * MILLI_G_PER_LSB);
    _az = (int16_t)(rz * MILLI_G_PER_LSB);

    // Shake: total acceleration well above the 1g the pet feels at rest.
    // Magnitude rather than any single axis, so a shake in any direction
    // counts and orientation does not matter.
    int32_t mag2 = (int32_t)_ax * _ax + (int32_t)_ay * _ay + (int32_t)_az * _az;
    int32_t thresh2 = (int32_t)SHAKE_THRESHOLD_MG * SHAKE_THRESHOLD_MG;
    if (mag2 > thresh2 && (now - _shakeArmedAt) > SHAKE_COOLDOWN_MS) {
        _shakeArmedAt = now;
        _shakePending = true;
        int intensity = (int)(sqrtf((float)mag2)) - 1000;
        EventBus::instance().publish(EVENT_SHAKE, intensity);
    }

    // Tilt is EDGE triggered, not level triggered. A cooldown alone is not
    // enough: if the board simply rests in a tilted orientation, |ax| stays
    // over the threshold forever and it re-fires every time the cooldown
    // expires. Observed on hardware - the pet walked itself into the menu and
    // kept scrolling while sitting on the desk.
    //
    // So: fire once when the tilt STARTS, then latch until the board comes
    // back near level (hysteresis via a lower release threshold, so a hand
    // wobbling near the trigger point does not chatter).
    if (_tiltLatched) {
        if (_ax > -TILT_RELEASE_MG && _ax < TILT_RELEASE_MG) {
            _tiltLatched = false;
        }
    } else if ((now - _tiltArmedAt) > TILT_COOLDOWN_MS) {
        if (_ax > TILT_THRESHOLD_MG) {
            _tiltArmedAt = now;
            _tiltLatched = true;
            _tiltRightPending = true;
            EventBus::instance().publish(EVENT_TILT_RIGHT, _ax);
        } else if (_ax < -TILT_THRESHOLD_MG) {
            _tiltArmedAt = now;
            _tiltLatched = true;
            _tiltLeftPending = true;
            EventBus::instance().publish(EVENT_TILT_LEFT, _ax);
        }
    }
#endif
}

bool IMUModule::takeShake() {
    bool v = _shakePending;
    _shakePending = false;
    return v;
}

bool IMUModule::takeTiltLeft() {
    bool v = _tiltLeftPending;
    _tiltLeftPending = false;
    return v;
}

bool IMUModule::takeTiltRight() {
    bool v = _tiltRightPending;
    _tiltRightPending = false;
    return v;
}

bool IMUModule::readRegister(uint8_t reg, uint8_t* data, size_t len) {
#if !defined(SIMULATOR_BUILD) && HEXHOUND_HAS_IMU
    Wire.beginTransmission(g_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    size_t got = Wire.requestFrom((int)g_addr, (int)len);
    if (got != len) {
        while (Wire.available()) Wire.read();
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)Wire.read();
    }
    return true;
#else
    (void)reg; (void)data; (void)len;
    return false;
#endif
}

bool IMUModule::writeRegister(uint8_t reg, uint8_t value) {
#if !defined(SIMULATOR_BUILD) && HEXHOUND_HAS_IMU
    Wire.beginTransmission(g_addr);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
#else
    (void)reg; (void)value;
    return false;
#endif
}
