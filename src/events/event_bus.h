#pragma once
#include <functional>
#include "event_types.h"

// ── HexHound - Pub/Sub Event Bus ─────────────────────────────────

#define MAX_SUBSCRIBERS   8
#define EVENT_QUEUE_SIZE  32

using EventHandler = std::function<void(const Event&)>;

struct Subscription {
    EventType   type;
    EventHandler handler;
    bool         active;
};

class EventBus {
public:
    static EventBus& instance();

    // Subscribe a handler to a specific event type
    bool subscribe(EventType type, EventHandler handler);

    // Publish an event (queued for next processAll)
    void publish(EventType type, int32_t data = 0);

    // Process all queued events - call once per loop iteration
    void processAll();

    // Direct immediate dispatch (bypass queue)
    void dispatch(const Event& evt);

    // Reset all state (for testing)
    void reset();

private:
    EventBus() = default;

    Subscription _subs[MAX_SUBSCRIBERS * EVENT_TYPE_COUNT];
    int          _subCount = 0;

    Event        _queue[EVENT_QUEUE_SIZE];
    int          _queueHead = 0;
    int          _queueTail = 0;
};
