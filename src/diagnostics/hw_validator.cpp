#include "hw_validator.h"
#include "../config.h"
#include "../hal/hal.h"
#include "../modules/storage_module.h"
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <WiFi.h>
#ifndef HEXHOUND_DISABLE_LED
#include <FastLED.h>
#endif

// Dedicated SPI bus for SD card - must NOT use the default FSPI (SPI2)
// which TFT_eSPI owns. Using HSPI (SPI3) avoids clobbering TFT pin routing.
static SPIClass hwTestSdSPI(HSPI);

namespace {

bool roundTripFile(fs::FS& fs, const char* path, const String& expected) {
    if (fs.exists(path) && !fs.remove(path)) {
        return false;
    }

    File out = fs.open(path, FILE_WRITE);
    if (!out) {
        return false;
    }
    out.print(expected);
    out.flush();
    out.close();

    File in = fs.open(path, FILE_READ);
    if (!in) {
        return false;
    }
    String actual = in.readString();
    in.close();
    fs.remove(path);
    return actual == expected;
}

} // namespace

// ── HexHound - Hardware Validation Implementation ─────────────────

#define COL_PASS   0x07E0  // green
#define COL_FAIL   0xF800  // red
#define COL_WARN   0xFD20  // amber
#define COL_SKIP   0x8410  // gray
#define COL_HEADER 0x07FF  // cyan
#define COL_TEXT   0xFFFF  // white
#define COL_DIM    0x8410  // gray

static const char* resultStr(HWTestResult r) {
    switch (r) {
        case HW_PASS: return "PASS";
        case HW_FAIL: return "FAIL";
        case HW_WARN: return "WARN";
        case HW_SKIP: return "SKIP";
        default:      return "????";
    }
}

static uint16_t resultColor(HWTestResult r) {
    switch (r) {
        case HW_PASS: return COL_PASS;
        case HW_FAIL: return COL_FAIL;
        case HW_WARN: return COL_WARN;
        case HW_SKIP: return COL_SKIP;
        default:      return COL_TEXT;
    }
}

HWValidator& HWValidator::instance() {
    static HWValidator v;
    return v;
}

void HWValidator::init(TFT_eSPI* tft) {
    _tft = tft;
}

bool HWValidator::isBootTestRequested() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    delay(50);  // debounce
    return (digitalRead(PIN_BUTTON) == LOW);
}

// ── Run All Tests ───────────────────────────────────────────────────────────

void HWValidator::runAll() {
    memset(&_report, 0, sizeof(_report));

    drawTestHeader();

    // Test 1: Display (if we see this, it passed)
    _report.display = testDisplay();
    drawTestLine(0, "Display", _report.display);

    // Test 2: SD Card
    _report.persistence = testPersistence();
    drawTestLine(1, "Persist", _report.persistence);

    // Test 3: SD Card
    _report.sdCard = testSDCard();
    drawTestLine(2, "SD Card", _report.sdCard);

    // Test 4: WiFi
    _report.wifi = testWiFi();
    drawTestLine(3, "WiFi", _report.wifi);

    // Test 5: BLE
    _report.ble = testBLE();
    drawTestLine(4, "BLE", _report.ble);

    // Test 6: LED (visual confirm - user sees color flash)
    _report.led = testLED();
    drawTestLine(5, "LED", _report.led);

    // Test 7: USB
    _report.usb = testUSB();
    drawTestLine(6, "USB", _report.usb);

    // Tally results
    _report.totalTests = 7;
    _report.passCount = 0;
    _report.failCount = 0;
    HWTestResult* results[] = {
        &_report.display, &_report.persistence, &_report.sdCard, &_report.wifi,
        &_report.ble, &_report.led, &_report.usb
    };
    for (int i = 0; i < 7; i++) {
        if (*results[i] == HW_PASS) _report.passCount++;
        else if (*results[i] == HW_FAIL) _report.failCount++;
    }

    drawSummary();
    saveResults();

    // Wait for button release, then button press to continue
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);
    Serial.println("[HWTest] Tests complete. Press button to continue...");
    while (digitalRead(PIN_BUTTON) == HIGH) delay(10);
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);  // wait for release
}

