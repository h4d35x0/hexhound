#pragma once
#ifndef SIMULATOR_BUILD
#include <Arduino.h>
#endif
#include "../hal/tft_compat.h"

// ── HexHound - Hardware Validation Suite ──────────────────────────
// Triggered by holding button during boot. Tests all hardware subsystems
// and displays results on screen + saves to /sd/hwtest_results.log.

enum HWTestResult : uint8_t {
    HW_PASS = 0,
    HW_FAIL,
    HW_SKIP,
    HW_WARN
};

struct HWTestReport {
    HWTestResult display;
    HWTestResult persistence;
    HWTestResult sdCard;
    HWTestResult wifi;
    HWTestResult ble;
    HWTestResult led;
    HWTestResult usb;
    int          passCount;
    int          failCount;
    int          totalTests;
};

class HWValidator {
public:
    static HWValidator& instance();

    void init(TFT_eSPI* tft);

    // Run full hardware test suite. Blocks until complete.
    void runAll();

    // Check if button held at boot (call early in setup)
    static bool isBootTestRequested();

    const HWTestReport& report() const { return _report; }

private:
    HWValidator() = default;

    // Individual test functions
    HWTestResult testDisplay();
    HWTestResult testPersistence();
    HWTestResult testSDCard();
    HWTestResult testWiFi();
    HWTestResult testBLE();
    HWTestResult testLED();
    HWTestResult testUSB();

    // UI helpers
    void drawTestHeader();
    void drawTestLine(int row, const char* name, HWTestResult result);
    void drawSummary();
    void saveResults();

    TFT_eSPI*    _tft = nullptr;
    HWTestReport _report;
};

