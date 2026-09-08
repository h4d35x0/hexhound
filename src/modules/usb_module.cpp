#include "usb_module.h"
#include "../events/event_bus.h"
#include "../events/event_types.h"
#include "../config.h"

// HARDWARE_VERIFY: TinyUSB HID requires USB_MODE=0 (OTG mode) on ESP32-S3.
// With ARDUINO_USB_MODE=1 (CDC), native HID is not available.
// To enable real HID missions, rebuild with that board's HID environment:
// t-dongle-s3-hid, or lilygo-t-rgb-hid.
// Reference: docs/hardware_checklist.md

#ifdef USB_MODE_HID
#include <USB.h>
#include <USBHIDKeyboard.h>
// The keyboard is a FILE-SCOPE OBJECT, not allocated in init(), and that is
// not a style choice - see the long note beside _hidLinkObj below. In short: a
// HID interface can only be registered before TinyUSB starts, and on any image
// with CDC on boot the core has already started it before setup() runs.
//
// It is declared FIRST so that it, not _hidLinkObj, is the one that registers
// the HID device: globals in one translation unit are constructed in
// declaration order. Do not reorder these two.
static USBHIDKeyboard  _keyboardObj;
static USBHIDKeyboard* _keyboard = &_keyboardObj;

// A second handle on the SAME TinyUSB HID endpoint, kept only for its return
// value.
//
// USBHIDKeyboard::sendReport() is declared `void` and throws away the bool from
// USBHID::SendReport(), so every press(), release() and releaseAll() in the
// Arduino keyboard API is fire-and-forget. That is the hole underneath this
// whole file: the module's stated guarantee that a modifier is "ALWAYS
// released" rests on a call that returns nothing and can silently do nothing -
// the host can be unready, the report mutex can time out, tud_hid_n_report()
// can refuse, or the completion callback can never arrive.
//
// USBHID carries no per-instance state. Its constructor is guarded by a global
// "already initialized" flag, begin() only creates the two FreeRTOS handles if
// they are still NULL, and SendReport()/ready() work purely off file-scope
// globals in the framework. So a second instance is a view onto the endpoint
// the keyboard already registered, not a second device: it adds no interface,
// no descriptor and no report ID. It is constructed AFTER the keyboard in
// init() so the keyboard is the one that calls addDevice().
//
// ── WHY THESE ARE FILE-SCOPE OBJECTS AND NOT `new`ed IN init() ─────────────
//
// Because a HID interface can only be registered BEFORE TinyUSB starts, and on
// a CDC-on-boot image TinyUSB has already started by the time setup() runs.
//
// `cores/esp32/USB.h:23` defines ARDUINO_USB_ON_BOOT as the OR of the three
// ON_BOOT macros, so any image with ARDUINO_USB_CDC_ON_BOOT=1 makes it true.
// `cores/esp32/main.cpp` app_main() then calls Serial.begin() and USB.begin()
// under `#if ARDUINO_USB_ON_BOOT && !ARDUINO_USB_MODE` - BEFORE initArduino(),
// before loopTask exists, and therefore long before USBModule::init() at
// STEP 13 of setup(). The HID interface is registered by the USBHID
// constructor, and `esp32-hal-tinyusb.c:663` refuses once TinyUSB is up:
//
//     log_e("TinyUSB has already started! Interface %s not enabled")
//
// A keyboard constructed in init() on such an image therefore never exists.
// The board enumerates as a serial port and nothing else, every mission
// silently does nothing, and the ONLY report of it is a log_e that goes to the
// IDF console on UART0 - not to the CDC console you would be watching. A
// silent HID failure in the very image built to make failures visible.
//
// Declaring them here makes their constructors run in do_global_ctors(), which
// ESP-IDF calls before app_main(). The interface is registered before anything
// can start TinyUSB, so this works in BOTH configurations rather than only the
// one that happens to ship today.
//
// Declaration order is load-bearing and matches what the note above describes:
// globals in one translation unit are constructed in declaration order, so the
// keyboard is constructed first and its own USBHID member is the one that runs
// addDevice(). _hidLinkObj's constructor then finds the "already initialized"
// flag set and adds nothing.
//
// The pointers are kept so every call site below is unchanged, and because
// they are constant-initialised (the address of a static object) they are
// valid before any dynamic initialisation runs.
static USBHID          _hidLinkObj;
static USBHID* _hidLink = &_hidLinkObj;

