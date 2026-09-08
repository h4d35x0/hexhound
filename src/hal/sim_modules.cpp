#ifdef SIMULATOR_BUILD

// ── HexHound - Simulator Module Stubs ────────────────────────────
// Fake implementations of WiFi, BLE, USB, Storage, and Notif modules
// that inject synthetic data for desktop testing.

#include "hal.h"
#include "tft_compat.h"
#include "../config.h"
#include "../modules/wifi_module.h"
#include "../modules/ble_module.h"
#include "../modules/usb_module.h"
#include "../modules/storage_module.h"
#include "../modules/notif_module.h"
#include "../events/event_bus.h"
#include "../events/event_types.h"
#include "../pet/pet_core.h"
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ═══════════════════════════════════════════════════════════════════════════
//  WiFi Module - Simulator Stub
// ═══════════════════════════════════════════════════════════════════════════

static const char* FAKE_SSIDS[] = {
    "HomeNetwork", "CoffeeShop_Free", "NETGEAR-5G",
    "FBI_Surveillance_Van", "HomeNetwork",  // duplicate!
    "OpenWiFi", "TP-Link_Guest",  // OpenWiFi is open
    "DIRECT-printer"
};
static const int FAKE_SSID_COUNT = 8;

WiFiModule& WiFiModule::instance() {
    static WiFiModule mod;
    return mod;
}

void WiFiModule::init() {
    ::printf("[WiFi-Sim] Initialized (fake data mode)\n");
}

bool WiFiModule::startScan() {
    if (_scanning) return false;
    _scanning = true;
    _resultCount = 0;
    _openCount = 0;
    _dupeCount = 0;
    memset(_results, 0, sizeof(_results));
    ::printf("[WiFi-Sim] Scan started\n");
    return true;
}

bool WiFiModule::pollScan() {
    if (!_scanning) return false;

    // Complete after 3 seconds
    static uint32_t scanStart = 0;
    if (scanStart == 0) scanStart = millis();
    if (millis() - scanStart < 3000) return false;
    scanStart = 0;

    _scanning = false;
    _resultCount = FAKE_SSID_COUNT;

    for (int i = 0; i < _resultCount; i++) {
        strlcpy(_results[i].ssid, FAKE_SSIDS[i], sizeof(_results[i].ssid));
        snprintf(_results[i].bssid, sizeof(_results[i].bssid), "AA:BB:CC:DD:EE:%02d", i);
        _results[i].rssi = -40 - (rand() % 40);
        _results[i].channel = 1 + (rand() % 13);
        _results[i].encType = 0;
        _results[i].isDuplicate = false;
        _results[i].isOpen = false;
        _results[i].isHidden = false;
    }

    // Mark "OpenWiFi" as open
    _results[5].isOpen = true;
    _results[5].encType = 0; // WIFI_AUTH_OPEN

    // Analyze for duplicates
    analyzeResults();

    // Fire callbacks
    if (_onNetworkFound) {
        for (int i = 0; i < _resultCount; i++) {
            _onNetworkFound(_results[i].ssid, _results[i].isDuplicate,
                           _results[i].isOpen, _results[i].channel);
        }
    }

    // Publish events
    auto& bus = EventBus::instance();
    bus.publish(EVENT_WIFI_SCAN_DONE, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isDuplicate) bus.publish(EVENT_WIFI_DUPLICATE_SSID, i);
        if (_results[i].isOpen) bus.publish(EVENT_WIFI_OPEN_NETWORK, i);
    }

    ::printf("[WiFi-Sim] Scan done: %d nets, %d open, %d dupes\n",
             _resultCount, _openCount, _dupeCount);
    return true;
}

void WiFiModule::analyzeResults() {
    _openCount = 0;
    _dupeCount = 0;
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isOpen) _openCount++;
        if (isDuplicateSSID(_results[i].ssid, i)) {
            _results[i].isDuplicate = true;
            _dupeCount++;
        }
    }
}

