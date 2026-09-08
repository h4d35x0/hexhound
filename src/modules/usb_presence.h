#pragma once
#include <stdint.h>

// ── HexHound - USB host presence ─────────────────────────────────
//
// "Is a USB host there?" has THREE answers on this project, not two, because
// the two build shapes can answer it to different degrees:
//
//   HID images  (USB_MODE_HID): TinyUSB owns the port and USBHID::ready() is a
//               live, cheap read of the endpoint state. A real measurement.
//   CDC images  (no USB_MODE_HID): there is no HID endpoint to ask, and this
//               firmware asks nothing else. No measurement exists.
//
// A bool cannot carry "no measurement exists", and that is exactly how
// USBModule::update() came to hardcode `_connected = true`: the only two values
// available were a claim of presence and a claim of absence, and on a CDC image
// both are unfounded. Naming the third state is the fix; the debouncer below is
// what makes the HID answer usable once the state can actually change.
//
// This header deliberately depends on NOTHING - not Arduino, not TinyUSB, not
// even the module. `now` is a parameter rather than a millis() call for that
// reason: the policy is then a pure function of its inputs and a native unit
// test can drive it across a whole timeline without a USB stack. See
// test/test_usb_presence/.

enum UsbHostState : uint8_t {
    // Measured: no host. Missions cannot run and the UI should say so.
    USB_HOST_ABSENT  = 0,
    // Measured: a host is enumerated and the endpoint is live.
    USB_HOST_PRESENT = 1,
    // This build has no way to tell. NOT a synonym for either of the above,
    // and specifically not for ABSENT: a CDC board is usually plugged into a
    // computer, so reporting absence would replace one wrong answer with
    // another one that also misdirects the owner ("plug into a computer" when
    // it already is).
    USB_HOST_UNKNOWN = 2,
};

// How long the endpoint must report not-ready before the host is called gone.
//
// This exists because ready() answers a slightly different question than the
// one being asked here. tud_hid_n_ready(0) is
//
//     tud_ready() && ep_in && !usbd_edpt_busy()
//
// and only the first term is about the host. The last term goes false for the
// few milliseconds a report is in flight, so a raw read of ready() would report
// a connected host as disconnected every time this device typed anything.
//
// That is not a cosmetic flicker. update() publishes EVENT_USB_CONNECTED and
// EVENT_USB_DISCONNECTED on transitions, and PetRules subscribes to both: each
// spurious pair would feed the pet 5 hunger, add 2 trust, increment the
// persisted usbConnects counter, subtract 5 mood, and mark the save dirty. A
// busy endpoint would have paid the pet for a plug-in that never happened.
//
// So the two edges are treated asymmetrically, because the evidence is
// asymmetric:
//
//   ready() TRUE is conclusive. Nothing but an enumerated, unsuspended host
//     makes tud_ready() true, so presence is latched immediately with no
//     debounce at all - a host that appears should be usable on the next frame.
//
//   ready() FALSE is inconclusive. It means "absent, OR suspended, OR merely
//     busy for the next millisecond", and this side cannot tell which. So it
//     only counts once it has held for longer than any legitimate busy window.
//
// 1500 ms is chosen against the longest thing that can hold the endpoint in
// this module: SendReport() takes a KEY_CLEAR_TIMEOUT_MS of 50 and
// clearAllKeys() makes at most 6 of those spaced by 20 ms, so under 500 ms of
// worst-case transmit even when every attempt times out. 1500 clears that with
// room to spare and is still fast enough that unplugging the board updates the
// screen while the owner is still looking at it.
static const uint32_t USB_HOST_ABSENT_MS = 1500;

// Folds the raw endpoint answer into a stable presence state.
//
// Header-only and trivially copyable. It holds no pointer to the USB stack and
// never calls into one, so it is safe to construct at file scope alongside the
// other HID-only statics in usb_module.cpp rather than as a USBModule member -
// which also keeps six CDC images from carrying bytes in .bss for state they
// can never populate.
class UsbHostPresence {
public:
    // Call once per update() with the live endpoint answer and the current
    // millis(). Returns the state after folding `ready` in.
    UsbHostState update(bool ready, uint32_t now) {
        if (ready) {
            // Conclusive. Latch presence and cancel any pending absence.
            _present = true;
            _timing  = false;
        } else if (_present && !_timing) {
            // First inconclusive read since the host was last seen. Start the
            // clock; do NOT decide anything yet.
            _timing          = true;
            _notReadySinceMs = now;
        } else if (_present && (uint32_t)(now - _notReadySinceMs) >= USB_HOST_ABSENT_MS) {
            // Not-ready has now held longer than any transmit can explain.
            // Unsigned subtraction, so this stays correct across the millis()
            // wrap at 49.7 days.
            _present = false;
            _timing  = false;
        }
        return state();
    }

    // Starts ABSENT and stays there until the first conclusive read, so a board
    // that boots on battery never reports a host it has not seen, and no
    // spurious "disconnected" is published before the first connect.
    UsbHostState state() const {
        return _present ? USB_HOST_PRESENT : USB_HOST_ABSENT;
    }

private:
    bool     _present         = false;
    bool     _timing          = false;
    uint32_t _notReadySinceMs = 0;
};
