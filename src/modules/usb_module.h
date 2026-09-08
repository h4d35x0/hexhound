#pragma once
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

#include "usb_presence.h"

// ── HexHound - USB HID Module ────────────────────────────────────

#define MAX_MISSIONS 9

// Mission flags
#define MISSION_NORMAL        0x00
#define MISSION_HIGH_MISCHIEF 0x01  // increases mischief stat, shown with >M

// The mission presses the host's GUI (Win / Cmd / Super) modifier, so running
// it reaches OUTSIDE the focused window and changes what the owner's machine
// is doing. Missions 4 (Win+L) and 6 (Win+R) are the only two.
//
// Deliberately a SEPARATE bit from MISSION_HIGH_MISCHIEF, which means "raises
// the mischief stat" and nothing more. The two sets are not the same: mission
// 7 is high mischief and only types text, mission 6 presses Win and is not
// high mischief. Reusing that bit would have made a display flag load-bearing
// for safety.
//
// APPENDED rather than reordering the table, and that is the whole point.
// Mission::id is written into journal entries as "M%d: %s" and read back, so
// moving a row renames history that is already on owners' boards. A new bit
// cannot break a save.
#define MISSION_GUI_KEYS      0x02

// Whether the owner has confirmed a GUI-keys mission on the glass.
//
// This exists to close a SILENT failure mode. The confirm gate lives in
// UIMissions::longPressShouldExecute(), and main.cpp is under no obligation to
// call it: drop that one line and the build is green, the tests are green, and
// both Win missions run on a single hold again with nothing to notice. A gate
// that exists because a demo once took over the owner's machine should not be
// removable by deleting a line nobody would miss.
//
// So executeMission() takes this as a REQUIRED parameter with no default. A
// caller cannot omit it, and a caller that has not run the gate has nothing
// honest to pass. It is defence in depth rather than a second gate: the UI is
// still what asks the owner. This is the part that refuses if the asking was
// skipped.
enum class GuiKeyConsent : uint8_t {
    NotGiven         = 0,
    ConfirmedOnGlass = 1,
};

struct Mission {
    uint8_t id;
    const char* name;
    const char* payload;  // text to type (or action sequence)
    uint8_t flags;
};

// Why executeMission() returned false, as a compile-time string.
//
// A CDC image has no USB keyboard at all, so executeMission() logs one line to
// a serial console nobody is watching and returns false. The mission-brief
// screen used to ignore that and switch straight home, so selecting a mission
// looked like a dead button: no typing, no message, no clue. The T-Dongle S3's
// recommended image has the same silence, and it is at least documented there;
// on the T-RGB the menu row is visible with no HID build even mentioned.
//
// This is a MACRO, not a helper method, on purpose. The answer is fixed by the
// build, so a function would be an out-of-line copy in every image to return a
// constant its own compiler already knew. It also keeps the whole change at
// the call site: one string, no new symbol, nothing for the linker to place.
//
// Both strings are worded so that their FIRST 15 characters are already a
// complete, true sentence. That is not decoration: on the 480 round panel the
// alert body currently gets one 16-column line (an unscaled 240-era bound in
// ui_alert.cpp, written up in LANE_B_CALLSITE.md), and the T-RGB is the board
// this message exists for. Truncated it still reads "No USB keyboard" and
// "Mission failed.", which is the honest half of each sentence rather than a
// fragment that means nothing.
#ifdef USB_MODE_HID
#define USB_MISSION_FAILED_MSG \
    "Mission failed. Unplug and replug, then retry."
#else
#define USB_MISSION_FAILED_MSG \
    "No USB keyboard in this build. Flash the HID firmware."
#endif

class USBModule {
public:
    static USBModule& instance();

    void init();
    void update();

    // Execute a HID keyboard mission (Gremlin Mode)
    bool executeMission(uint8_t missionIndex, GuiKeyConsent consent);

    int missionCount() const { return MAX_MISSIONS; }
    const Mission& getMission(uint8_t idx) const;

    // The precise answer. Prefer this over isConnected() anywhere the third
    // state matters, which is anywhere the UI is about to TELL the owner
    // something about the USB port.
    //
    // Resolved at compile time on a CDC image: the #else branch is a constant,
    // so the whole call folds away and the six CDC images pay nothing for it.
    UsbHostState hostState() const {
#ifdef USB_MODE_HID
        return _connected ? USB_HOST_PRESENT : USB_HOST_ABSENT;
#else
        return USB_HOST_UNKNOWN;
#endif
    }

