#include "wifi_module.h"
#include "../events/event_bus.h"
#include "../events/event_types.h"
#include "../config.h"
#include <WiFi.h>

// ── HexHound - Wi-Fi Module Implementation ──────────────────────

WiFiModule& WiFiModule::instance() {
    static WiFiModule mod;
    return mod;
}

void WiFiModule::init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.println("[WiFi] Initialized in STA mode");
}

bool WiFiModule::startScan() {
    if (_scanning) return false;

    _resultCount = 0;
    _openCount   = 0;
    _dupeCount   = 0;
    memset(_results, 0, sizeof(_results));

    // Start async scan (show hidden=false, passive=false, max_ms, channel=0=all)
    int16_t rc = WiFi.scanNetworks(true, false, false, 300);
    if (rc == WIFI_SCAN_RUNNING) {
        _scanning = true;
        Serial.println("[WiFi] Scan started");
        return true;
    }
    Serial.printf("[WiFi] Scan start failed: %d\n", rc);
    return false;
}

bool WiFiModule::pollScan() {
    if (!_scanning) return false;

    int16_t status = WiFi.scanComplete();
    if (status == WIFI_SCAN_RUNNING) return false;

    _scanning = false;

    if (status < 0) {
        Serial.printf("[WiFi] Scan error: %d\n", status);
        return true;
    }

    _resultCount = min((int)status, MAX_SSIDS);

    // Process each network and populate results
    for (int i = 0; i < _resultCount; i++) {
        strlcpy(_results[i].ssid, WiFi.SSID(i).c_str(), sizeof(_results[i].ssid));
        strlcpy(_results[i].bssid, WiFi.BSSIDstr(i).c_str(), sizeof(_results[i].bssid));
        _results[i].rssi    = (int8_t)WiFi.RSSI(i);
        _results[i].channel = WiFi.channel(i);
        _results[i].encType = WiFi.encryptionType(i);
        _results[i].isDuplicate = false;
        _results[i].isOpen  = (_results[i].encType == WIFI_AUTH_OPEN);
        _results[i].isHidden = (strlen(_results[i].ssid) == 0);
    }

    analyzeResults();

    // Fire per-network callback for live HUD updates
    if (_onNetworkFound) {
        for (int i = 0; i < _resultCount; i++) {
            _onNetworkFound(_results[i].ssid,
                            _results[i].isDuplicate,
                            _results[i].isOpen,
                            _results[i].channel);
        }
    }

    // Publish aggregate events
    auto& bus = EventBus::instance();
    bus.publish(EVENT_WIFI_SCAN_DONE, _resultCount);

    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isDuplicate) {
            bus.publish(EVENT_WIFI_DUPLICATE_SSID, i);
        }
        if (_results[i].isOpen) {
            bus.publish(EVENT_WIFI_OPEN_NETWORK, i);
        }
    }

    WiFi.scanDelete();
    Serial.printf("[WiFi] Scan done: %d nets, %d open, %d dupes\n",
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
