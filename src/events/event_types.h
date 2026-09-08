#pragma once
#include <cstdint>

// ── HexHound - Event Type Definitions ────────────────────────────

enum EventType : uint16_t {
    EVENT_NONE = 0,

    // USB events
    EVENT_USB_CONNECTED,
    EVENT_USB_DISCONNECTED,

    // Wi-Fi events
    EVENT_WIFI_SCAN_DONE,        // data = network count
    EVENT_WIFI_DUPLICATE_SSID,   // data = index of duplicate
    EVENT_WIFI_OPEN_NETWORK,     // data = index of open AP

    // BLE events
    EVENT_BLE_DEVICE_FOUND,      // data = device count
    EVENT_BLE_TRACKER_ALERT,     // data = RSSI or flag

    // Button events
    EVENT_BUTTON_SHORT,
    EVENT_BUTTON_LONG,

    // Motion events (boards with an IMU - see HEXHOUND_HAS_IMU). Appended
    // after the button events on purpose: these values are not persisted, but
    // keeping the existing ones at their current ordinals avoids churning any
    // subscriber table indices on boards that never publish these.
    EVENT_SHAKE,                 // data = shake intensity (milli-g over 1g)
    EVENT_TILT_LEFT,
    EVENT_TILT_RIGHT,

    // Game events
    EVENT_MISSION_COMPLETE,      // data = mission ID
    EVENT_TIMER_TICK,            // data = tick count

    // System events
    EVENT_PET_EVOLVED,           // data = new stage (legacy notification)
    EVENT_PET_HATCHED,
    EVENT_STAGE_EVOLVED,         // data = (fromStage << 8) | toStage - triggers cutscene
    EVENT_SAVE_REQUESTED,

    EVENT_TYPE_COUNT
};

// Event payload
struct Event {
    EventType type;
    int32_t   data;
};
