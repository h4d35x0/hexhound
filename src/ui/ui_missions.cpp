#include "ui_missions.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../modules/usb_module.h"
#include "../pet/pet_core.h"
#include "../config.h"
#ifdef SIMULATOR_BUILD
#include <cstdlib>   // getenv / atoi, for the capture hooks in drawBriefing()
#endif

// ── HexHound - Mission Select Implementation ──────────────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan
#define COL_CURSOR_BG 0x2945  // dark blue
#define COL_DIM       0xFFFF  // white
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white
#define COL_MISCHIEF  0xF800  // red
#define COL_HIGH_M    0xFD20  // amber - high mischief indicator

UIMissions& UIMissions::instance() {
    static UIMissions ui;
    return ui;
}

void UIMissions::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIMissions::open() {
    _cursor = 0;
    _scroll = 0;
    // Belt and braces with the disarm in drawBriefing(). Going back to the
    // list is the ordinary way to abandon a brief, and an arm that survived
    // it would be waiting the next time the brief screen came up.
    _armed  = -1;
    draw();
}

int UIMissions::totalEntries() const {
    return USBModule::instance().missionCount() + 1;  // BACK + missions
}

void UIMissions::draw() {
    if (!_tft) return;
#if HEXHOUND_PANEL_ROUND
    drawRound();
    return;
#endif

    auto& usb = USBModule::instance();
    int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);

    if (SCREEN_H > 100) {
        // ── Large-panel (320x172) layout ─────────────────────────────────
        uiBigHeader(*_tft, "\x04 MISSIONS", COL_HEADER);

        // Page indicator, drawn size 2 at top-right.
        if (total > MISSIONS_PER_PAGE) {
            int page = (_scroll / MISSIONS_PER_PAGE) + 1;
            int totalPages = ((total - 1) / MISSIONS_PER_PAGE) + 1;
            char pgBuf[8];
            snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
            int pgW = (int)strlen(pgBuf) * uilg::CHAR_W;
            _tft->setTextSize(2);
            _tft->setTextColor(COL_DIM, TFT_BLACK);
            _tft->setCursor(SCREEN_W - pgW - uilg::PAD, uilg::TITLE_Y);
            _tft->print(pgBuf);
        }

        // Rows
        const int rowH = 22;
        int by = uilg::BODY_Y;
        int bvisible = min(MISSIONS_PER_PAGE, total - _scroll);
#if HEXHOUND_HAS_TOUCH
        _rows.note(uilg::BODY_Y, rowH, bvisible, _scroll);
#endif
        for (int i = 0; i < bvisible; i++) {
            int idx = _scroll + i;
            bool isBack = (idx == 0);
            const Mission& m = usb.getMission(isBack ? 0 : idx - 1);

            _tft->setTextSize(2);
            if (idx == _cursor) {
                _tft->fillRect(0, by, SCREEN_W, rowH, COL_CURSOR_BG);
                _tft->setTextColor(COL_CURSOR, COL_CURSOR_BG);
                _tft->setCursor(uilg::PAD, by + 3);
                _tft->print("> ");

                if (isBack) {
                    _tft->print("BACK");
                } else {
                    if (m.flags & MISSION_HIGH_MISCHIEF) {
                        _tft->setTextColor(COL_HIGH_M, COL_CURSOR_BG);
                        _tft->print(">M ");
                        _tft->setTextColor(COL_CURSOR, COL_CURSOR_BG);
                    }
                    char buf[20];
                    strlcpy(buf, m.name, sizeof(buf));
                    _tft->print(buf);
                }
            } else {
                _tft->setTextColor(COL_ITEM, TFT_BLACK);
                _tft->setCursor(uilg::PAD + 2 * uilg::CHAR_W, by + 3);

                if (isBack) {
                    _tft->print("BACK");
                } else {
                    if (m.flags & MISSION_HIGH_MISCHIEF) {
                        _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
                        _tft->print(">M ");
                        _tft->setTextColor(COL_ITEM, TFT_BLACK);
                    }
                    char buf[20];
                    strlcpy(buf, m.name, sizeof(buf));
                    _tft->print(buf);
                }
            }

            by += rowH;
        }

        // Footer: mischief stat (left) + hold hint (right).
        int mischief = PetCore::instance().state().stats.mischief;
        char misBuf[12];
        snprintf(misBuf, sizeof(misBuf), "Mis:%d", mischief);
        uiBigFooter(*_tft, misBuf, "hold=go",
                    mischief > 60 ? COL_MISCHIEF : COL_FOOTER);
        return;
    }

    drawHeader();

    // Visible entries
    int y = 14;
    int visible = min(MISSIONS_PER_PAGE, total - _scroll);
