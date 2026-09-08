#include "event_bus.h"
#if defined(UNIT_TEST)
// Test stubs provide Serial
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// ── HexHound - Event Bus Implementation ──────────────────────────

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

bool EventBus::subscribe(EventType type, EventHandler handler) {
    if (_subCount >= (MAX_SUBSCRIBERS * EVENT_TYPE_COUNT)) {
        Serial.println("[EventBus] subscriber limit reached");
        return false;
    }
    _subs[_subCount] = { type, handler, true };
    _subCount++;
    return true;
}

void EventBus::publish(EventType type, int32_t data) {
    int next = (_queueHead + 1) % EVENT_QUEUE_SIZE;
    if (next == _queueTail) {
        Serial.println("[EventBus] queue full, dropping event");
        return;
    }
    _queue[_queueHead] = { type, data };
    _queueHead = next;
}

void EventBus::processAll() {
    while (_queueTail != _queueHead) {
        Event evt = _queue[_queueTail];
        _queueTail = (_queueTail + 1) % EVENT_QUEUE_SIZE;
        dispatch(evt);
    }
}

void EventBus::dispatch(const Event& evt) {
    for (int i = 0; i < _subCount; i++) {
        if (_subs[i].active && _subs[i].type == evt.type) {
            _subs[i].handler(evt);
        }
    }
}

void EventBus::reset() {
    _subCount = 0;
    _queueHead = 0;
    _queueTail = 0;
}
