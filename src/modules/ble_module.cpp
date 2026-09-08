#include "ble_module.h"
#include "../events/event_bus.h"
#include "../events/event_types.h"
#include "../config.h"

#ifdef HEXHOUND_USE_CORE_BLE
#include <BLEDevice.h>
#else
#include <NimBLEDevice.h>
#endif

// ── HexHound - BLE Module Implementation ────────────────────────

// ── Static NimBLE callback wrappers ──────────────────────────────────────

#ifdef HEXHOUND_USE_CORE_BLE
class HexHoundBLEScanCB : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        BLEModule::instance().onAsyncDeviceFound(&dev);
    }
};

static HexHoundBLEScanCB s_bleScanCallbacks;

static void onScanCompleteCB(BLEScanResults results) {
    BLEModule::instance().onAsyncScanComplete();
}
#else
class HexHoundBLEScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        BLEModule::instance().onAsyncDeviceFound(dev);
    }
};

static HexHoundBLEScanCB s_bleScanCallbacks;

static void onScanCompleteCB(NimBLEScanResults results) {
    BLEModule::instance().onAsyncScanComplete();
}
#endif

// ── Singleton ────────────────────────────────────────────────────────────

BLEModule& BLEModule::instance() {
    static BLEModule mod;
    return mod;
}

void BLEModule::init() {
#ifdef HEXHOUND_USE_CORE_BLE
    BLEDevice::init("HexHound");
    Serial.println("[BLE] Initialized via Arduino BLE");
#else
    NimBLEDevice::init("HexHound");
    Serial.println("[BLE] Initialized via NimBLE");
#endif
}

// ── Blocking Scan (legacy) ───────────────────────────────────────────────

void BLEModule::startScan() {
    if (_scanning) return;
    _scanning = true;
    _resultCount  = 0;
    _trackerCount = 0;
    memset(_results, 0, sizeof(_results));

    auto* scan =
#ifdef HEXHOUND_USE_CORE_BLE
        BLEDevice::getScan();
#else
        NimBLEDevice::getScan();
#endif
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);

    Serial.println("[BLE] Blocking scan starting...");
#ifdef HEXHOUND_USE_CORE_BLE
    BLEScanResults* scanResults = scan->start(BLE_SCAN_DURATION_S, false);
    int count = scanResults ? scanResults->getCount() : 0;
#else
    NimBLEScanResults scanResults = scan->start(BLE_SCAN_DURATION_S, false);
    int count = scanResults.getCount();
#endif

    for (int i = 0; i < count && _resultCount < MAX_BLE_DEVICES; i++) {
#ifdef HEXHOUND_USE_CORE_BLE
        BLEAdvertisedDevice dev = scanResults->getDevice(i);
#else
        NimBLEAdvertisedDevice dev = scanResults.getDevice(i);
#endif

        strlcpy(_results[_resultCount].name,
                dev.getName().c_str(),
                sizeof(_results[_resultCount].name));
        strlcpy(_results[_resultCount].addr,
                dev.getAddress().toString().c_str(),
                sizeof(_results[_resultCount].addr));
        _results[_resultCount].rssi = (int8_t)dev.getRSSI();

        uint16_t companyId = 0;
        if (dev.haveManufacturerData()) {
#ifdef HEXHOUND_USE_CORE_BLE
            String mfg = dev.getManufacturerData();
            if (mfg.length() >= 2) {
                companyId = (uint16_t)((uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8));
            }
#else
            std::string mfg = dev.getManufacturerData(0);
            if (mfg.size() >= 2) {
                companyId = (uint16_t)(mfg[0] | (mfg[1] << 8));
            }
#endif
        }
        _results[_resultCount].companyId = companyId;
        _results[_resultCount].isTracker = isLikelyTracker(
            companyId, _results[_resultCount].rssi, _results[_resultCount].name);

        if (_results[_resultCount].isTracker) _trackerCount++;
        _resultCount++;
    }

    scan->clearResults();
    _scanning = false;

    auto& bus = EventBus::instance();
    bus.publish(EVENT_BLE_DEVICE_FOUND, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isTracker) {
            bus.publish(EVENT_BLE_TRACKER_ALERT, _results[i].rssi);
        }
    }

    Serial.printf("[BLE] Blocking scan done: %d devs, %d trackers\n",
                  _resultCount, _trackerCount);
}

// ── Async Scan (non-blocking, for live HUD) ─────────────────────────────

