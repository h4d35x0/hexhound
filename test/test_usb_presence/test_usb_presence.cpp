// ── HexHound - USB host presence unit tests ──────────────────────
//
// Covers UsbHostPresence, the policy that turns the raw HID endpoint answer
// into the stable "a host is there" that USBModule::update() publishes
// EVENT_USB_CONNECTED / EVENT_USB_DISCONNECTED on.
//
// This is the half of "is a USB host there?" that CAN be tested off hardware.
// The other half - that _hidLink->ready() means what usb_module.cpp says it
// means - is a claim about TinyUSB and only a board can settle it.
//
// The debounce is the reason the suite exists. Every one of these transitions
// costs the pet real, persisted state through PetRules: a connect pays 5
// hunger, 2 trust and one usbConnects (a hatch requirement) and marks the save
// dirty, and a disconnect costs 5 mood. A presence signal that chatters does
// not produce a cosmetic flicker, it produces phantom plug-in rewards.

#include "../test_stubs.h"

#include "../../src/modules/usb_presence.h"

// ── Tests ──────────────────────────────────────────────────────────────────

// A board that boots on battery has never seen a host and must not claim one.
// This is also what keeps update() from publishing a DISCONNECTED before the
// first CONNECT: _wasConnected and this both start false.
TEST(test_starts_absent) {
    UsbHostPresence p;
    ASSERT_EQ((int)p.state(), (int)USB_HOST_ABSENT);
}

// ready() true is conclusive evidence, so presence latches on the same call
// with no debounce. A host that appears should be usable on the next frame.
TEST(test_ready_is_present_immediately) {
    UsbHostPresence p;
    ASSERT_EQ((int)p.update(true, 1000), (int)USB_HOST_PRESENT);
    ASSERT_EQ((int)p.state(), (int)USB_HOST_PRESENT);
}

// Never ready means never present. No amount of elapsed time invents a host,
// and the absence timer must not run when there is nothing to lose.
TEST(test_never_ready_stays_absent) {
    UsbHostPresence p;
    for (uint32_t t = 0; t < 10000; t += 250) {
        ASSERT_EQ((int)p.update(false, t), (int)USB_HOST_ABSENT);
    }
}

// THE CASE THIS CLASS EXISTS FOR. ready() is
// tud_ready() && ep_in && !usbd_edpt_busy(), so it reads false for the few
// milliseconds a report is in flight. Typing must not look like an unplug.
TEST(test_busy_endpoint_does_not_drop_presence) {
    UsbHostPresence p;
    p.update(true, 0);
    // A pessimistic stand-in for the longest transmit in usb_module.cpp:
    // clearAllKeys() is at most 6 SendReport() calls of KEY_CLEAR_TIMEOUT_MS 50
    // spaced by 20 ms, so under 500 ms even when every attempt times out.
    for (uint32_t t = 1; t <= 500; t += 10) {
        ASSERT_EQ((int)p.update(false, t), (int)USB_HOST_PRESENT);
    }
}

// A real unplug still has to be reported, and at the documented deadline.
TEST(test_sustained_not_ready_drops_at_threshold) {
    UsbHostPresence p;
    p.update(true, 1000);
    // One tick short of the threshold: still present.
    ASSERT_EQ((int)p.update(false, 1000), (int)USB_HOST_PRESENT);
    ASSERT_EQ((int)p.update(false, 1000 + USB_HOST_ABSENT_MS - 1),
              (int)USB_HOST_PRESENT);
    // At the threshold: gone.
    ASSERT_EQ((int)p.update(false, 1000 + USB_HOST_ABSENT_MS),
              (int)USB_HOST_ABSENT);
}