// Tuning for clearAllKeys(). Named rather than inline because the two-of-six
// shape is the entire safety argument and a bare `2` in a loop does not carry
// it.
//
// CONFIRMATIONS is 2, not 1, on purpose: a confirmed transmission means the
// device handed the report to the host controller, not that the host processed
// it. Two identical all-zero reports, spaced further apart than any HID poll
// interval, is what makes a single lost report survivable. The report is
// idempotent, so a host that receives both is in exactly the state a host that
// receives one is in.
static constexpr uint8_t  KEY_CLEAR_ATTEMPTS      = 6;
static constexpr uint8_t  KEY_CLEAR_CONFIRMATIONS = 2;
static constexpr uint16_t KEY_CLEAR_SPACING_MS    = 20;
static constexpr uint32_t KEY_CLEAR_TIMEOUT_MS    = 50;

// How long a modifier combo stays down. Long enough for the host to see it as a
// chord, short enough that a stall while it is held is measured in tens of
// milliseconds.
static constexpr uint16_t COMBO_HOLD_MS = 40;

// How long the Windows Run dialog gets to appear and take focus after Win+R.
static constexpr uint16_t RUN_DIALOG_MS = 500;

// ── Typing cadence ────────────────────────────────────────────────────────
//
// Measured on glass, 2026-09-02, T-RGB into Notepad on Windows: with NO delay
// the Sec Checklist arrived with EVERY capital letter lowercased and two of its
// eight lines destroyed, while the device reported success and every all-clear
// confirmed. The loss is at the HOST, and nothing on this side could see it -
// SendReport() confirms a report was handed over, never that it was processed.
//
// The lowercasing is the diagnostic. USBHIDKeyboard::write() is press() then
// release() back to back with nothing between: for a shifted character the
// press report carries {modifier=SHIFT, key=X} and the release carries {0,0},
// and when both land inside one host polling window the modifier is dropped or
// the whole keypress is lost. A key that is never observably DOWN was never
// typed.
//
// So a character is now HELD for PRESS_MS and followed by GAP_MS of nothing.
// USB polls at 1 to 8 ms, so 10 each puts the two edges in separate windows.
// It costs about 20 ms per character - roughly 3.6 s for the Sec Checklist -
// and a mission that types slowly and correctly beats one that types instantly
// and lies about it.
static constexpr uint16_t TYPE_PRESS_MS = 10;
static constexpr uint16_t TYPE_GAP_MS   = 10;

// Set whenever clearAllKeys() could not confirm the all-clear, and cleared the
// moment it can. While it is set, the host may be holding a modifier that this
// device asked it to release and could not prove it did.
//
// update() retries on this flag so a host that was merely busy, suspended or
// mid-enumeration gets its keyboard back on its own. Without it the only
// recovery is a re-enumeration - which is exactly what "close the laptop lid to
// get your keyboard back" was on 2026-09-01, and the owner should never have to
// discover that trick again.
//
// File-scope rather than a member on purpose: adding two fields to USBModule
// would grow its singleton in .bss and relocate everything after it in all six
// non-HID images, for state those images can never use.
static bool     _keysUnconfirmed  = false;
static uint32_t _lastClearRetryMs = 0;

// True while the only thing _keysUnconfirmed is describing is the all-clear
// init() queued at boot, which had not been delivered yet.
//
// It exists so the log can tell two different situations apart with the same
// mechanism behind them. A clear that lands after a FAILED one is a recovery
// and worth a line that sounds like one. A clear that lands after a boot is
// the routine case that happens on every single power-up, and printing
// "Key state recovered" there would cry wolf at the operator every time the
// board starts normally - which is the fastest way to teach someone to ignore
// the one line that ever matters.
//
// Discharged by ANY confirmed clear, not just the retry in update(): if a
// mission runs first and confirms the state itself, the boot obligation is
// met and a later failure must be reported as a failure, not as a boot.
//
// File-scope like its two neighbours, and for the same reason: a member would
// grow USBModule's singleton in .bss and relocate everything after it in all
// six non-HID images, for one byte they can never use.
static bool     _bootClearPending = false;

// Retry spacing for that recovery. Deliberately unhurried: the retry only runs
// when the endpoint reports ready, so nothing is being fixed in the gaps.
static constexpr uint32_t KEY_CLEAR_RETRY_MS = 1000;