#if HEXHOUND_HAS_TOUCH
    // 14 and 13 are this layout's own literals, a few lines below and at the
    // `y += 13` that closes the loop. No touch board is 160x80, so this branch
    // never runs with a panel attached; it is recorded anyway rather than left
    // as a hole for the next small touch board to fall into.
    _rows.note(14, 13, visible, _scroll);
#endif
    for (int i = 0; i < visible; i++) {
        int idx = _scroll + i;
        bool isBack = (idx == 0);
        const Mission& m = usb.getMission(isBack ? 0 : idx - 1);

        if (idx == _cursor) {
            _tft->fillRect(0, y, SCREEN_W, 12, COL_CURSOR_BG);
            _tft->setTextSize(1);
            _tft->setTextColor(COL_CURSOR, COL_CURSOR_BG);
            _tft->setCursor(4, y + 2);
            _tft->print("> ");

            if (isBack) {
                _tft->print("BACK");
            } else {
                // High mischief indicator
                if (m.flags & MISSION_HIGH_MISCHIEF) {
                    _tft->setTextColor(COL_HIGH_M, COL_CURSOR_BG);
                    _tft->print(">M ");
                    _tft->setTextColor(COL_CURSOR, COL_CURSOR_BG);
                }

                // Truncate name to fit
                char buf[20];
                strlcpy(buf, m.name, sizeof(buf));
                _tft->print(buf);
            }
        } else {
            _tft->setTextSize(1);
            _tft->setTextColor(COL_ITEM, TFT_BLACK);
            _tft->setCursor(10, y + 2);

            if (isBack) {
                _tft->print("BACK");
            } else {
                if (m.flags & MISSION_HIGH_MISCHIEF) {
                    _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
                    _tft->print(">M ");
                    _tft->setTextColor(COL_ITEM, TFT_BLACK);
                }

                char buf[20];
                strlcpy(buf, m.name, sizeof(buf));
                _tft->print(buf);
            }
        }

        y += 13;
    }

    drawFooter();
}

void UIMissions::drawHeader() {
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 MISSIONS");

    // Page indicator
    int total = totalEntries();
    if (total > MISSIONS_PER_PAGE) {
        int page = (_scroll / MISSIONS_PER_PAGE) + 1;
        int totalPages = ((total - 1) / MISSIONS_PER_PAGE) + 1;
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        char pgBuf[8];
        snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
        int pgW = strlen(pgBuf) * 6;
        _tft->setCursor(SCREEN_W - pgW - 4, 2);
        _tft->print(pgBuf);
    }

    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
}

void UIMissions::drawFooter() {
    int footerY = SCREEN_H - 10;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);

    // Mischief stat in footer
    int mischief = PetCore::instance().state().stats.mischief;
    _tft->setTextColor(mischief > 60 ? COL_MISCHIEF : COL_DIM, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->printf("Mis:%d", mischief);

    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(SCREEN_W - 48, footerY);
    _tft->print("hold=go");
}

#if HEXHOUND_HAS_TOUCH
void UIMissions::setCursor(int index) {
    const int total = totalEntries();
    if (total <= 0) return;
    if (index < 0 || index >= total) return;

    _cursor = index;

    // Same clamp scrollDown() uses. A tapped row is visible by construction, so
    // this is a no-op today; it is here so the two ways of moving the cursor
    // cannot disagree about what _scroll means if either list ever grows a
    // second entry point.
    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + MISSIONS_PER_PAGE) {
        _scroll = _cursor - MISSIONS_PER_PAGE + 1;
    }
}
#endif

#if HEXHOUND_HAS_TOUCH
// See uiPageBy() in touch_nav.h. Guarded rather than left uncalled: an
// unreferenced method IS dropped by --gc-sections, but dropping it did not
// leave the non-touch images byte-identical when this was measured on the
// menu, so the guard is the only version that can be verified.
void UIMissions::pageDown() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), MISSIONS_PER_PAGE, 1)) draw();
}

void UIMissions::pageUp() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), MISSIONS_PER_PAGE, -1)) draw();
}
#endif

void UIMissions::scrollDown() {
    int total = totalEntries();
    _cursor = (_cursor + 1) % total;

    // Adjust scroll window
    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + MISSIONS_PER_PAGE) {
        _scroll = _cursor - MISSIONS_PER_PAGE + 1;
    }

    draw();
}