bool WiFiModule::isDuplicateSSID(const char* ssid, int currentIndex) {
    if (strlen(ssid) == 0) return false;
    for (int i = 0; i < currentIndex; i++) {
        if (strcmp(_results[i].ssid, ssid) == 0) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BLE Module - Simulator Stub
// ═══════════════════════════════════════════════════════════════════════════

static const char* FAKE_BLE_NAMES[] = { "MI Band 5", "", "JBL Speaker", "AirPods", "Unknown" };
static const char* FAKE_BLE_ADDRS[] = {
    "AA:BB:CC:DD:EE:01", "FF:EE:DD:CC:BB:02",
    "11:22:33:44:55:03", "AA:BB:CC:DD:EE:04",
    "FF:00:11:22:33:05"
};
static const int FAKE_BLE_COUNT = 5;

BLEModule& BLEModule::instance() {
    static BLEModule mod;
    return mod;
}

void BLEModule::init() {
    ::printf("[BLE-Sim] Initialized (fake data mode)\n");
}

void BLEModule::startScan() {
    _scanning = true;
    _resultCount = 0;
    _trackerCount = 0;
    memset(_results, 0, sizeof(_results));

    for (int i = 0; i < FAKE_BLE_COUNT && _resultCount < MAX_BLE_DEVICES; i++) {
        strlcpy(_results[i].name, FAKE_BLE_NAMES[i], sizeof(_results[i].name));
        strlcpy(_results[i].addr, FAKE_BLE_ADDRS[i], sizeof(_results[i].addr));
        _results[i].rssi = -30 - (rand() % 50);
        _results[i].companyId = (i == 3) ? 0x004C : 0; // AirPods = Apple
        _results[i].isTracker = isLikelyTracker(
            _results[i].companyId, _results[i].rssi, _results[i].name);
        if (_results[i].isTracker) _trackerCount++;
        _resultCount++;
    }

    _scanning = false;

    auto& bus = EventBus::instance();
    bus.publish(EVENT_BLE_DEVICE_FOUND, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isTracker) {
            bus.publish(EVENT_BLE_TRACKER_ALERT, _results[i].rssi);
        }
    }
}

bool BLEModule::startAsyncScan() {
    if (_scanning) return false;
    _scanning = true;
    _asyncTotalFound = 0;
    _asyncProcessed = 0;
    _asyncDone = false;
    _asyncEventPublished = false;
    _resultCount = 0;
    _trackerCount = 0;
    memset(_asyncBuffer, 0, sizeof(_asyncBuffer));
    memset(_results, 0, sizeof(_results));
    ::printf("[BLE-Sim] Async scan started\n");
    return true;
}

void BLEModule::onAsyncDeviceFound(void*) {}

void BLEModule::onAsyncScanComplete() {
    _asyncDone = true;
    _scanning = false;
}

int BLEModule::pollNewDevices(BLEResult* outBuf, int maxOut) {
    if (!_scanning && !_asyncDone) return 0;

    // Simulate devices appearing one at a time
    static uint32_t lastDeviceTime = 0;
    uint32_t now = millis();

    if (_asyncTotalFound < FAKE_BLE_COUNT && (now - lastDeviceTime > 600)) {
        lastDeviceTime = now;
        int idx = _asyncTotalFound;
        strlcpy(_asyncBuffer[idx].name, FAKE_BLE_NAMES[idx], sizeof(_asyncBuffer[idx].name));
        strlcpy(_asyncBuffer[idx].addr, FAKE_BLE_ADDRS[idx], sizeof(_asyncBuffer[idx].addr));
        _asyncBuffer[idx].rssi = -30 - (rand() % 50);
        _asyncBuffer[idx].companyId = (idx == 3) ? 0x004C : 0;
        _asyncBuffer[idx].isTracker = isLikelyTracker(
            _asyncBuffer[idx].companyId, _asyncBuffer[idx].rssi, _asyncBuffer[idx].name);
        _asyncTotalFound = idx + 1;
    }

    // Mark scan complete after all devices
    if (_asyncTotalFound >= FAKE_BLE_COUNT && !_asyncDone) {
        _asyncDone = true;
        _scanning = false;
        ::printf("[BLE-Sim] Async scan complete: %d devices\n", _asyncTotalFound);
    }

    int available = _asyncTotalFound - _asyncProcessed;
    if (available <= 0) return 0;

    int toProcess = available < maxOut ? available : maxOut;
    for (int i = 0; i < toProcess; i++) {
        int idx = _asyncProcessed + i;
        outBuf[i] = _asyncBuffer[idx];
        if (_resultCount < MAX_BLE_DEVICES) {
            _results[_resultCount] = _asyncBuffer[idx];
            if (_asyncBuffer[idx].isTracker) _trackerCount++;
            _resultCount++;
        }
        if (_onDeviceFound) {
            _onDeviceFound(_asyncBuffer[idx].addr, _asyncBuffer[idx].isTracker);
        }
    }
    _asyncProcessed += toProcess;

    if (_asyncDone && _asyncProcessed >= _asyncTotalFound) {
        auto& bus = EventBus::instance();
        bus.publish(EVENT_BLE_DEVICE_FOUND, _resultCount);
        for (int i = 0; i < _resultCount; i++) {
            if (_results[i].isTracker) {
                bus.publish(EVENT_BLE_TRACKER_ALERT, _results[i].rssi);
            }
        }
    }

    return toProcess;
}

bool BLEModule::isLikelyTracker(uint16_t companyId, int8_t rssi, const char* name) {
    if (companyId == 0x004C || companyId == 0x0075 || companyId == 0x03DA) return true;
    if (strlen(name) == 0 && rssi > -50) return true;
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  USB Module - Simulator Stub
// ═══════════════════════════════════════════════════════════════════════════

const Mission USBModule::_missions[] = {
    { 0, "Password Tip", "Tip: Use a password manager.", MISSION_NORMAL },
    { 1, "Phishing Alert", "Tip: Verify sender emails.", MISSION_NORMAL },
    { 2, "WiFi Safety", "Tip: Avoid open WiFi.", MISSION_NORMAL },
    { 3, "WiFi Audit", nullptr, MISSION_NORMAL },
    { 4, "Lock Screen", nullptr, MISSION_HIGH_MISCHIEF | MISSION_GUI_KEYS },
    { 5, "Sec Checklist", "=== CHECKLIST ===", MISSION_NORMAL },
    { 6, "Map Link", nullptr, MISSION_GUI_KEYS },
    { 7, "Assessment Note", ">> PHYSICAL SECURITY ASSESSMENT NOTE <<", MISSION_HIGH_MISCHIEF },
    // Mission 8 was MISSING here while the firmware table in usb_module.cpp has
    // had it since Term Tip was added. MAX_MISSIONS is 9, so the ninth element
    // was not absent - it was value-initialised to { 0, nullptr, nullptr, 0 },
    // and nothing about that is visible at the declaration.
    //
    // It was reachable. UIMissions::totalEntries() is missionCount() + 1, which
    // is 10 rows, and the last row calls getMission(8) and hands m.name to
    // strlcpy(). A null there is strlen(nullptr), so scrolling to the bottom of
    // the missions list in the simulator was a segfault, not a blank row. The
    // round panel reaches the same field through snprintf("%s"), which is also
    // undefined and merely happens to print "(null)" on this libc.
    //
    // The lesson is the shape, not the row: an aggregate sized by a macro will
    // silently pad itself when the two drift apart. The static_assert below is
    // what makes the next divergence a build failure instead of a crash.
    { 8, "Term Tip", nullptr, MISSION_NORMAL },
};

USBModule& USBModule::instance() {
    static USBModule mod;
    return mod;
}

void USBModule::init() {
    ::printf("[USB-Sim] Initialized (simulated HID)\n");
}

void USBModule::update() {
    bool wasConnected = _wasConnected;
    _connected = true;  // always connected in sim
    if (_connected && !wasConnected) {
        EventBus::instance().publish(EVENT_USB_CONNECTED);
    }
    _wasConnected = _connected;
}

bool USBModule::executeMission(uint8_t idx, GuiKeyConsent consent) {
    if (idx >= MAX_MISSIONS) return false;
    // Same refusal as the firmware. A simulator that runs a mission the device
    // would refuse is worse than no simulator for this screen.
    if ((_missions[idx].flags & MISSION_GUI_KEYS) &&
        consent != GuiKeyConsent::ConfirmedOnGlass) {
        ::printf("[USB-Sim] Refused: GUI-keys mission with no confirmation\n");
        return false;
    }
    ::printf("[USB-Sim] Mission executed: %s\n", _missions[idx].name);
    EventBus::instance().publish(EVENT_MISSION_COMPLETE, idx);
    return true;
}

const Mission& USBModule::getMission(uint8_t idx) const {
    // The bound is deduced from the initialiser list above (see usb_module.h),
    // so this is the check that a row was not quietly left out.
    static_assert(sizeof(_missions) / sizeof(_missions[0]) == MAX_MISSIONS,
                  "mission table size must match MAX_MISSIONS");
    if (idx >= MAX_MISSIONS) idx = 0;
    return _missions[idx];
}

void USBModule::setLastPatrolSummary(const char* summary) {
    strlcpy(_lastPatrolSummary, summary, sizeof(_lastPatrolSummary));
}

// Signatures must track usb_module.h. These four are never called here - the
// simulator's executeMission() above short-circuits before reaching them - but
// they still have to MATCH, and a mismatch is a compile error rather than a
// silent divergence, which is the point.
//
// The three that gained a bool return did so because a mission must be able to
// refuse to type when it cannot confirm the host's keyboard is clear. Returning
// false here is the honest simulator answer: there is no host and no keyboard,
// so nothing can ever be confirmed.
void USBModule::typeString(const char*) {}
void USBModule::typeMultiLine(const char*) {}
void USBModule::typeChar(char) {}
void USBModule::typePaced(const char*) {}
bool USBModule::clearAllKeys() { return false; }
bool USBModule::pressCombo(uint8_t, uint8_t) { return false; }
void USBModule::executeTerminalTip() {}
bool USBModule::executeLockScreen() { return false; }
bool USBModule::executeOpenURL(const char*) { return false; }

// ═══════════════════════════════════════════════════════════════════════════
//  Storage Module - Simulator Stub (in-memory)
// ═══════════════════════════════════════════════════════════════════════════

static String s_savedPetJson = "";
static String s_savedConfigJson = "";
static bool s_hasSavedPetState = false;

StorageModule& StorageModule::instance() {
    static StorageModule mod;
    return mod;
}

bool StorageModule::init() {
    _ready = true;
    ::printf("[Storage-Sim] In-memory storage initialized\n");
    return true;
}

bool StorageModule::ensureDir(const char*) { return true; }

bool StorageModule::loadPetState() {
    if (!s_hasSavedPetState || s_savedPetJson.empty()) {
        return false;
    }
    return PetCore::instance().loadFrom(s_savedPetJson.c_str());
}

bool StorageModule::savePetState() {
    s_savedPetJson = PetCore::instance().saveToJson();
    s_hasSavedPetState = true;
    PetCore::instance().state().dirty = false;
    return true;
}

// Simple in-memory journal
static char s_journal[4096] = "";
static int  s_journalLen = 0;

bool StorageModule::appendJournal(const char* type, const char* detail1,
                                   const char* detail2) {
    char line[128];
    int len = snprintf(line, sizeof(line), "%lu|%s|%s|%s\n",
                       (unsigned long)millis(), type, detail1, detail2);
    if (s_journalLen + len < (int)sizeof(s_journal)) {
        memcpy(s_journal + s_journalLen, line, len);
        s_journalLen += len;
        s_journal[s_journalLen] = '\0';
    }
    return true;
}

String StorageModule::readLastJournalEntries(int count) {
    return String(s_journal);
}

bool StorageModule::loadConfig() {
    if (s_savedConfigJson.empty()) {
        return true;
    }

    JsonDocument doc;
    if (deserializeJson(doc, s_savedConfigJson)) {
        return false;
    }

    _ssidCount = 0;
    JsonArray ssids = doc["trusted_ssids"].as<JsonArray>();
    for (JsonVariant v : ssids) {
        if (_ssidCount < MAX_TRUSTED_SSIDS) {
            strlcpy(_trustedSSIDs[_ssidCount], v.as<const char*>(), 33);
            _ssidCount++;
        }
    }

    _hostCount = 0;
    JsonArray hosts = doc["trusted_hosts"].as<JsonArray>();
    for (JsonVariant v : hosts) {
        if (_hostCount < MAX_TRUSTED_HOSTS) {
            strlcpy(_trustedHosts[_hostCount], v.as<const char*>(), 32);
            _hostCount++;
        }
    }

    _displayFlipped = doc["display_flipped"] | false;
    return true;
}

bool StorageModule::saveConfig() {
    JsonDocument doc;

    JsonArray ssids = doc["trusted_ssids"].to<JsonArray>();
    for (int i = 0; i < _ssidCount; i++) {
        ssids.add(_trustedSSIDs[i]);
    }

    JsonArray hosts = doc["trusted_hosts"].to<JsonArray>();
    for (int i = 0; i < _hostCount; i++) {
        hosts.add(_trustedHosts[i]);
    }

    doc["display_flipped"] = _displayFlipped;
    s_savedConfigJson.clear();
    serializeJson(doc, s_savedConfigJson);
    return true;
}

bool StorageModule::isSSIDTrusted(const char* ssid) {
    for (int i = 0; i < _ssidCount; i++) {
        if (strcmp(_trustedSSIDs[i], ssid) == 0) return true;
    }
    return false;
}

void StorageModule::addTrustedSSID(const char* ssid) {
    if (_ssidCount >= MAX_TRUSTED_SSIDS) return;
    if (isSSIDTrusted(ssid)) return;
    strlcpy(_trustedSSIDs[_ssidCount], ssid, 33);
    _ssidCount++;
}

void StorageModule::removeTrustedSSID(int index) {
    if (index < 0 || index >= _ssidCount) return;
    for (int i = index; i < _ssidCount - 1; i++) {
        strlcpy(_trustedSSIDs[i], _trustedSSIDs[i + 1], 33);
    }
    _ssidCount--;
}

const char* StorageModule::getTrustedSSID(int idx) const {
    if (idx < 0 || idx >= _ssidCount) return "";
    return _trustedSSIDs[idx];
}

bool StorageModule::isHostTrusted(const char* host) {
    for (int i = 0; i < _hostCount; i++) {
        if (strcmp(_trustedHosts[i], host) == 0) return true;
    }
    return false;
}

void StorageModule::addTrustedHost(const char* host) {
    if (_hostCount >= MAX_TRUSTED_HOSTS) return;
    if (isHostTrusted(host)) return;
    strlcpy(_trustedHosts[_hostCount], host, 32);
    _hostCount++;
}

void StorageModule::removeTrustedHost(int index) {
    if (index < 0 || index >= _hostCount) return;
    for (int i = index; i < _hostCount - 1; i++) {
        strlcpy(_trustedHosts[i], _trustedHosts[i + 1], 32);
    }
    _hostCount--;
}

const char* StorageModule::getTrustedHost(int idx) const {
    if (idx < 0 || idx >= _hostCount) return "";
    return _trustedHosts[idx];
}

void StorageModule::toggleDisplayFlip() {
    _displayFlipped = !_displayFlipped;
    saveConfig();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Notif Module - Simulator Stub
// ═══════════════════════════════════════════════════════════════════════════

NotifModule& NotifModule::instance() {
    static NotifModule mod;
    return mod;
}

void NotifModule::init() {
    halLED().init();
    ::printf("[Notif-Sim] LED initialized\n");
}

void NotifModule::update() {
    if (_active && millis() > _showUntil) {
        clear();
    }
}

void NotifModule::notify(NotifLevel level, const char* message, const char* source) {
    _current.level = level;
    _current.timestamp = millis();
    strlcpy(_current.message, message, sizeof(_current.message));
    strlcpy(_current.source, source, sizeof(_current.source));
    _active = true;
    _showUntil = millis() + 5000;

    switch (level) {
    case NOTIF_INFO: setLED(0, 0, 80); break;
    case NOTIF_WARN: setLED(80, 60, 0); break;
    case NOTIF_CRIT: setLED(100, 0, 0); break;
    }

    ::printf("[Notif] %s: %s [%s]\n",
             level == NOTIF_CRIT ? "CRIT" : level == NOTIF_WARN ? "WARN" : "INFO",
             message, source);
}

void NotifModule::clear() {
    _active = false;
    ledOff();
}

void NotifModule::setLED(uint8_t r, uint8_t g, uint8_t b) {
    halLED().setColor(r, g, b);
}

void NotifModule::ledOff() {
    halLED().clear();
}

void NotifModule::setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
    _active = false;
    setLED(r, g, b);
}

void NotifModule::ledClear() {
    _active = false;
    ledOff();
}

// ═══════════════════════════════════════════════════════════════════════════
//  HW Validator - Simulator Stub
// ═══════════════════════════════════════════════════════════════════════════

#include "../diagnostics/hw_validator.h"

HWValidator& HWValidator::instance() {
    static HWValidator val;
    return val;
}

void HWValidator::init(TFT_eSPI*) {}
void HWValidator::runAll() { ::printf("[HWTest-Sim] Skipped in simulator\n"); }
bool HWValidator::isBootTestRequested() { return false; }
HWTestResult HWValidator::testDisplay() { return HW_SKIP; }
HWTestResult HWValidator::testPersistence() { return HW_SKIP; }
HWTestResult HWValidator::testSDCard() { return HW_SKIP; }
HWTestResult HWValidator::testWiFi() { return HW_SKIP; }
HWTestResult HWValidator::testBLE() { return HW_SKIP; }
HWTestResult HWValidator::testLED() { return HW_SKIP; }
HWTestResult HWValidator::testUSB() { return HW_SKIP; }
void HWValidator::drawTestHeader() {}
void HWValidator::drawTestLine(int, const char*, HWTestResult) {}
void HWValidator::drawSummary() {}
void HWValidator::saveResults() {}

#endif // SIMULATOR_BUILD