// Turns the raw endpoint answer into the stable "a host is there" that
// update() publishes events on. See usb_presence.h for why the two edges are
// not symmetric.
//
// File-scope, and inside the HID guard, for the same reason as its three
// neighbours above: a member would grow USBModule's singleton in .bss and
// relocate everything after it in all six non-HID images, for state those
// images can never populate. hostState() reads _connected, which already
// exists, so this change adds no member to the class at all.
static UsbHostPresence _presence;
#endif

// ── HexHound - USB Module Implementation ────────────────────────

const Mission USBModule::_missions[] = {
    // Original security tip missions (0-2)
    { 0, "Password Tip",
      "Tip: Use a password manager and enable 2FA on all accounts!",
      MISSION_NORMAL },
    { 1, "Phishing Alert",
      "Tip: Always verify sender email addresses before clicking links.",
      MISSION_NORMAL },
    { 2, "WiFi Safety",
      "Tip: Avoid open WiFi networks. Use a VPN on public networks.",
      MISSION_NORMAL },

    // Expanded missions (3-7)
    { 3, "WiFi Audit",
      nullptr,  // dynamic - uses last patrol data
      MISSION_NORMAL },
    { 4, "Lock Screen",
      nullptr,  // special - types the reminder, THEN sends Win+L
      MISSION_HIGH_MISCHIEF | MISSION_GUI_KEYS },
    { 5, "Sec Checklist",
      "=== SECURITY CHECKLIST ===\n"
      "[ ] Passwords unique per site\n"
      "[ ] 2FA enabled on email\n"
      "[ ] OS auto-updates ON\n"
      "[ ] Firewall enabled\n"
      "[ ] Disk encryption active\n"
      "[ ] Backup strategy tested\n"
      "=== HexHound ===",
      MISSION_NORMAL },
    { 6, "Map Link",
      nullptr,  // special - opens a map pin via Win+R
      MISSION_GUI_KEYS },
    { 7, "Assessment Note",
      ">> PHYSICAL SECURITY ASSESSMENT NOTE <<\n"
      "This workstation was accessed by an authorized USB device.\n"
      "If you did not expect this, review your physical security.\n"
      "-- HexHound //",
      MISSION_HIGH_MISCHIEF },

    // Linux/BSD demo. Types into a terminal the operator already has open, so
    // it needs no window-manager shortcut - unlike missions 4 and 6, which are
    // Win+L and Win+R and simply do nothing under i3/openbox/most tiling WMs.
    { 8, "Term Tip",
      nullptr,  // special - see executeTerminalTip()
      MISSION_NORMAL },
};

USBModule& USBModule::instance() {
    static USBModule mod;
    return mod;
}