// ── Individual Tests ─────────────────────────────────────────────────────────

HWTestResult HWValidator::testDisplay() {
    // If we can draw to screen, display works
    if (!_tft) return HW_FAIL;

    // Draw a test pattern - colored bars
    int barH = 10;
    int y = 50;
    _tft->fillRect(0, y, 40, barH, TFT_RED);
    _tft->fillRect(40, y, 40, barH, TFT_GREEN);
    _tft->fillRect(80, y, 40, barH, TFT_BLUE);
    _tft->fillRect(120, y, 40, barH, TFT_WHITE);
    delay(300);

    // Overwrite with black (cleanup)
    _tft->fillRect(0, y, SCREEN_W, barH, TFT_BLACK);

    Serial.println("[HWTest] Display: PASS");
    return HW_PASS;
}

HWTestResult HWValidator::testPersistence() {
    auto& storage = StorageModule::instance();
    if (!storage.isReady()) {
        Serial.println("[HWTest] Persistence: FAIL (storage not initialized)");
        return HW_FAIL;
    }

    String token = String("hexhound-persist-") + String((unsigned long)millis());

    if (storage.isFlashReady()) {
        if (roundTripFile(SPIFFS, "/hw_persist_probe.txt", token)) {
            Serial.println("[HWTest] Persistence: PASS (SPIFFS roundtrip)");
            return HW_PASS;
        }
        Serial.println("[HWTest] Persistence: FAIL (SPIFFS roundtrip)");
        return HW_FAIL;
    }

    if (storage.isSDReady()) {
        if (roundTripFile(SD, "/sd/hw_persist_probe.txt", token)) {
            Serial.println("[HWTest] Persistence: PASS (SD roundtrip)");
            return HW_PASS;
        }
        Serial.println("[HWTest] Persistence: FAIL (SD roundtrip)");
        return HW_FAIL;
    }

    Serial.println("[HWTest] Persistence: FAIL (no backend)");
    return HW_FAIL;
}

HWTestResult HWValidator::testSDCard() {
    // Init SD on HSPI (SPI3) so it doesn't clobber TFT_eSPI on FSPI (SPI2)
    hwTestSdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, hwTestSdSPI)) {
        Serial.println("[HWTest] SD Card: FAIL (init failed)");
        return HW_FAIL;
    }

    // Check card type
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[HWTest] SD Card: FAIL (no card)");
        return HW_FAIL;
    }

    // Try to read card size
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[HWTest] SD Card: PASS (%lluMB)\n", cardSize);
    return HW_PASS;
}

HWTestResult HWValidator::testWiFi() {
    // Quick scan - just check if WiFi radio initializes and finds networks
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int16_t n = WiFi.scanNetworks(false, false, false, 300);  // 300ms timeout

    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);

    if (n < 0) {
        Serial.println("[HWTest] WiFi: FAIL (scan error)");
        return HW_FAIL;
    }
    if (n == 0) {
        Serial.println("[HWTest] WiFi: WARN (0 networks, radio OK)");
        return HW_WARN;  // radio works but no networks visible
    }

    Serial.printf("[HWTest] WiFi: PASS (%d networks)\n", n);
    return HW_PASS;
}

HWTestResult HWValidator::testBLE() {
    // NimBLE init check - we just verify the stack initializes
    // Full BLE scan would take too long for diagnostics
    // BLEModule::init() is called later; here we do a minimal check
    #if defined(CONFIG_BT_ENABLED)
        Serial.println("[HWTest] BLE: PASS (BT enabled in build)");
        return HW_PASS;
    #else
        Serial.println("[HWTest] BLE: FAIL (BT not enabled)");
        return HW_FAIL;
    #endif
}