    // "Not known to be absent."
    //
    // Read that carefully, because it is NOT "a host is present" and the
    // difference is the whole point of hostState() above. On a HID image the
    // two coincide. On a CDC image this returns true while hostState() returns
    // USB_HOST_UNKNOWN, because the honest bool projection of "no measurement
    // exists" is the one that does not block and does not accuse.
    //
    // Returning false on CDC instead was considered and rejected: it would have
    // made three separate call sites state a new falsehood (the mission-brief
    // labels would read "USB NOT CONNECTED" on a board that is plugged in, the
    // config screen would answer "NO HOST CONNECTED" to Trust This Host, and
    // main.cpp would raise "USB host not connected. Plug into a computer
    // first." on a board where plugging in changes nothing), and it would have
    // stopped EVENT_USB_CONNECTED from ever firing on six of the eight boards,
    // which is the only thing that increments usbConnects, which is a hatch
    // requirement. Trading one wrong answer for three plus a dead egg is not a
    // fix. USB_MISSION_FAILED_MSG is where a CDC image tells the truth, and it
    // only gets to run because this returns true.
    bool isConnected() const { return _connected; }

    // Get last patrol summary string (for WiFi Audit Report mission)
    const char* lastPatrolSummary() const { return _lastPatrolSummary; }
    void setLastPatrolSummary(const char* summary);

private:
    USBModule() = default;
    void typeString(const char* str);
    void typeMultiLine(const char* str);

    // ── Paced typing ──────────────────────────────────────────────────────
    //
    // Every character this device sends goes through typeChar(). It presses,
    // WAITS, releases, and waits again, instead of the Arduino keyboard's
    // write(), which does press-then-release with nothing in between.
    //
    // That gap is not politeness. Measured on a T-RGB into Notepad on
    // 2026-09-02: at full speed every capital letter arrived lowercased and two
    // of eight lines were destroyed, while the device reported success and
    // every all-clear confirmed. The press and release of a shifted character
    // were landing in the same host polling window, so the modifier - or the
    // whole keypress - was never observed.
    //
    // Do not reintroduce _keyboard->print() or _keyboard->write() for payload
    // text. Both type at full speed and are exactly what dropped the
    // characters.
    void typeChar(char c);
    void typePaced(const char* str);

    // Assert, and CONFIRM, that nothing is held on the host.
    //
    // Returns true only when the USB stack reported that it actually
    // transmitted an all-zero key report. Everything that types goes through a
    // true return from this first; a false return means the keyboard state on
    // the host is UNKNOWN, and the only safe response is to type nothing and
    // fail the mission. See the long note above the definition.
    bool clearAllKeys();

    // Press a modifier+key and guarantee the release, or say that it could not.
    // Never press a modifier directly - a held modifier leaves the host's
    // keyboard unusable until the device re-enumerates.
    //
    // Returns false if the release could not be confirmed. A false return is
    // NOT advisory: the caller must not type anything afterwards.
    bool pressCombo(uint8_t modifier, uint8_t key);

    // Linux-friendly demo: types echo lines into an already-open terminal.
    void executeTerminalTip();

    // The two missions that press a modifier. Both return false rather than
    // continuing on an unconfirmed key state.
    bool executeLockScreen();
    bool executeOpenURL(const char* url);

    bool    _connected     = false;
    bool    _wasConnected  = false;
    char    _lastPatrolSummary[256] = "";

    // Declared with NO bound on purpose, so each definition's size is deduced
    // from its own initialiser list and getMission() can static_assert it
    // against MAX_MISSIONS.
    //
    // With the bound written here as [MAX_MISSIONS], a definition that supplies
    // too FEW rows is not an error: C++ value-initialises the rest, so a short
    // table silently becomes { 0, nullptr, nullptr, 0 } padding. That is not
    // hypothetical - the simulator table in hal/sim_modules.cpp sat at 8 rows
    // against a MAX_MISSIONS of 9 and turned the last row of the missions list
    // into a strlcpy() of a null name. Deducing the size turns the same mistake
    // into a build failure. A definition with too many rows was always an error
    // and still is.
    static const Mission _missions[];
};