// ── Mission Briefing Screen ─────────────────────────────────────────────

// Short descriptions for each mission (indexed by mission ID)
static const char* MISSION_DESC[] = {
    "Type password & 2FA tip\nvia USB keyboard.",         // 0 Password Tip
    "Type phishing awareness\ntip via USB keyboard.",     // 1 Phishing Alert
    "Type public WiFi safety\ntip via USB keyboard.",     // 2 WiFi Safety
    "Type last patrol WiFi\naudit report via USB.",       // 3 WiFi Audit
    "Type reminder, then lock\nhost screen (Win+L).",     // 4 Lock Screen
    "Type a full security\nchecklist via USB.",           // 5 Sec Checklist
    "Open a map pin via the\nRun dialog.",                // 6 Map Link
    "Type an authorized secu-\nrity assessment note.",    // 7 Assessment Note
    "Type a tip into a shell\nyou already have open.",    // 8 Term Tip
};

// The bound for the three lookups below, derived from the table and never
// restated.
//
// It used to be a literal 8, hand-copied at all three call sites, and it was
// already wrong: a ninth mission (Term Tip) was appended to
// USBModule::_missions and this array was not, so mission 8 briefed as
// "No description." on every board from the day it was added. A literal is a
// copy of a count that lives somewhere else, and a copy goes stale in silence.
//
// The static_assert is the half that matters. Deriving the bound only stops
// the out-of-range read; it would NOT have stopped this bug, because an array
// with eight entries has a perfectly good count of eight. Tying that count to
// MAX_MISSIONS is what makes a mission added without a description a compile
// error in the very build that adds it, which is the one moment somebody is
// looking at this.
constexpr int MISSION_DESC_COUNT =
    (int)(sizeof(MISSION_DESC) / sizeof(MISSION_DESC[0]));
static_assert(MISSION_DESC_COUNT == MAX_MISSIONS,
              "every mission needs a briefing line: add one to MISSION_DESC "
              "when you add a row to USBModule::_missions");