void USBModule::init() {
#ifdef USB_MODE_HID
    // Both objects already exist and have already registered the HID interface:
    // they are file-scope, so their constructors ran in do_global_ctors() before
    // app_main(). Constructing them here instead would be too late on any image
    // with CDC on boot. See the note beside their definitions.
    //
    // USB.begin() is still called, and is safe to call twice: ESPUSB::begin() is
    // guarded by its own _started flag and tinyusb_init() returns ESP_OK early if
    // it is already up. On a CDC-on-boot image the core called it first and this
    // is a no-op; on the CDC-off HID images this is the only call and it is what
    // starts TinyUSB.
    USB.begin();
    _keyboard->begin();
    // Idempotent - it finds the semaphores the keyboard already created and
    // leaves them alone.
    _hidLink->begin();

    // Tell the host nothing is held, as early as this device is able to.
    //
    // This closes the one hole clearAllKeys() cannot cover from inside a
    // mission. The T-RGB was ALSO cycling on USB during the 2026-09-01
    // incident, which means it was resetting. If the chip resets while a
    // modifier is down, no code on the device can send the release: the
    // report state, the module, the whole heap are gone, while the HOST still
    // believes GUI is held because the last report it ever received said so.
    // Nothing in the previous fix runs in that window, because everything in
    // it hangs off a mission and the reset happened between missions. The
    // host stays broken until something re-enumerates it - which is exactly
    // the lid-close the owner had to discover for himself.
    //
    // ready() is the discriminator between the two ways this can fail to send,
    // and it is why there is no "quiet" flag anywhere here:
    //
    //   ready() false - the host has not enumerated us. USB.begin() returned
    //     microseconds ago and enumeration takes hundreds of milliseconds, so
    //     this is the normal case on EVERY boot, not a fault. Say nothing, arm
    //     the retry, let update() deliver it the moment the host appears.
    //     Calling clearAllKeys() here anyway would burn six SendReport()
    //     attempts that cannot succeed and print an alarming "NOT confirmed
    //     clear (0/2)" on every single power-up.
    //
    //   ready() true - the endpoint is mounted, not suspended and not busy, so
    //     a report should go out. If clearAllKeys() still cannot confirm one,
    //     that IS worrying and its existing error line is the right output.
    //
    // Safe to call before enumeration: USBHID::ready() is tud_hid_n_ready(0),
    // which is tud_ready() && the instance's ep_in && !usbd_edpt_busy(). It
    // takes no lock, has no tud_inited() assert, and tud_mounted() is a read
    // of _usbd_dev.cfg_num that is simply 0 until the host sets a configuration
    // - so it short-circuits to false before touching the endpoint state. It
    // cannot block and it cannot fault here.
    //
    // Back-dating the retry stamp matters. Leaving it at 0 would make the
    // first retry ineligible until uptime passed KEY_CLEAR_RETRY_MS, and a
    // host holding a modifier should not wait an extra second for the release
    // once the endpoint is finally up. The subtraction is deliberate unsigned
    // wraparound and the comparison in update() is a difference, so it stays
    // correct even when millis() is smaller than the interval at this point.
    _keysUnconfirmed  = true;
    _bootClearPending = true;
    _lastClearRetryMs = millis() - KEY_CLEAR_RETRY_MS;
    if (_hidLink->ready() && clearAllKeys()) {
        Serial.println("[USB] Boot all-clear confirmed");
    }

    Serial.println("[USB] HID keyboard initialized");
#else
    // This line names ONE env and there is now one per board, so on five of
    // the six CDC boards it points at the wrong build. Left as it is ON
    // PURPOSE: rewording it changes the length of a string in a translation
    // unit that links early, which shifts every rodata address after it and
    // relocates the literal pools that reference them. Measured on this
    // change: 130044 bytes differ across 17924 runs in the T-Display S3 image,
    // for four characters. That is a real, unreviewable image churn on six
    // shipping boards in exchange for a developer-only serial line that the
    // on-screen alert has now made redundant.
    //
    // If someone applies the main.cpp mission alert (LANE_B_CALLSITE.md), that
    // image pays the same relocation cost anyway and this can ride along with
    // it for free. Fix it there or not at all.
    Serial.println("[USB] HID disabled (CDC mode) - use t-dongle-s3-hid env for missions");
#endif
}

void USBModule::update() {
    // ── Is a USB host actually there? ─────────────────────────────────────
    //
    // This used to be an unconditional `_connected = true` with a
    // HARDWARE_VERIFY note attached, which made isConnected() and the
    // `if (!_connected)` guard in executeMission() both dead, and made the
    // "USB host not connected" alert in main.cpp unreachable. See
    // docs/open-work.md items E and J.
    //
    // The two build shapes get different answers because they can support
    // different answers. Neither is a guess:
    //
    //   HID: _hidLink->ready() is tud_ready() && ep_in && !usbd_edpt_busy(),
    //        so a true is conclusive evidence of an enumerated, unsuspended
    //        host. It is safe to call before enumeration - see the long note in
    //        init() - and it is already trusted for exactly this purpose by the
    //        all-clear retry a few lines below. The only trap is that a false
    //        also covers "busy for the next millisecond", which is why it goes
    //        through UsbHostPresence rather than straight into _connected. That
    //        debounce is not a nicety: see the event note below.
    //
    //   CDC: nothing to ask. USB_MODE_HID is off, TinyUSB is not the stack, and
    //        this firmware reads no other host-presence signal. The state is
    //        USB_HOST_UNKNOWN, which hostState() returns as a compile-time
    //        constant, and _connected stays true because "unknown" must not be
    //        published as "absent" - a CDC board is normally plugged in, and
    //        the truthful thing to tell its owner is USB_MISSION_FAILED_MSG
    //        ("No USB keyboard in this build"), not "plug into a computer".
    //        See the isConnected() note in usb_module.h for why the alternative
    //        was rejected.
    //
    // WHAT CHANGES NOW THAT THIS CAN MOVE. Both events below have been
    // unreachable-in-practice since they were written: CONNECTED fired exactly
    // once per boot and DISCONNECTED never fired at all. PetRules subscribes to
    // both (pet_rules.cpp), so on a HID image a real unplug now costs 5 mood for
    // the first time ever, and a real plug-in pays 5 hunger, 2 trust and one
    // usbConnects instead of that being handed over for free at boot. That is
    // the behaviour those handlers were always written for; it had simply never
    // been reachable. It is also why the falling edge is debounced - without it
    // every typed character would have paid the pet for a plug-in cycle.
    //
    // A CDC image is unaffected: _connected is true from the first update, so
    // CONNECTED still fires once at boot and usbConnects still reaches
    // EGG_USB_CONNECTS. Hatching on those six boards is untouched.
#ifdef USB_MODE_HID
    _connected = (_presence.update(_hidLink && _hidLink->ready(), millis())
                  == USB_HOST_PRESENT);
#else
    _connected = true;  // USB_HOST_UNKNOWN - see hostState()
#endif

    if (_connected && !_wasConnected) {
        EventBus::instance().publish(EVENT_USB_CONNECTED);
        Serial.println("[USB] Host connected");
    } else if (!_connected && _wasConnected) {
        EventBus::instance().publish(EVENT_USB_DISCONNECTED);
        Serial.println("[USB] Host disconnected");
    }
    _wasConnected = _connected;

#ifdef USB_MODE_HID
    // Keep trying to hand back a keyboard we could not confirm we released.
    //
    // ready() is checked FIRST and it does not block - it is a straight read of
    // the TinyUSB endpoint state. That matters: clearAllKeys() can spend most
    // of half a second failing, and calling it once a second at an absent host
    // would stall the UI loop on a battery board with nothing plugged in. When
    // the endpoint is ready the clear costs about 20 ms and normally succeeds
    // on the first two sends.
    if (_keysUnconfirmed && _hidLink && _hidLink->ready() &&
        (millis() - _lastClearRetryMs) >= KEY_CLEAR_RETRY_MS) {
        _lastClearRetryMs = millis();
        // Read before the clear, which is what discharges the flag.
        const bool wasBoot = _bootClearPending;
        if (clearAllKeys()) {
            Serial.println(wasBoot
                ? "[USB] Boot all-clear confirmed"
                : "[USB] Key state recovered - host keyboard released");
        }
    }
#endif
}

