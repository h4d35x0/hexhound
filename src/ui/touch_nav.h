#pragma once

#include <stdint.h>

// ── HexHound - Touch Navigation Primitives ──────────────────────
//
// The pure logic behind touch navigation: a gesture recognizer, and the row hit
// test that turns a gesture's coordinates into a list index.
//
// The recognizer turns a stream of polled touch samples into the handful of
// gestures the app needs. It deliberately knows NOTHING about the controller,
// the panel geometry, or the screens: main.cpp feeds it samples and maps what
// comes out onto the same short-press / long-press / back actions the physical
// button already drives.
//
// That split is the whole design. Touch is an INPUT SOURCE, not a per-screen
// feature, so none of the ~16 screens has to become touch-aware and the button
// keeps behaving exactly as it always has. It is also the only reason any of
// this is testable: the desktop simulator has no touch panel (TouchModule
// reports unavailable under SIMULATOR_BUILD) and the native test runner has no
// board at all, so anything entangled with the hardware read could only ever be
// verified by flashing a device and waving a finger at it.
//
// Header-only on purpose, and with no dependency on board_profile.h. Six of the
// eight boards have no touch panel and never call any of this; an inline
// function nobody calls emits no code at all, which is a stronger guarantee
// than trusting the linker to garbage-collect an unreferenced .cpp. It also
// lets the native test include this file with no board macro defined.

// ── Row hit testing ───────────────────────────────────────────────────────
// Index of the row a touch at screen y lands on, or -1 for anywhere outside the
// block. The caller passes the geometry it DREW with, so the hitbox cannot
// drift from the thing it is a hitbox for.
//
// Not in uiround, and not in uilg: it takes the geometry as arguments, so it is
// not chord-dependent and not panel-dependent. The 480x480 round T-RGB and the
// 320x172 Waveshare Touch both have a touch panel, and a copy in each layout
// family would be the same four lines twice, waiting to disagree.
//
// Degenerate geometry returns -1 rather than row 0. A menu that has never been
// drawn has no rows, and must not claim one was touched.
inline int uiRowAtY(int y, int y0, int rowH, int rows) {
    if (rowH <= 0 || rows <= 0 || y < y0) {
        return -1;
    }
    const int i = (y - y0) / rowH;
    return i < rows ? i : -1;
}

// ── Paging a list window ──────────────────────────────────────────────────
// Move the WINDOW by whole screenfuls and carry the cursor into it, which is
// what a swipe means. Returns true if anything moved, so the caller only
// repaints when there is something new to see.
//
// This is the finger's verb. A single button can only step one row at a time -
// the only way it reaches row 9 is through rows 1 to 8 - so every list here
// grew a scrollDown() shaped that way. A finger has no such constraint, and one
// row per swipe made a fourteen-entry menu take thirteen swipes to cross. The
// owner's report, in one line: "as I scroll it should move the entire menu up
// or down, not the cursor one item at a time, then when I see what I want I can
// select it."
//
// Shared rather than copied into each screen. Five lists page identically, and
// a second copy of clamping arithmetic is exactly the kind of thing that drifts
// silently - a list that pages one row short still looks like a list that
// works. Same reasoning as uiRowAtY() above.
//
// The cursor is CLAMPED into the new window rather than moved by the same
// amount, because at the ends the window travels less than a full page and a
// cursor moved in lockstep would overshoot past the last row.
//
// Deliberately does NOT wrap. scrollDown() wraps because a one-button device
// needs a way back to the top; a viewport that jumps from the end to the
// beginning under a finger just reads as having lost your place.
//
// Header-only and inline, so the boards with no touch panel emit none of it.
inline bool uiPageBy(int& cursor, int& scroll, int count, int perPage,
                     int deltaPages) {
    if (count <= 0 || perPage <= 0 || count <= perPage) {
        return false;              // everything already fits; nothing to scroll
    }

    const int maxScroll = count - perPage;
    int next = scroll + deltaPages * perPage;
    if (next < 0)         next = 0;
    if (next > maxScroll) next = maxScroll;
    if (next == scroll) {
        return false;              // already at that end
    }
    scroll = next;

    if (cursor < scroll)                cursor = scroll;
    else if (cursor >= scroll + perPage) cursor = scroll + perPage - 1;
    return true;
}

// ── The row block a list screen last drew ─────────────────────────────────
// The geometry of that block, plus the hit test over it, held by the screen
// that drew it and recorded at the one point in draw() that knows the numbers.
//
// Recording beats recomputing for the same reason the menu already does it
// that way: a hitbox derived a second time from the layout rules is a second
// copy of those rules, free to drift from the first, and it drifts SILENTLY -
// a list that opens the row above the one you touched still looks like a list
// that works.
//
// Every selectable list in this firmware draws the same shape: `count` rows of
// a uniform height `h` starting at `y0`, showing list indices from `first`.
// That holds for the round layout and for both rectangular ones, so this is
// not panel-dependent and belongs in neither uilg nor uiround.
struct RowHitBox {
    int y0    = 0;
    int h     = 0;
    int count = 0;   // rows REALLY there, never a padded draw count
    int first = 0;   // list index of the topmost drawn row

    void note(int y0_, int h_, int count_, int first_) {
        y0    = y0_;
        h     = h_;
        count = count_ < 0 ? 0 : count_;
        first = first_;
    }