// The window is measured from the LAST conclusive sighting, not from the first
// not-ready read ever. A host that is intermittently busy for hours must never
// accumulate its way to a disconnect.
TEST(test_ready_cancels_pending_absence) {
    UsbHostPresence p;
    p.update(true, 0);
    for (int cycle = 0; cycle < 20; cycle++) {
        const uint32_t base = (uint32_t)cycle * 2000;
        // Almost long enough to drop...
        ASSERT_EQ((int)p.update(false, base + 1), (int)USB_HOST_PRESENT);
        ASSERT_EQ((int)p.update(false, base + USB_HOST_ABSENT_MS - 1),
                  (int)USB_HOST_PRESENT);
        // ...then one conclusive sighting, which must reset the clock.
        ASSERT_EQ((int)p.update(true, base + USB_HOST_ABSENT_MS),
                  (int)USB_HOST_PRESENT);
    }
    ASSERT_EQ((int)p.state(), (int)USB_HOST_PRESENT);
}

// The clock STARTS on the first not-ready read; it cannot also expire on it.
// Dropping a host therefore always takes two reads spanning the window, and
// this pins that down because the first draft of the suite above assumed
// otherwise and the harness caught it.
TEST(test_single_not_ready_read_never_drops) {
    UsbHostPresence p;
    p.update(true, 0);
    // Even an absurd timestamp on the FIRST not-ready read decides nothing:
    // there is no earlier not-ready reading to measure a window against.
    ASSERT_EQ((int)p.update(false, 999999), (int)USB_HOST_PRESENT);
}

// After a drop, a host that comes back is present again with no lingering
// state from the previous session. An unplug/replug cycle must be usable.
TEST(test_recovers_after_drop) {
    UsbHostPresence p;
    p.update(true, 0);
    ASSERT_EQ((int)p.update(false, 1), (int)USB_HOST_PRESENT);
    ASSERT_EQ((int)p.update(false, 1 + USB_HOST_ABSENT_MS), (int)USB_HOST_ABSENT);
    ASSERT_EQ((int)p.update(true, 1 + USB_HOST_ABSENT_MS + 10),
              (int)USB_HOST_PRESENT);
    // The absence clock must have been REARMED by that sighting, not left
    // holding the stale pre-drop timestamp. If it were stale, the elapsed time
    // on the next not-ready read would already exceed the window and the host
    // would drop again instantly, so these two reads are the real assertion.
    ASSERT_EQ((int)p.update(false, 1 + USB_HOST_ABSENT_MS + 20),
              (int)USB_HOST_PRESENT);
    ASSERT_EQ((int)p.update(false, 1 + USB_HOST_ABSENT_MS + 30),
              (int)USB_HOST_PRESENT);
}

// millis() wraps every 49.7 days and this board is meant to live on a lanyard.
// The comparison is an unsigned difference, so a window that straddles the wrap
// must behave exactly like one that does not.
TEST(test_millis_wraparound) {
    UsbHostPresence p;
    const uint32_t nearMax = 0xFFFFFFFFu - 500;
    p.update(true, nearMax);
    ASSERT_EQ((int)p.update(false, nearMax), (int)USB_HOST_PRESENT);
    // 499 ms later, still before the wrap: present.
    ASSERT_EQ((int)p.update(false, 0xFFFFFFFFu - 1), (int)USB_HOST_PRESENT);
    // Past the wrap but still inside the window: present.
    ASSERT_EQ((int)p.update(false, USB_HOST_ABSENT_MS - 502),
              (int)USB_HOST_PRESENT);
    // Past the wrap and past the window: absent.
    ASSERT_EQ((int)p.update(false, (uint32_t)(nearMax + USB_HOST_ABSENT_MS)),
              (int)USB_HOST_ABSENT);
}

int main() {
    printf("\n=== USB Host Presence Tests ===\n");

    RUN_TEST(test_starts_absent);
    RUN_TEST(test_ready_is_present_immediately);
    RUN_TEST(test_never_ready_stays_absent);
    RUN_TEST(test_busy_endpoint_does_not_drop_presence);
    RUN_TEST(test_sustained_not_ready_drops_at_threshold);
    RUN_TEST(test_ready_cancels_pending_absence);
    RUN_TEST(test_single_not_ready_read_never_drops);
    RUN_TEST(test_recovers_after_drop);
    RUN_TEST(test_millis_wraparound);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