void UIMissions::drawBriefing(int missionIndex) {
    // Entering the brief screen ALWAYS starts disarmed.
    //
    // This is the whole reason the gate needs only ONE line in main.cpp. Both
    // entry points to SCREEN_MISSION_BRIEF call this, so "the owner left and
    // came back" and "the owner opened a different mission" both disarm here,
    // with nothing for the call site to remember to do. Before the early
    // return below, so it happens on every panel family.
    _armed = -1;

    if (!_tft) return;

#ifdef SIMULATOR_BUILD
    // Capture hooks, simulator only, so no board carries a byte of this.
    //
    //   HEXHOUND_SIM_BRIEF_MISSION=<n>  brief THAT mission instead of the one
    //                                   HEXHOUND_SIM_FORCE_SCREEN=brief picks
    //   HEXHOUND_SIM_BRIEF_ARMED=1      paint the ARMED confirm screen
    //
    // Both exist because the confirm screen is otherwise unreachable in the
    // simulator: the execution path runs through main.cpp, which this lane does
    // not own, so until the one-line change in LANE_B_CALLSITE.md is applied no
    // sequence of presses can arm anything. Without these the armed layout
    // would have shipped on four panel families with nobody having LOOKED at it
    // on any of them, which on this screen is not a trade worth making.
    //
    // The armed hook calls the REAL gate rather than the painter directly, so a
    // capture shows what a first hold actually produces, arming included, and
    // cannot drift away from it.
    if (const char* mi = getenv("HEXHOUND_SIM_BRIEF_MISSION")) {
        const int n = atoi(mi);
        if (n >= 0 && n < USBModule::instance().missionCount()) missionIndex = n;
    }
    if (getenv("HEXHOUND_SIM_BRIEF_ARMED")) {
        longPressShouldExecute(missionIndex);
        return;
    }
#endif

#if HEXHOUND_PANEL_ROUND
    drawBriefingRound(missionIndex);
    return;
#endif

    auto& usb = USBModule::instance();
    const Mission& m = usb.getMission(missionIndex);

    _tft->fillScreen(TFT_BLACK);

    if (SCREEN_H > 100) {
        // ── Large-panel (320x172) briefing ───────────────────────────────
        uiBigHeader(*_tft, "\x04 MISSION BRIEF", COL_HEADER);

        // Mission name, size 2.
        _tft->setTextSize(2);
        _tft->setTextColor(COL_CURSOR, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y);
        _tft->print(m.name);

        if (m.flags & MISSION_HIGH_MISCHIEF) {
            _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
            int nameW = (int)strlen(m.name) * uilg::CHAR_W;
            _tft->setCursor(uilg::PAD + nameW + uilg::CHAR_W, uilg::BODY_Y);
            _tft->print("[!M]");
        }

        // Description - word wrap at newlines, size 2, 20px per line.
        _tft->setTextColor(COL_ITEM, TFT_BLACK);
        int by = uilg::BODY_Y + 24;
        const char* bdesc = (missionIndex >= 0 && missionIndex < MISSION_DESC_COUNT) ? MISSION_DESC[missionIndex]
                                               : "No description.";
        const char* bp = bdesc;
        while (*bp && by < uilg::FOOTER_DIV - 40) {
            _tft->setCursor(uilg::PAD, by);
            while (*bp && *bp != '\n') {
                _tft->print(*bp);
                bp++;
            }
            if (*bp == '\n') bp++;
            by += 20;
        }

        // USB connection status, size 2, just above the footer.
        bool bconnected = usb.isConnected();
        _tft->setTextSize(2);
        _tft->setCursor(uilg::PAD, uilg::FOOTER_DIV - 22);
        if (bconnected) {
            _tft->setTextColor(0x07E0, TFT_BLACK);  // green
            _tft->print("USB: CONNECTED");
        } else {
            _tft->setTextColor(COL_MISCHIEF, TFT_BLACK);  // red
            _tft->print("USB: NOT CONNECTED");
        }

        uiBigFooter(*_tft, "back", bconnected ? "hold=exec" : nullptr,
                    COL_FOOTER);
        return;
    }

    // Header
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 MISSION BRIEF");
    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);

    // Mission name
    _tft->setTextColor(COL_CURSOR, TFT_BLACK);
    _tft->setCursor(4, 16);
    _tft->print(m.name);

    // High mischief warning
    if (m.flags & MISSION_HIGH_MISCHIEF) {
        _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
        int nameW = strlen(m.name) * 6;
        _tft->setCursor(4 + nameW + 6, 16);
        _tft->print("[!M]");
    }

    // Description - word wrap at newlines
    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    int y = 28;
    const char* desc = (missionIndex >= 0 && missionIndex < MISSION_DESC_COUNT) ? MISSION_DESC[missionIndex] : "No description.";
    const char* p = desc;
    while (*p && y < SCREEN_H - 24) {
        _tft->setCursor(4, y);
        while (*p && *p != '\n') {
            _tft->print(*p);
            p++;
        }
        if (*p == '\n') p++;
        y += 10;
    }

    // USB connection status
    bool connected = usb.isConnected();
    _tft->setCursor(4, SCREEN_H - 20);
    if (connected) {
        _tft->setTextColor(0x07E0, TFT_BLACK);  // green
        _tft->print("USB: CONNECTED");
    } else {
        _tft->setTextColor(COL_MISCHIEF, TFT_BLACK);  // red
        _tft->print("USB: NOT CONNECTED");
    }

    // Footer
    int footerY = SCREEN_H - 10;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print("back");
    if (connected) {
        _tft->setCursor(SCREEN_W - 54, footerY);
        _tft->print("hold=exec");
    }
}

// ── GUI-keys confirm gate ───────────────────────────────────────────────────
//
// Two of the nine missions do something the other seven do not: they press the
// host's GUI modifier, so they reach OUTSIDE the window the owner has focused
// and change what the machine is doing. Mission 4 is Win+L, mission 6 is Win+R.
// Everything else only types text into whatever already had focus.
//
// One hold used to be enough for either kind, and on the round boards a hold in
// the mission list moves the cursor AND opens the row, so a hold that lands one
// row off puts the owner on a DIFFERENT mission's brief than the one being
// looked at - and the two Win missions sit immediately under four that only
// type. See item H in docs/open-work.md.
//
// So the two Win missions now take two holds: the first ARMS and repaints the
// brief with a warning naming the actual key combination, the second runs it.
//
// Driven by the MISSION_GUI_KEYS flag, never by a row index or a mission name.
// That bit is deliberately separate from MISSION_HIGH_MISCHIEF, and the two
// sets are not the same: mission 7 is high mischief and only types, mission 6
// presses Win and is not high mischief. Gating on the mischief flag would have
// confirmed the wrong two missions and left Map Link on one hold.