bool USBModule::executeMission(uint8_t missionIndex, GuiKeyConsent consent) {
    if (missionIndex >= MAX_MISSIONS) return false;

    // A mission that presses the host's GUI modifier reaches outside the
    // focused window, so it runs only if the owner was asked on the glass and
    // said yes. The asking is UIMissions::longPressShouldExecute(); this is the
    // backstop that makes skipping it a refusal rather than a silent
    // regression. Checked before the connection guard below because consent is
    // a property of the request, not of the hardware.
    if ((_missions[missionIndex].flags & MISSION_GUI_KEYS) &&
        consent != GuiKeyConsent::ConfirmedOnGlass) {
        Serial.println("[USB] Refused: GUI-keys mission with no confirmation");
        return false;
    }
    // Refuse only on a MEASURED absence. USB_HOST_UNKNOWN must not refuse here:
    // on a CDC image every mission would then fail this guard instead of the
    // #ifndef below, and the caller would still show USB_MISSION_FAILED_MSG, so
    // the only effect would be to route the CDC case through a check that never
    // examined the build. Worse, it would leave a guard that reads like a
    // runtime host test but is really a compile-time one. On a HID image this
    // is now a live answer for the first time; before this it was `!_connected`
    // against a hardcoded true and could never fire.
    if (hostState() == USB_HOST_ABSENT) return false;
#ifndef USB_MODE_HID
    Serial.println("[USB] HID not available in CDC mode");
    return false;
#else
    const Mission& m = _missions[missionIndex];
    Serial.printf("[USB] Executing mission: %s\n", m.name);

    // Nothing may be held when a mission starts, and "nothing is held" has to
    // be confirmed rather than assumed. A modifier left over from a previous
    // run, or from anything else that touched the endpoint, would turn this
    // mission's plain text into a run of shortcuts.
    //
    // Refusing here is the safe outcome, not a degraded one: the caller shows
    // USB_MISSION_FAILED_MSG and no keystrokes reach the host.
    //
    // Note what this does NOT do: it does not consult _keysUnconfirmed. That
    // flag is a retry ticket for update(), never a gate. init() now sets it on
    // every boot, and a mission selected before the host has enumerated would
    // otherwise refuse for a reason that has nothing to do with this host's
    // keyboard. The question asked here is always the live one - can a report
    // go out right now - and the answer is taken fresh.
    if (!clearAllKeys()) {
        Serial.println("[USB] Refusing mission: key state not confirmed clear");
        return false;
    }

    // Every mission that presses a modifier reports whether it ended clean.
    // The plain-typing missions cannot leave a modifier of their own, but they
    // still pass through the confirmed clear at the end.
    bool ok = true;

    switch (missionIndex) {
    case 3:
        // WiFi Audit Report - type last patrol summary
        if (strlen(_lastPatrolSummary) > 0) {
            typeString("=== WIFI AUDIT REPORT ===");
            typeMultiLine(_lastPatrolSummary);
            typeString("=== HexHound ===");
        } else {
            typeString("WiFi Audit: No patrol data available. Run a patrol first.");
        }
        break;

    case 4:
        // Lock Screen - type the message, THEN Win+L
        ok = executeLockScreen();
        break;

    case 6:
        // Open a map pin via Win+R.
        //
        // The URL is typed into the host's Run dialog a character at a
        // time, so it stays short and uses the plain ?q= form rather than
        // the longer /maps/search/?api=1&query= one: every extra character
        // is another keystroke that can be dropped if the host is busy.
        // Commas and spaces are percent-safe here because Windows hands the
        // whole string to the default browser rather than parsing it.
        ok = executeOpenURL("https://maps.google.com/?q=3284+Northside+Parkway+NW,+Atlanta,+GA");
        break;

    case 8:
        // Linux terminal tip - plain typing, no shortcuts
        executeTerminalTip();
        break;

    default:
        // Standard text payload
        if (m.payload) {
            if (strchr(m.payload, '\n')) {
                typeMultiLine(m.payload);
            } else {
                typeString(m.payload);
            }
        }
        break;
    }

    // Never leave a key or modifier held once a mission ends, however it ended,
    // and never claim it ended clean unless that was confirmed. This also
    // covers the plain-typing missions: print() sets the SHIFT modifier for
    // every capital letter, so "HexHound" presses and releases a modifier too.
    if (!clearAllKeys()) {
        ok = false;
    }

    if (!ok) {
        // Deliberately no EVENT_MISSION_COMPLETE and no XP. A mission that
        // could not confirm a clean keyboard did not succeed, whatever it
        // managed to type first.
        Serial.printf("[USB] Mission %u did not complete cleanly\n", m.id);
        return false;
    }

    EventBus::instance().publish(EVENT_MISSION_COMPLETE, m.id);
    return true;
#endif // USB_MODE_HID
}