    // List index a touch at screen y landed on, or -1 for the dead space above
    // or below the rows. Note that the round draw paths clamp their visible
    // count up to 1 so an EMPTY list still paints an empty-state row; that row
    // is not selectable and must never be passed in as `count`, or a tap on an
    // empty quest list would select a quest that does not exist.
    int indexAtY(int y) const {
        const int i = uiRowAtY(y, y0, h, count);
        return i < 0 ? -1 : first + i;
    }
};

namespace touchnav {

enum Gesture : uint8_t {
    GESTURE_NONE = 0,
    GESTURE_TAP,          // pressed and released in one place
    GESTURE_HOLD,         // held in one place past holdMs; fires while still down
    GESTURE_SWIPE_UP,     // finger travelled up the glass
    GESTURE_SWIPE_DOWN
};

struct GestureEvent {
    Gesture kind = GESTURE_NONE;
    // Where the contact STARTED, not where it ended. A hit test wants the row
    // the finger landed on; the last sample of a lazy tap can easily have
    // drifted into the row below it.
    int x = 0;
    int y = 0;
};

// Thresholds. Expressed as plain pixels and milliseconds so the recognizer
// stays panel-agnostic; the caller derives them from the panel it is on, since
// the two boards with a touch panel are 320x172 and 480x480 and a slop that is
// a fingertip on one is a rounding error on the other.
struct GestureConfig {
    int tapSlop        = 12;   // travel still considered "one place"
    int swipeMin       = 40;   // travel that makes it a swipe
    uint32_t holdMs    = 1000; // held this long without moving is a hold
    uint32_t minTapMs  = 40;   // shorter than this is contact bounce, not a press
    uint32_t releaseGraceMs = 60;  // see the dropped-poll note in update()
};

class GestureRecognizer {
public:
    GestureRecognizer() = default;
    explicit GestureRecognizer(const GestureConfig& cfg) : _cfg(cfg) {}

    void configure(const GestureConfig& cfg) {
        _cfg = cfg;
        reset();
    }

    // Forget any contact in progress. Used when input has to be suppressed
    // wholesale (during an expedition) so nothing is left parked to fire the
    // moment suppression lifts.
    void reset() {
        _down = false;
        _fired = false;
        _upSince = 0;
    }

    bool contactInProgress() const { return _down; }

    // Feed one poll. Returns the gesture that just completed, or GESTURE_NONE.
    GestureEvent update(bool pressed, int x, int y, uint32_t nowMs);

private:
    bool movedBeyond(int slop) const {
        int dx = _lastX - _startX;
        int dy = _lastY - _startY;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx > slop || dy > slop;
    }

    GestureConfig _cfg;
    bool     _down    = false;
    bool     _fired   = false;   // this contact already produced a hold
    uint32_t _startMs = 0;
    uint32_t _upSince = 0;       // 0 = no release pending
    int      _startX  = 0;
    int      _startY  = 0;
    int      _lastX   = 0;
    int      _lastY   = 0;
};

inline GestureEvent GestureRecognizer::update(bool pressed, int x, int y,
                                              uint32_t nowMs) {
    GestureEvent ev;

    if (pressed) {
        _upSince = 0;            // contact resumed; any pending release was noise
        if (!_down) {
            _down    = true;
            _fired   = false;
            _startX  = x;
            _startY  = y;
            _lastX   = x;
            _lastY   = y;
            _startMs = nowMs;
        }
        _lastX = x;
        _lastY = y;

        // The hold fires while the finger is still down rather than on release.
        // The request this feature exists for was "touch and hold the item in
        // the menu I want to go to", and a hold that only resolves when the
        // finger lifts feels like nothing happened.
        if (!_fired && !movedBeyond(_cfg.tapSlop) &&
            (nowMs - _startMs) >= _cfg.holdMs) {
            _fired  = true;
            ev.kind = GESTURE_HOLD;
            ev.x    = _startX;
            ev.y    = _startY;
        }
        return ev;
    }

    if (!_down) {
        return ev;
    }

    // A capacitive controller drops the occasional poll in the middle of a
    // contact, and an FT3267 polled at 25 ms drops enough of them to matter.
    // Believing the first empty read would split one press into two: a phantom
    // tap, then a second gesture out of the remainder of the same contact. So a
    // release only counts once the panel has stayed quiet for releaseGraceMs.
    if (_upSince == 0) {
        _upSince = nowMs;
        if (_upSince == 0) {
            _upSince = 1;        // 0 is the "nothing pending" sentinel
        }
    }
    if ((nowMs - _upSince) < _cfg.releaseGraceMs) {
        return ev;
    }

    const bool     fired  = _fired;
    const uint32_t heldMs = _upSince - _startMs;
    const int      dx     = _lastX - _startX;
    const int      dy     = _lastY - _startY;

    _down    = false;
    _fired   = false;
    _upSince = 0;

    if (fired) {
        return ev;               // the hold already spoke for this contact
    }
    if (heldMs < _cfg.minTapMs) {
        return ev;               // contact bounce
    }

    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;

    if (ady >= _cfg.swipeMin && ady > adx) {
        ev.kind = dy < 0 ? GESTURE_SWIPE_UP : GESTURE_SWIPE_DOWN;
        ev.x    = _startX;
        ev.y    = _startY;
        return ev;
    }

    if (adx <= _cfg.tapSlop && ady <= _cfg.tapSlop) {
        ev.kind = GESTURE_TAP;
        ev.x    = _startX;
        ev.y    = _startY;
        return ev;
    }

    // Travelled too far to be a tap, and not far enough or not straight enough
    // to be a swipe. Reporting nothing is the right answer: guessing here is
    // how a thumb dragged across the glass ends up opening a menu row.
    return ev;
}

} // namespace touchnav