namespace {

// What a GUI-keys mission actually does to the host, keyed by Mission::id.
//
// Keyed on the ID rather than the ROW for the same reason usb_module.h appends
// a flag instead of reordering the table: Mission::id is already a stable
// contract, because journal entries store "M%d: %s" and are read back. A row
// index is not. Keying this positionally would silently move the warning onto
// the wrong mission the first time a row was inserted above Lock Screen.
//
// `effect` is pre-broken into lines of at most 22 columns. That is what the
// 160x80 panel fits at text size 1 with the padding these layouts use, and it
// is the narrowest of the four families. The round layouts word-wrap anyway
// and honour the newlines, so one string serves all four.
struct GuiKeyEffect {
    uint8_t     id;
    const char* combo;   // the literal combination, for the owner to recognise
    const char* effect;  // what it does to their machine
};

const GuiKeyEffect GUI_KEY_EFFECTS[] = {
    { 4, "WIN+L", "It LOCKS the computer\nyou are plugged into." },
    { 6, "WIN+R", "It opens the Run box\nand types a web link." },
};

// Used when a mission carries MISSION_GUI_KEYS and has no row above.
//
// This degradation is the point of having a fallback at all. If a later
// mission gains the flag and nobody adds it here, the owner STILL gets the
// warning and STILL has to hold twice, because the gate is driven by the flag
// and never by this table. The only thing lost is the name of the exact
// combination. The opposite mistake, a row here for a mission that does not
// carry the flag, is simply never read.
const GuiKeyEffect GUI_KEY_UNKNOWN = {
    0, "WIN", "It reaches OUTSIDE the\nwindow you have open."
};

const GuiKeyEffect& guiKeyEffect(const Mission& m) {
    for (unsigned i = 0; i < sizeof(GUI_KEY_EFFECTS) / sizeof(GUI_KEY_EFFECTS[0]); i++) {
        if (GUI_KEY_EFFECTS[i].id == m.id) return GUI_KEY_EFFECTS[i];
    }
    return GUI_KEY_UNKNOWN;
}

// How long an armed mission stays armed.
//
// DESIGN CALL: it DOES time out, at ten seconds.
//
// The reason is the second half of the 2026-09-01 report, the half that is not
// about a stuck modifier: mission 4 fired three times in 45 seconds on the
// bench, unintentionally. This is a badge, it gets carried, and
// BUTTON_LONG_PRESS_MS is 1000 - a press against a pocket clears that bar
// easily. An arm that never expired would mean a deliberate hold now and an
// accidental hold at any later moment add up to a confirmation, which is not a
// confirmation of anything.
//
// Ten seconds is roughly three times how long the warning takes to read on the
// 160x80 panel, the smallest of the four and the one that has to say it in the
// fewest pixels. Erring SHORT is free: an expired arm cannot run anything, it
// can only re-arm, so the worst case of too short is one extra hold. Erring
// long is not free. The number is therefore taken from the cheap side.
//
// There is no countdown on the glass, deliberately. The brief screen has no
// per-frame tick and adding one would need a second call in main.cpp for a
// caller to forget. The screen says "HOLD AGAIN TO RUN", which stays true
// after the window closes - the next hold simply re-arms instead of running.
// It promises nothing about a deadline it cannot then show expiring.
constexpr uint32_t ARM_WINDOW_MS = 10000;

// The floor under that window: a confirmation must land at least this long
// after the arm.
//
// This is not about human timing, it is about wiring. If the gate is ever
// called twice for a single physical press - the obvious way to get the call
// site wrong - the second call would otherwise find the mission armed, inside
// the window, and read its own sibling as the owner's confirmation. That would
// turn one hold into a run, which is precisely the behaviour being removed.
// 300 ms cannot be reached by two calls in one loop iteration and cannot be
// reached by a bouncing button. A genuine second hold needs a release plus
// another BUTTON_LONG_PRESS_MS, so no owner will ever meet this floor.
constexpr uint32_t ARM_MIN_CONFIRM_MS = 300;

} // namespace