const Mission& USBModule::getMission(uint8_t idx) const {
    // The bound is deduced from the initialiser list above (see usb_module.h),
    // so this is the check that a row was not quietly left out.
    static_assert(sizeof(_missions) / sizeof(_missions[0]) == MAX_MISSIONS,
                  "mission table size must match MAX_MISSIONS");
    if (idx >= MAX_MISSIONS) idx = 0;
    return _missions[idx];
}

void USBModule::setLastPatrolSummary(const char* summary) {
    strlcpy(_lastPatrolSummary, summary, sizeof(_lastPatrolSummary));
}

#ifdef USB_MODE_HID
// One character, with the host given time to see both edges of it.
//
// Deliberately NOT _keyboard->write(), which is press() and release() with
// nothing in between. See the TYPE_PRESS_MS note above: that is what lost
// every capital letter in the 2026-09-02 bench run.
//
// A carriage return is skipped rather than typed, matching what the Arduino
// keyboard's own multi-byte write does, so a CRLF payload cannot produce a
// blank line on the host.
void USBModule::typeChar(char c) {
    if (c == '\r') {
        return;
    }
    if (_keyboard->press((uint8_t)c) == 0) {
        // Not a character this layout can produce. Nothing went down, so
        // there is nothing to release - releasing anyway would send a report
        // describing a key the host was never told about.
        return;
    }
    delay(TYPE_PRESS_MS);
    _keyboard->release((uint8_t)c);
    delay(TYPE_GAP_MS);
}

// Every typing path in this module goes through here. There is no remaining
// call to _keyboard->print(): print() types at full speed and is exactly what
// dropped characters on the bench.
void USBModule::typePaced(const char* str) {
    if (!str) return;
    for (const char* p = str; *p; p++) {
        typeChar(*p);
    }
}

void USBModule::typeString(const char* str) {
    typePaced(str);
    typeChar('\n');
    delay(50);
}

