// ── HexHound - EventBus Unit Tests ───────────────────────────────

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(test_subscribe_and_fire) {
    EventBus::instance().reset();
    int received = 0;
    EventBus::instance().subscribe(EVENT_TIMER_TICK, [&](const Event& e) {
        received++;
    });
    EventBus::instance().publish(EVENT_TIMER_TICK);
    EventBus::instance().processAll();
    ASSERT_EQ(received, 1);
}

TEST(test_multiple_subscribers) {
    EventBus::instance().reset();
    int count1 = 0, count2 = 0;
    EventBus::instance().subscribe(EVENT_WIFI_SCAN_DONE, [&](const Event& e) { count1++; });
    EventBus::instance().subscribe(EVENT_WIFI_SCAN_DONE, [&](const Event& e) { count2++; });
    EventBus::instance().publish(EVENT_WIFI_SCAN_DONE, 5);
    EventBus::instance().processAll();
    ASSERT_EQ(count1, 1);
    ASSERT_EQ(count2, 1);
}

TEST(test_ring_buffer_wrap) {
    EventBus::instance().reset();
    int received = 0;
    EventBus::instance().subscribe(EVENT_TIMER_TICK, [&](const Event& e) {
        received++;
    });

    for (int batch = 0; batch < 3; batch++) {
        for (int i = 0; i < EVENT_QUEUE_SIZE - 1; i++) {
            EventBus::instance().publish(EVENT_TIMER_TICK);
        }
        EventBus::instance().processAll();
    }
    ASSERT_EQ(received, 3 * (EVENT_QUEUE_SIZE - 1));
}

TEST(test_event_payload) {
    EventBus::instance().reset();
    int32_t receivedData = -1;
    EventBus::instance().subscribe(EVENT_STAGE_EVOLVED, [&](const Event& e) {
        receivedData = e.data;
    });
    int32_t payload = (2 << 8) | 3;
    EventBus::instance().publish(EVENT_STAGE_EVOLVED, payload);
    EventBus::instance().processAll();
    ASSERT_EQ(receivedData, payload);
    ASSERT_EQ((receivedData >> 8) & 0xFF, 2);
    ASSERT_EQ(receivedData & 0xFF, 3);
}

int main() {
    printf("\n=== EventBus Tests ===\n");

    RUN_TEST(test_subscribe_and_fire);
    RUN_TEST(test_multiple_subscribers);
    RUN_TEST(test_ring_buffer_wrap);
    RUN_TEST(test_event_payload);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
