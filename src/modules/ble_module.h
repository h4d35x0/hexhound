#pragma once
#if defined(UNIT_TEST)
#include <cstdint>
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif
#include <functional>

// ── HexHound - BLE Scanning Module ──────────────────────────────

#define MAX_BLE_DEVICES 32

// Known tracker manufacturer prefixes (Apple FindMy, Tile, Samsung SmartTag)
// HARDWARE_VERIFY: expand with real OUI prefixes from hardware testing.
// Reference: docs/hardware_checklist.md
#define TRACKER_COMPANY_APPLE   0x004C
#define TRACKER_COMPANY_SAMSUNG 0x0075
#define TRACKER_COMPANY_TILE    0x03DA

struct BLEResult {
    char    name[24];
    char    addr[18];
    int8_t  rssi;
    bool    isTracker;
    uint16_t companyId;
};

// Callback fired per-device as devices are discovered
using BLEDeviceCallback = std::function<void(const char* addr, bool isTracker)>;

class BLEModule {
public:
    static BLEModule& instance();

    void init();

    // ── Blocking scan (legacy - used when HUD is not active) ──────────
    void startScan();

    // ── Async scan (non-blocking - for live HUD updates) ──────────────
    bool startAsyncScan();
    // Returns number of new devices found since last call. Fills outBuf.
    int  pollNewDevices(BLEResult* outBuf, int maxOut);
    bool isAsyncScanDone() const { return _asyncDone; }

    // Register per-device callback
    void setOnDeviceFound(BLEDeviceCallback cb) { _onDeviceFound = cb; }
    void clearOnDeviceFound() { _onDeviceFound = nullptr; }

    // Called by NimBLE scan callback (public so static wrapper can call)
    void onAsyncDeviceFound(void* advertisedDevice);
    void onAsyncScanComplete();

    int             resultCount()  const { return _resultCount; }
    const BLEResult* results()    const { return _results; }
    int             trackerCount() const { return _trackerCount; }
    bool            isScanning()   const { return _scanning; }

private:
    BLEModule() = default;
    bool isLikelyTracker(uint16_t companyId, int8_t rssi, const char* name);

    // Final results (populated after scan completes)
    BLEResult _results[MAX_BLE_DEVICES];
    int       _resultCount  = 0;
    int       _trackerCount = 0;
    bool      _scanning     = false;

    BLEDeviceCallback _onDeviceFound;

    // Async scan state
    volatile int  _asyncTotalFound = 0;   // written by NimBLE task
    int           _asyncProcessed  = 0;   // read by main loop
    volatile bool _asyncDone       = false;
    bool          _asyncEventPublished = false;
    BLEResult     _asyncBuffer[MAX_BLE_DEVICES];

    void publishAsyncResultsIfReady();
};