void USBModule::typeMultiLine(const char* str) {
    const char* p = str;
    while (*p) {
        const char* lineEnd = strchr(p, '\n');
        if (lineEnd) {
            while (p < lineEnd) {
                typeChar(*p);
                p++;
            }
            typeChar('\n');
            p++;
            delay(30);
        } else {
            typePaced(p);
            typeChar('\n');
            break;
        }
    }
    delay(50);
}

// Put the host's keyboard back to "nothing held", and prove it went out.
//
// WHY THIS EXISTS, and why the previous version of pressCombo() was not enough.
//
// On 2026-09-01 a T-RGB running the HID demo opened pinned taskbar apps
// repeatedly and left the owner's keyboard and mouse unusable until a lid-close
// re-enumerated USB. The URL that mission 6 types contains "3284", and with the
// GUI modifier held those four characters are Win+3, Win+2, Win+8 and Win+4 -
// four pinned apps. So the host had GUI held while it was receiving the URL.
// The exact mechanism has NOT been reproduced on hardware and this comment does
// not claim one.
//
// What IS certain from the framework source is that the old guard could not
// have detected it. It ended with:
//
//     _keyboard->releaseAll(); delay(40); _keyboard->releaseAll();
//
// described as "a second release costs nothing and covers a dropped first
// report". It covers nothing. releaseAll() zeroes the library's local
// _keyReport and calls sendReport(), which is `void` and discards the result of
// USBHID::SendReport(). If the host was not ready, if the report mutex timed
// out, or if the completion callback never came, BOTH calls returned normally
// having transmitted nothing, and the device then believed the keyboard was
// clear. Nothing afterwards re-asserts it, because the local state already says
// clear.
//
// So this function does the two things the old one could not:
//   1. It builds an explicit all-zero report and sends it through a handle that
//      RETURNS whether the stack transmitted it, instead of trusting a void
//      call. An unconfirmed clear is now a visible failure rather than silence.
//   2. It requires two confirmed transmissions, spaced further apart than a HID
//      poll interval, so a single report lost on the wire is not the last word.
//
// And the callers do the third thing, which is the one that actually protects
// the owner: if this returns false, NOTHING GETS TYPED. A fix that only worked
// when reports were delivered would fix nothing, so the failure path is the
// point. The mission reports failure, the UI already says "Mission failed.
// Unplug and replug, then retry.", and no keystrokes reach an unknown host.
bool USBModule::clearAllKeys() {
    if (!_keyboard || !_hidLink) {
        return false;
    }

    // Keep the library's own idea of the key state in step with what is about
    // to be asserted on the wire. Without this, _keyReport could still hold a
    // keycode, and the next release() would send a report describing a key the
    // host was never told about.
    _keyboard->releaseAll();

    hid_keyboard_report_t zero;
    memset(&zero, 0, sizeof(zero));

    uint8_t confirmed = 0;
    for (uint8_t i = 0; i < KEY_CLEAR_ATTEMPTS && confirmed < KEY_CLEAR_CONFIRMATIONS; i++) {
        if (i) {
            delay(KEY_CLEAR_SPACING_MS);
        }
        if (_hidLink->SendReport(HID_REPORT_ID_KEYBOARD, &zero, sizeof(zero),
                                 KEY_CLEAR_TIMEOUT_MS)) {
            confirmed++;
        }
    }

    if (confirmed < KEY_CLEAR_CONFIRMATIONS) {
        // update() will keep retrying from here until the host takes it.
        _keysUnconfirmed = true;
        Serial.printf("[USB] Key state NOT confirmed clear (%u/%u)\n",
                      confirmed, KEY_CLEAR_CONFIRMATIONS);
        return false;
    }
    _keysUnconfirmed  = false;
    _bootClearPending = false;
    return true;
}

// Hold a modifier combo for the shortest time that still registers, and ALWAYS
// release it - or report that the release could not be confirmed, so the caller
// types nothing. A modifier left held is not a cosmetic bug: the host treats
// every later keypress as a shortcut, and the user's keyboard stays broken
// until the device re-enumerates (unplug, or sleep/wake). Anything that presses
// a modifier must go through here.
//
// The leading clear is not redundant with the caller's. A combo must never be
// built on top of a key state nobody has confirmed, because press() ORs into
// whatever _keyReport already holds.
bool USBModule::pressCombo(uint8_t modifier, uint8_t key) {
    if (!clearAllKeys()) {
        return false;
    }

    _keyboard->press(modifier);
    _keyboard->press(key);
    delay(COMBO_HOLD_MS);

    return clearAllKeys();
}

