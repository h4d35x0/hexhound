#pragma once
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif
#include "../config.h"

// ── HexHound - Notification / RGB LED Module ─────────────────────

struct Notification {
    NotifLevel level;
    char       message[48];
    char       source[16];
    uint32_t   timestamp;
};

class NotifModule {
public:
    static NotifModule& instance();

    void init();
    void update();

    // Show a notification (sets LED + stores for UI)
    void notify(NotifLevel level, const char* message, const char* source = "");

    // Clear current notification
    void clear();

    bool hasActive() const { return _active; }
    const Notification& current() const { return _current; }

    // Direct LED control (for cutscenes)
    void setLEDColor(uint8_t r, uint8_t g, uint8_t b);
    void ledClear();

private:
    NotifModule() = default;
    void setLED(uint8_t r, uint8_t g, uint8_t b);
    void ledOff();

    Notification _current;
    bool         _active    = false;
    uint32_t     _showUntil = 0;
};