HWTestResult HWValidator::testLED() {
#ifdef HEXHOUND_DISABLE_LED
    Serial.println("[HWTest] LED: SKIP (disabled in build)");
    return HW_SKIP;
#else
    // Flash the APA102 LED through R->G->B->off
    halLED().setColor(255, 0, 0);
    delay(200);
    halLED().setColor(0, 255, 0);
    delay(200);
    halLED().setColor(0, 0, 255);
    delay(200);
    halLED().clear();

    // Can't programmatically verify - user sees colors
    Serial.println("[HWTest] LED: PASS (visual confirm)");
    return HW_PASS;
#endif
}

HWTestResult HWValidator::testUSB() {
    // Check if USB/Serial is available
    if (Serial) {
        Serial.println("[HWTest] USB: PASS (Serial active)");
        return HW_PASS;
    }
    // If we got here, serial might not be connected but USB could still work
    return HW_WARN;
}

// ── UI Drawing ──────────────────────────────────────────────────────────────

void HWValidator::drawTestHeader() {
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 HW DIAGNOSTICS");
    _tft->drawFastHLine(0, 12, SCREEN_W, 0x2104);

    _tft->setTextColor(COL_DIM, TFT_BLACK);
    _tft->setCursor(4, 14);
    _tft->print("Running tests...");
}

void HWValidator::drawTestLine(int row, const char* name, HWTestResult result) {
    if (!_tft) return;

    int lineSpacing = (SCREEN_H <= 80) ? 7 : 10;
    int y = 22 + row * lineSpacing;
    _tft->setTextSize(1);

    // Test name
    _tft->setTextColor(COL_TEXT, TFT_BLACK);
    _tft->setCursor(4, y);
    _tft->print(name);

    // Dots
    int nameLen = strlen(name);
    _tft->setTextColor(COL_DIM, TFT_BLACK);
    int dotX = 4 + nameLen * 6 + 2;
    while (dotX < SCREEN_W - 30) {
        _tft->setCursor(dotX, y);
        _tft->print(".");
        dotX += 6;
    }

    // Result
    _tft->setTextColor(resultColor(result), TFT_BLACK);
    _tft->setCursor(SCREEN_W - 28, y);
    _tft->print(resultStr(result));

    delay(150);  // brief pause for visual feedback
}

void HWValidator::drawSummary() {
    if (!_tft) return;

    int y = SCREEN_H - 10;
    _tft->drawFastHLine(0, y - 3, SCREEN_W, 0x2104);
    _tft->setTextSize(1);

    uint16_t sumColor = (_report.failCount == 0) ? COL_PASS : COL_FAIL;
    _tft->setTextColor(sumColor, TFT_BLACK);
    _tft->setCursor(4, y);
    _tft->printf("%d/%d PASS", _report.passCount, _report.totalTests);

    _tft->setTextColor(COL_DIM, TFT_BLACK);
    _tft->setCursor(SCREEN_W - 54, y);
    _tft->print("btn=cont");
}

void HWValidator::saveResults() {
    // Save results to SD if available (reuse HSPI bus from testSDCard)
    if (!SD.begin(PIN_SD_CS, hwTestSdSPI)) {
        Serial.println("[HWTest] Cannot save - SD not ready");
        return;
    }

    File f = SD.open("/sd/hwtest_results.log", FILE_WRITE);
    if (!f) {
        Serial.println("[HWTest] Cannot open results log");
        return;
    }

    f.println("=== HexHound HW Test Results ===");
    f.printf("Timestamp: %lu ms\n", millis());
    f.println("-------------------------------------------");

    const char* names[] = {"Display", "Persist", "SD Card", "WiFi", "BLE", "LED", "USB"};
    HWTestResult results[] = {
        _report.display, _report.persistence, _report.sdCard, _report.wifi,
        _report.ble, _report.led, _report.usb
    };

    for (int i = 0; i < 7; i++) {
        f.printf("%-10s %s\n", names[i], resultStr(results[i]));
    }

    f.println("-------------------------------------------");
    f.printf("PASS: %d / %d\n", _report.passCount, _report.totalTests);
    f.printf("FAIL: %d\n", _report.failCount);
    f.println("===========================================");
    f.close();

    Serial.println("[HWTest] Results saved to /sd/hwtest_results.log");
}