bool UIMissions::longPressShouldExecute(int missionIndex) {
    auto& usb = USBModule::instance();

    // Out of range: refuse, and do not try to identify it. getMission() clamps
    // a bad index to mission 0 and returns THAT, so asking it here would
    // answer about a different mission - and "mission 0 has no GUI keys" is
    // the one wrong answer that ends in something running.
    if (missionIndex < 0 || missionIndex >= usb.missionCount()) {
        _armed = -1;
        return false;
    }

    const Mission& m = usb.getMission((uint8_t)missionIndex);

    if (!(m.flags & MISSION_GUI_KEYS)) {
        // The majority path: seven of nine missions, and it is unchanged. No
        // repaint, no extra hold, no state carried. The single store clears an
        // arm left on a different mission, so moving to a text-only mission
        // cannot leave one waiting behind it.
        _armed = -1;
        return true;
    }

    if (!_tft) {
        // No display, so the warning cannot be shown, so the owner cannot have
        // consented to anything. Refuse - and do NOT arm, because arming here
        // would let a second hold run a mission whose warning was never on the
        // glass. Unreachable in a booted image; it is here so that the failure
        // mode is decided rather than discovered.
        _armed = -1;
        return false;
    }

    const uint32_t now = millis();

    if (_armed == missionIndex) {
        // Unsigned subtraction, so this is correct across the millis() wrap
        // rather than one 49-day window per boot where it is not.
        const uint32_t since = now - _armedAt;
        if (since >= ARM_MIN_CONFIRM_MS && since <= ARM_WINDOW_MS) {
            _armed = -1;   // consumed: one arm buys exactly one run
            return true;
        }
        // Too soon, or too late. Fall through and re-arm rather than run: both
        // ends of the window fail in the direction of asking again.
    }

    _armed   = missionIndex;
    _armedAt = now;
    drawBriefingArmed(missionIndex);
    return false;
}

void UIMissions::drawBriefingArmed(int missionIndex) {
    if (!_tft) return;
#if HEXHOUND_PANEL_ROUND
    drawBriefingArmedRound(missionIndex);
    return;
#endif

    const Mission& m = USBModule::instance().getMission((uint8_t)missionIndex);
    const GuiKeyEffect& e = guiKeyEffect(m);

    // Named separately from the effect so the combination is on its own line.
    // An owner who did not mean to start this has to be able to recognise it
    // at a glance, and "WIN+L" buried mid-sentence is not that.
    char sends[36];
    snprintf(sends, sizeof(sends), "Sends %s to the host.", e.combo);

    _tft->fillScreen(TFT_BLACK);

    if (SCREEN_H > 100) {
        // ── Large-panel (320x172) armed layout ───────────────────────────
        // Red header, not cyan: the ordinary brief and the armed brief must
        // not be told apart only by reading them.
        uiBigHeader(*_tft, "\x04 CONFIRM RUN", COL_MISCHIEF);

        _tft->setTextSize(2);
        _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y);
        _tft->print(m.name);

        _tft->setTextColor(COL_MISCHIEF, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y + 24);
        _tft->print(sends);

        _tft->setTextColor(COL_ITEM, TFT_BLACK);
        int by = uilg::BODY_Y + 44;
        const char* bp = e.effect;
        while (*bp && by < uilg::FOOTER_DIV - 40) {
            _tft->setCursor(uilg::PAD, by);
            while (*bp && *bp != '\n') {
                _tft->print(*bp);
                bp++;
            }
            if (*bp == '\n') bp++;
            by += 20;
        }

        _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::FOOTER_DIV - 22);
        _tft->print("HOLD AGAIN TO RUN");

        // No "hold=exec" on the right. The right-hand hint on the ordinary
        // brief is what a single hold used to promise, and leaving it here
        // would say the screen still works the old way.
        uiBigFooter(*_tft, "press=cancel", nullptr, COL_FOOTER);
        return;
    }

    // ── Compact (160x80) armed layout ────────────────────────────────────
    _tft->setTextSize(1);
    _tft->setTextColor(COL_MISCHIEF, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 CONFIRM RUN");
    _tft->drawFastHLine(0, 12, SCREEN_W, COL_MISCHIEF);

    _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
    _tft->setCursor(4, 16);
    _tft->print(m.name);

    _tft->setTextColor(COL_MISCHIEF, TFT_BLACK);
    _tft->setCursor(4, 28);
    _tft->print(sends);

    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    int y = 38;
    const char* p = e.effect;
    while (*p && y < SCREEN_H - 30) {
        _tft->setCursor(4, y);
        while (*p && *p != '\n') {
            _tft->print(*p);
            p++;
        }
        if (*p == '\n') p++;
        y += 10;
    }

    _tft->setTextColor(COL_HIGH_M, TFT_BLACK);
    _tft->setCursor(4, 58);
    _tft->print("HOLD AGAIN TO RUN");

    const int footerY = SCREEN_H - 10;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_MISCHIEF);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print("press=cancel");
}

#if HEXHOUND_PANEL_ROUND