bool BLEModule::startAsyncScan() {
    if (_scanning) return false;

    _scanning        = true;
    _asyncTotalFound = 0;
    _asyncProcessed  = 0;
    _asyncDone       = false;
    _asyncEventPublished = false;
    _resultCount     = 0;
    _trackerCount    = 0;
    memset(_asyncBuffer, 0, sizeof(_asyncBuffer));
    memset(_results, 0, sizeof(_results));

    auto* scan =
#ifdef HEXHOUND_USE_CORE_BLE
        BLEDevice::getScan();
#else
        NimBLEDevice::getScan();
#endif
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setAdvertisedDeviceCallbacks(&s_bleScanCallbacks, false);

    Serial.println("[BLE] Async scan starting...");
    // Non-blocking start with completion callback.
    if (!scan->start(BLE_SCAN_DURATION_S, onScanCompleteCB, false)) {
        _scanning = false;
        _asyncDone = true;
        Serial.println("[BLE] Async scan failed to start");
        return false;
    }
    return true;
}

// Called on NimBLE task when a device is discovered
void BLEModule::onAsyncDeviceFound(void* advDevice) {
#ifdef HEXHOUND_USE_CORE_BLE
    BLEAdvertisedDevice* dev = (BLEAdvertisedDevice*)advDevice;
#else
    NimBLEAdvertisedDevice* dev = (NimBLEAdvertisedDevice*)advDevice;
#endif
    int idx = _asyncTotalFound;
    if (idx >= MAX_BLE_DEVICES) return;

    strlcpy(_asyncBuffer[idx].name, dev->getName().c_str(),
            sizeof(_asyncBuffer[idx].name));
    strlcpy(_asyncBuffer[idx].addr, dev->getAddress().toString().c_str(),
            sizeof(_asyncBuffer[idx].addr));
    _asyncBuffer[idx].rssi = (int8_t)dev->getRSSI();

    uint16_t companyId = 0;
    if (dev->haveManufacturerData()) {
#ifdef HEXHOUND_USE_CORE_BLE
        String mfg = dev->getManufacturerData();
        if (mfg.length() >= 2) {
            companyId = (uint16_t)((uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8));
        }
#else
        std::string mfg = dev->getManufacturerData(0);
        if (mfg.size() >= 2) {
            companyId = (uint16_t)(mfg[0] | (mfg[1] << 8));
        }
#endif
    }
    _asyncBuffer[idx].companyId = companyId;
    _asyncBuffer[idx].isTracker = isLikelyTracker(
        companyId, _asyncBuffer[idx].rssi, _asyncBuffer[idx].name);

    // Increment AFTER filling data (atomic on ESP32 for aligned int)
    _asyncTotalFound = idx + 1;
}

// Called on NimBLE task when scan completes
void BLEModule::onAsyncScanComplete() {
    _asyncDone = true;
    _scanning = false;
    Serial.printf("[BLE] Async scan complete: %d devices\n", _asyncTotalFound);
}

// Called by main loop to drain newly discovered devices
int BLEModule::pollNewDevices(BLEResult* outBuf, int maxOut) {
    int available = _asyncTotalFound - _asyncProcessed;
    if (available <= 0) {
        publishAsyncResultsIfReady();
        return 0;
    }

    int toProcess = min(available, maxOut);
    for (int i = 0; i < toProcess; i++) {
        int idx = _asyncProcessed + i;
        outBuf[i] = _asyncBuffer[idx];

        // Copy to final results array
        if (_resultCount < MAX_BLE_DEVICES) {
            _results[_resultCount] = _asyncBuffer[idx];
            if (_asyncBuffer[idx].isTracker) _trackerCount++;
            _resultCount++;
        }

        // Fire per-device callback
        if (_onDeviceFound) {
            _onDeviceFound(_asyncBuffer[idx].addr, _asyncBuffer[idx].isTracker);
        }
    }

    _asyncProcessed += toProcess;

    publishAsyncResultsIfReady();

    return toProcess;
}

void BLEModule::publishAsyncResultsIfReady() {
    if (!_asyncDone || _asyncEventPublished || _asyncProcessed < _asyncTotalFound) {
        return;
    }

    auto& bus = EventBus::instance();
    bus.publish(EVENT_BLE_DEVICE_FOUND, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isTracker) {
            bus.publish(EVENT_BLE_TRACKER_ALERT, _results[i].rssi);
        }
    }
    _asyncEventPublished = true;
    Serial.printf("[BLE] Async results published: %d devices, %d trackers\n",
                  _resultCount, _trackerCount);
}

// ── Tracker Heuristics ───────────────────────────────────────────────────

bool BLEModule::isLikelyTracker(uint16_t companyId, int8_t rssi, const char* name) {
    // HARDWARE_VERIFY: refine heuristics with real-world testing.
    // Reference: docs/hardware_checklist.md
    if (companyId == TRACKER_COMPANY_APPLE ||
        companyId == TRACKER_COMPANY_SAMSUNG ||
        companyId == TRACKER_COMPANY_TILE) {
        return true;
    }
    if (strlen(name) == 0 && rssi > -50) {
        return true;
    }
    return false;
}
