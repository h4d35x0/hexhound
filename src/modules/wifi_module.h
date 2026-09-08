#pragma once
#if defined(UNIT_TEST)
#include <cstdint>
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif
#include <functional>

// ── HexHound - Wi-Fi Scanning Module ────────────────────────────

#define MAX_SSIDS 32

struct WiFiResult {
    char    ssid[33];
    char    bssid[18];
    int8_t  rssi;
    uint8_t channel;
    uint8_t encType;  // WIFI_AUTH_OPEN, etc.
    bool    isDuplicate;
    bool    isOpen;
    bool    isHidden;
};

// Callback fired per-network as scan results are processed
using WiFiNetworkCallback = std::function<void(const char* ssid, bool isDuplicate,
                                                bool isOpen, int channel)>;

class WiFiModule {
public:
    static WiFiModule& instance();

    void init();

    // Start async scan, returns true if scan started
    bool startScan();

    // Check if scan is complete, process results
    bool pollScan();

    // Register a per-network callback (fired during pollScan for each network)
    void setOnNetworkFound(WiFiNetworkCallback cb) { _onNetworkFound = cb; }
    void clearOnNetworkFound() { _onNetworkFound = nullptr; }

    // Access results after scan
    int              resultCount() const { return _resultCount; }
    const WiFiResult* results()   const { return _results; }
    int              openCount()  const { return _openCount; }
    int              dupeCount()  const { return _dupeCount; }

    bool isScanning() const { return _scanning; }

private:
    WiFiModule() = default;

    void analyzeResults();
    bool isDuplicateSSID(const char* ssid, int currentIndex);

    WiFiResult _results[MAX_SSIDS];
    int        _resultCount = 0;
    int        _openCount   = 0;
    int        _dupeCount   = 0;
    bool       _scanning    = false;

    WiFiNetworkCallback _onNetworkFound;
};