// ── Round layouts ─────────────────────────────────────────────────────────
//
// Added when the LilyGo T-RGB became the first round board with a native USB
// port. Before it, the only round board was the Waveshare 1.28, whose USB-C is
// a UART bridge; HEXHOUND_HAS_USB_HID is 0 there and menuItemVisible() hides
// MISSIONS completely, so this screen had never been drawn on a circle. It
// fell through to the rectangular layout and rendered into the top-left corner
// with most of it behind the bezel.

// Clip `src` to maxPx pixels at charPx per glyph, ending in ".." when it did
// not fit. A name that simply stops mid-word reads as the name.
static void misFit(char* dst, size_t dstSz, const char* src, int maxPx, int charPx) {
    if (!dst || dstSz == 0) return;
    dst[0] = '\0';
    if (!src || charPx <= 0) return;

    int maxChars = maxPx / charPx;
    if (maxChars < 1) return;
    if ((size_t)maxChars > dstSz - 1) maxChars = (int)dstSz - 1;

    const int len = (int)strlen(src);
    if (len <= maxChars) { memcpy(dst, src, (size_t)len); dst[len] = '\0'; return; }
    if (maxChars <= 2)   { memcpy(dst, src, (size_t)maxChars); dst[maxChars] = '\0'; return; }
    memcpy(dst, src, (size_t)(maxChars - 2));
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
}

void UIMissions::drawRound() {
    auto& usb = USBModule::instance();
    const int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "MISSIONS", COL_HEADER);

    const int avail = uiround::FOOTER_DIV - uiround::BODY_Y;
    int visible = min(MISSIONS_PER_PAGE, total - _scroll);
    if (visible < 1) visible = 1;

    int rowH = avail / visible;
    if (rowH > uiround::scaled(30)) rowH = uiround::scaled(30);

    // Centre the block so a short list sits on the widest part of the glass
    // rather than hugging the header, matching the quests and games lists.
    int y = uiround::BODY_Y + (avail - rowH * visible) / 2;

#if HEXHOUND_HAS_TOUCH
    {
        // `visible` was clamped UP to 1 above so an EMPTY list still paints its
        // empty-state row. That row is not a list entry and must not be
        // selectable, so the hit box is told how many rows are really there.
        int real = total - _scroll;
        if (real < 0) real = 0;
        if (real > visible) real = visible;
        _rows.note(y, rowH, real, _scroll);
    }
#endif

    for (int i = 0; i < visible; i++) {
        const int idx  = _scroll + i;
        const bool sel = (idx == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;

        if (sel) uiround::rowFill(*_tft, y, rowH, COL_CURSOR_BG, uiround::scaled(4));

        const int ty = y + (rowH - uiround::textH(2)) / 2;

        if (idx == 0) {
            uiround::centerText(*_tft, ty, "BACK", 2,
                                sel ? COL_CURSOR : COL_ITEM, bg);
            y += rowH;
            continue;
        }

        const Mission& m = usb.getMission(idx - 1);
        const bool hot = (m.flags & MISSION_HIGH_MISCHIEF) != 0;

        // The rectangular layouts colour the ">M" marker separately from the
        // name. That two-tone split does not survive being centred on a chord,
        // so a high-mischief mission is named in amber with the marker kept as
        // a prefix: same warning, one colour, and it still fits the chord.
        char label[28];
        snprintf(label, sizeof(label), "%s%s", hot ? ">M " : "", m.name);

        char fitted[28];
        const int halfW = uiround::chordHalfW(ty);
        misFit(fitted, sizeof(fitted), label,
               halfW * 2 - uiround::scaled(16), uiround::cellW(2));

        const uint16_t fg = hot ? COL_HIGH_M : (sel ? COL_CURSOR : COL_ITEM);
        uiround::centerText(*_tft, ty, fitted, 2, fg, bg);

        y += rowH;
    }

    const int mischief = PetCore::instance().state().stats.mischief;
    char foot[28];
    snprintf(foot, sizeof(foot), "Mis:%d  hold=go", mischief);
    uiround::footer(*_tft, foot, mischief > 60 ? COL_HIGH_M : COL_FOOTER);
}