// Types straight into whatever terminal is already focused. No modifiers, no
// window-manager shortcuts, so it behaves the same on Kali, Parrot, a bare tty
// or macOS. Everything it types is an `echo`, so the worst case is text on
// screen - nothing is installed, downloaded or persisted.
void USBModule::executeTerminalTip() {
    // QUOTED, and that is not cosmetic. PowerShell's `echo` is Write-Output,
    // which takes each unquoted word as a separate argument and prints one per
    // line - so on 2026-09-02 "echo HexHound was here" arrived as three lines.
    // Quoting renders identically in bash, cmd.exe and PowerShell. The comment
    // above says this mission targets Kali, Parrot, a bare tty or macOS;
    // PowerShell was never considered and is where it actually got run.
    static const char* const lines[] = {
        "echo \"---------------------------------------\"",
        "echo \"HexHound was here\"",
        "echo \"a keyboard you did not plug in can type\"",
        "echo \"lock your screen and trust no usb device\"",
        "echo \"---------------------------------------\"",
    };

    for (const char* line : lines) {
        typePaced(line);
        typeChar('\n');
        // Give the shell time to echo the line back before the next one; a
        // terminal that is still redrawing can drop characters.
        delay(120);
    }
    _keyboard->releaseAll();
}

bool USBModule::executeLockScreen() {
    // Order matters: type the reminder FIRST, then lock.
    //
    // The reverse order sent the text into the Windows lock screen, where the
    // only focused control is the password box - so the reminder was never
    // visible, and the trailing Return submitted it as a failed logon. Every
    // run cost the user one failed attempt against their lockout policy and
    // wrote an Event ID 4625, which on a monitored network looks exactly like
    // a USB device trying to brute-force credentials.
    //
    // No trailing Return here either: this types into whatever window happens
    // to hold focus, and pressing Enter in an unknown window is the same class
    // of problem. Print the text and leave submitting it to the human.
    //
    // The text is typed BEFORE the combo, so the ordering that protects the
    // typing run is the reverse of mission 6's: here the combo is last and
    // nothing follows it. That is why this one only needs a confirmed state to
    // start from, and a confirmed release to finish with.
    if (!clearAllKeys()) {
        return false;
    }
    typePaced("HexHound says: lock your screen.");
    delay(500);
    return pressCombo(KEY_LEFT_GUI, 'l');
}

bool USBModule::executeOpenURL(const char* url) {
    // This is the sequence that caused the 2026-09-01 incident, and the two
    // guards below are the fix. Neither is optional.
    if (!pressCombo(KEY_LEFT_GUI, 'r')) {
        // The GUI release could not be confirmed, so GUI may still be held on
        // the host. Typing the URL now is precisely the failure being fixed:
        // with GUI held, "3284" in the address is Win+3, Win+2, Win+8, Win+4,
        // and each one launches a pinned taskbar app. Type nothing.
        Serial.println("[USB] Aborting Map Link: GUI release not confirmed");
        return false;
    }

    delay(RUN_DIALOG_MS);

    // Re-assert immediately before the typing run rather than relying on the
    // clear that happened half a second ago. That gap is long enough for a
    // suspend, a resume, or a re-enumeration, and the whole point of this
    // change is that the state before a long typing run is checked rather than
    // assumed.
    if (!clearAllKeys()) {
        Serial.println("[USB] Aborting Map Link: key state unconfirmed before typing");
        return false;
    }

    // Paced, and this is the call site where it matters most. A dropped
    // character here does not garble a message, it changes the DESTINATION: the
    // Run dialog hands whatever it ends up holding to the default browser, so a
    // lost character in the host part of the URL navigates somewhere nobody
    // chose. The 2026-09-02 bench run destroyed two whole lines of a checklist
    // at full speed, which is well past the loss rate a URL can absorb.
    typePaced(url);
    delay(100);
    typeChar('\n');
    delay(50);
    return clearAllKeys();
}
#else
// Unreferenced in a CDC build - executeMission() returns before any of them can
// be called - so -ffunction-sections plus --gc-sections drops them and the six
// non-HID images do not move. The signatures still have to match the header.
bool USBModule::clearAllKeys() { return false; }
void USBModule::typeString(const char*) {}
void USBModule::typeMultiLine(const char*) {}
bool USBModule::pressCombo(uint8_t, uint8_t) { return false; }
void USBModule::executeTerminalTip() {}
bool USBModule::executeLockScreen() { return false; }
bool USBModule::executeOpenURL(const char*) { return false; }
#endif