void UIMissions::drawBriefingRound(int missionIndex) {
    auto& usb = USBModule::instance();
    const Mission& m = usb.getMission(missionIndex);
    const bool hot = (m.flags & MISSION_HIGH_MISCHIEF) != 0;

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "BRIEF", COL_HEADER);

    char nameBuf[28];
    const int nameY = uiround::BODY_Y;
    misFit(nameBuf, sizeof(nameBuf), m.name,
           uiround::chordHalfW(nameY) * 2 - uiround::scaled(16), uiround::cellW(2));
    uiround::centerText(*_tft, nameY, nameBuf, 2,
                        hot ? COL_HIGH_M : COL_CURSOR, TFT_BLACK);

    // The USB line is the one thing that decides whether the action works, so
    // it gets a reserved row above the footer rather than being wrapped into
    // the description and possibly pushed off the glass.
    const int usbY    = uiround::FOOTER_DIV - uiround::textH(1) - uiround::scaled(6);
    const int descTop = nameY + uiround::textH(2) + uiround::scaled(6);

    const char* desc = (missionIndex >= 0 && missionIndex < MISSION_DESC_COUNT) ? MISSION_DESC[missionIndex]
                                          : "No description.";
    uiround::textBlock(*_tft, descTop, usbY - uiround::scaled(4), desc, 1,
                       COL_ITEM, TFT_BLACK, uiround::scaled(10));

    const bool connected = usb.isConnected();
    uiround::centerText(*_tft, usbY,
                        connected ? "USB CONNECTED" : "USB NOT CONNECTED", 1,
                        connected ? 0x07E0 : COL_MISCHIEF, TFT_BLACK);

    // Where the text will land. A mission types into whatever window has focus
    // on the host - it does NOT open one, deliberately, because opening an app
    // means Win+R or Win+S and that is the modifier path that made the owner's
    // keyboard unusable on 2026-09-01. Putting a Win press in front of every
    // mission to save a click would be the wrong trade.
    //
    // So the screen has to say it. Without this line the first run of any
    // mission types into whatever happened to be focused, which on a phone-sized
    // panel the owner is looking at is usually nothing useful.
    if (connected) {
        uiround::centerText(*_tft, usbY - uiround::textH(1) - uiround::scaled(3),
                            "focus a text window first", 1,
                            COL_ITEM, TFT_BLACK);
    }

    uiround::footer(*_tft, connected ? "hold=run" : "hold=back", COL_FOOTER);
}

void UIMissions::drawBriefingArmedRound(int missionIndex) {
    const Mission& m = USBModule::instance().getMission((uint8_t)missionIndex);
    const GuiKeyEffect& e = guiKeyEffect(m);

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "CONFIRM", COL_MISCHIEF);

    char nameBuf[28];
    const int nameY = uiround::BODY_Y;
    misFit(nameBuf, sizeof(nameBuf), m.name,
           uiround::chordHalfW(nameY) * 2 - uiround::scaled(16), uiround::cellW(2));
    uiround::centerText(*_tft, nameY, nameBuf, 2, COL_HIGH_M, TFT_BLACK);

    // "HOLD AGAIN TO RUN" gets a reserved row directly above the footer rule,
    // for the same reason the USB line does on the ordinary round brief: it is
    // the line that says what happens next, and it must never be the one the
    // wrap runs out of room for.
    //
    // Size 1, NOT 2, and that is a measurement rather than a preference. The
    // string is 17 columns; at 240 a size-2 row here is 204 px against a chord
    // of about 198 usable, so it would be clipped by the bezel. At size 1 it is
    // 102 px. Every offset here is scaled()/textH(), so the row lands at twice
    // the coordinate on the 480 panel with the same proportional margins - the
    // 240-relative-constant trap that the games' GAME OVER screens fell into.
    const int holdY = uiround::FOOTER_DIV - uiround::textH(1) - uiround::scaled(6);
    const int bodyTop = nameY + uiround::textH(2) + uiround::scaled(8);
    const int bodyMax = holdY - uiround::scaled(4);

    char sends[36];
    snprintf(sends, sizeof(sends), "Sends %s to the host.", e.combo);

    // Two blocks, not one, so the combination keeps its own colour. textBlock()
    // returns the y after its last line, so the effect follows whatever the
    // chord actually allowed the first one to use rather than a guessed offset.
    const int afterSends =
        uiround::textBlock(*_tft, bodyTop, bodyMax, sends, 1,
                           COL_MISCHIEF, TFT_BLACK, uiround::scaled(10));
    uiround::textBlock(*_tft, afterSends + uiround::scaled(2), bodyMax,
                       e.effect, 1, COL_ITEM, TFT_BLACK, uiround::scaled(10));

    uiround::centerText(*_tft, holdY, "HOLD AGAIN TO RUN", 1,
                        COL_HIGH_M, TFT_BLACK);

    uiround::footer(*_tft, "press = cancel", COL_FOOTER);
}

#endif // HEXHOUND_PANEL_ROUND
