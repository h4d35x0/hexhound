#include "ui_update.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../ota/ota_serial.h"
#include "../ota/ota_health.h"
#include "../ota/ota_identity.h"
#include "../config.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Firmware Update Screen Implementation ────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan
#define COL_CURSOR_BG 0x2945  // dark blue
#define COL_DIM       0xC618  // light grey
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white
#define COL_GOOD      0x07E0  // green
#define COL_WARN      0xFD20  // amber
#define COL_BAD       0xF800  // red
#define COL_BAR_BG    0x39E7

UIUpdate& UIUpdate::instance() {
    static UIUpdate ui;
    return ui;
}

void UIUpdate::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIUpdate::open() {
    _cursor = 0;
    _lastHash = 0;
    draw();
}

// ── Rows ──────────────────────────────────────────────────────────────────

int UIUpdate::buildRows(Row* out) const {
    const OtaSession& s = OtaService::session();
    int n = 0;

    switch (s.state()) {

    case OtaSession::ARMED:
    case OtaSession::RECEIVING:
    case OtaSession::VERIFYING:
        // Nothing but stop. Leaving the screen while a transfer is live would
        // hide the one thing the owner needs to be watching, and there is no
        // other action that makes sense mid-flight.
        out[n++] = ROW_CANCEL;
        break;

    case OtaSession::READY:
        // ── No BACK here, deliberately ────────────────────────────────────
        //
        // The image is staged and the boot pointer has already moved; the
        // device WILL start the new firmware at its next restart whatever this
        // screen does. Offering "back" would read as "never mind", which is no
        // longer true and which the session itself refuses to pretend (cancel()
        // returns false from READY). So the two honest options are: restart
        // now, or carry on and let it happen at the next power-up.
        out[n++] = ROW_RESTART;
        out[n++] = ROW_LATER;
        break;

    case OtaSession::FAILED:
        out[n++] = ROW_BACK;
        if (OtaService::transportAvailable()) out[n++] = ROW_RETRY;
        break;

    case OtaSession::IDLE:
    default:
        out[n++] = ROW_BACK;
        // No arm row where an update could never complete anyway. A control
        // that cannot work is worse than one that is not there.
        if (OtaService::transportAvailable()) out[n++] = ROW_ARM;
        break;
    }
    return n;
}

int UIUpdate::rowCount() const {
    Row rows[6];
    return buildRows(rows);
}

const char* UIUpdate::rowLabel(Row r) {
    switch (r) {
        case ROW_BACK:    return "BACK";
        case ROW_ARM:     return "START UPDATE";
        case ROW_CANCEL:  return "STOP UPDATE";
        case ROW_RESTART: return "RESTART NOW";
        case ROW_LATER:   return "RESTART LATER";
        case ROW_RETRY:   return "TRY AGAIN";
    }
    return "";
}

// ── Words for the owner ───────────────────────────────────────────────────

// ── IDLE is two different situations ──────────────────────────────────────
//
// A session lands back in IDLE both when nothing has happened yet and after
// something was refused or stopped, and the two must not read the same. The
// case that forced this: arm() REFUSES while the running image is on trial and
// leaves the state at IDLE. Without this distinction the owner presses START
// UPDATE, the session says no, and the screen carries on saying "Up to date"
// with a hint telling them to press the button they just pressed. A control
// that appears to do nothing is indistinguishable from a broken one.
//
// cancel() lands here too, with UPDATE_ABANDONED, which is the other case.
bool UIUpdate::idleAfterRefusal() const {
    const OtaSession& s = OtaService::session();
    return s.state() == OtaSession::IDLE && s.verdict() != OtaImage::ACCEPTED;
}

const char* UIUpdate::statusLine() const {
    const OtaSession& s = OtaService::session();
    if (idleAfterRefusal()) {
        // On-trial is a "wait", not a "stopped": nothing was ever started, and
        // it clears itself in a few seconds.
        return (s.verdict() == OtaImage::UPDATE_ON_TRIAL) ? "Not right now"
                                                          : "Update stopped";
    }
    switch (s.state()) {
        case OtaSession::IDLE:      return "Up to date";
        case OtaSession::ARMED:     return "Ready to receive";
        case OtaSession::RECEIVING: return "Receiving update";
        case OtaSession::VERIFYING: return "Checking update";
        // Not "installed": it is verified and staged, and the OLD firmware is
        // still the one running. Saying installed would claim something that
        // only becomes true after a restart that has not happened.
        case OtaSession::READY:     return "Update ready";
        case OtaSession::FAILED:    return "Update refused";
    }
    return "";
}

const char* UIUpdate::detailLine() const {
    const OtaSession& s = OtaService::session();

    switch (s.state()) {
    case OtaSession::FAILED:
        // ── The refusal text comes from the engine, always ────────────────
        //
        // verdictHelp() is already written for the person holding the device
        // rather than for a developer, and it is the same sentence the
        // companion app shows. Writing a second set here would mean the same
        // refusal eventually gets described two different ways, and the owner
        // has no way to tell which one is right.
        return OtaImage::verdictHelp(s.verdict());

    case OtaSession::ARMED:
        return "Run the update tool on your computer now.";

    case OtaSession::RECEIVING:
        return "Keep the cable connected.";

    case OtaSession::VERIFYING:
        return "Checking the update really arrived intact.";

    case OtaSession::READY:
        // ── READY is a one-way door, so this must not imply a choice ──────
        //
        // setBootPartition() has already run and otadata names the new slot.
        // The update WILL be applied at the next boot whatever the owner does;
        // powering off does not cancel it, it defers it. Nothing in the session
        // can undo that, which is why cancel() refuses from here and why this
        // screen offers no cancel row.
        //
        // So the text states it as a fact rather than as an option, and then
        // gives the reassurance that makes that fact an easy one to hear: if
        // the new image does not work, the bootloader puts this one back by
        // itself. That is not a consolation, it is exactly what the trial
        // window in ota_health.h guarantees.
        return "Installs at the next restart. It rolls back if it fails.";

    case OtaSession::IDLE:
    default:
        // A refusal that left the state at IDLE still gets explained, in the
        // engine's words. See idleAfterRefusal(): the arm-during-trial refusal
        // is the one that would otherwise be completely silent.
        if (idleAfterRefusal()) {
            return OtaImage::verdictHelp(s.verdict());
        }
        if (!OtaService::transportAvailable()) {
            return "Updates need the real device.";
        }
        if (OtaHealth::bootedAfterRollback()) {
            // Surfaced here because otherwise a failed update looks to the
            // owner exactly like nothing happened, which is the single most
            // confusing outcome available: they ran the tool, they watched it
            // finish, and the device came back on the old version with no
            // explanation.
            return "Your last update was rejected and undone.";
        }
        return "Start this, then run the update tool.";
    }
}

int UIUpdate::progressPct() const {
    const OtaSession& s = OtaService::session();
    if (s.state() != OtaSession::RECEIVING) return -1;
    const uint32_t total = s.imageLen();
    if (total == 0) return 0;
    // 64-bit intermediate: a 1.2 MB image times 100 overflows a uint32 well
    // before the end of the transfer, and the bar would wrap to zero somewhere
    // around 40%.
    return (int)(((uint64_t)s.bytesWritten() * 100u) / total);
}

uint32_t UIUpdate::visibleHash() const {
    const OtaSession& s = OtaService::session();
    // Progress at percent granularity, not at byte granularity: the bar is
    // about 100 pixels wide, so a repaint per byte would redraw the same
    // pixels thousands of times and fight the transfer for the SPI bus.
    return ((uint32_t)s.state() << 24) ^
           ((uint32_t)s.verdict() << 16) ^
           ((uint32_t)(progressPct() + 1) << 8) ^
           (uint32_t)_cursor;
}

void UIUpdate::update() {
    const uint32_t h = visibleHash();
    if (h == _lastHash) return;
    draw();
}

void UIUpdate::scrollDown() {
    const int n = rowCount();
    if (n <= 0) return;
    _cursor = (_cursor + 1) % n;
    draw();
}

UpdateOutcome UIUpdate::act(uint32_t nowMs) {
    Row rows[6];
    const int n = buildRows(rows);
    if (n <= 0) return UPDATE_EXIT;

    int idx = _cursor;
    if (idx < 0 || idx >= n) idx = 0;

    switch (rows[idx]) {

    case ROW_BACK:
    case ROW_LATER:
        return UPDATE_EXIT;

    case ROW_ARM:
    case ROW_RETRY:
        // ── The owner gate, and the only call to it ───────────────────────
        //
        // This line is the whole of "no automatic updates and no silent ones".
        // A person is holding the button. Nothing else in the firmware reaches
        // OtaService::arm(), and nothing that arrives on the wire can.
        //
        // ── The refusal is looked at, not assumed away ────────────────────
        //
        // arm() returns false from READY and while the running image is on
        // trial. A screen that announced "ready to receive" over a session that
        // had refused would be telling the owner to go and run a tool that
        // cannot possibly work.
        //
        // Neither refusal is reachable from the rows THIS screen offers today:
        // READY does not carry an arm row at all, and the on-trial case is
        // rendered by idleAfterRefusal() from the verdict the session left
        // behind. The branch is here anyway because that is a property of the
        // current row table rather than of arm(), and the row table is exactly
        // the kind of thing a later change edits without rereading this.
        //
        // What handles it is the redraw below rather than anything in this
        // branch: a refused arm leaves the session carrying the verdict that
        // says why, and statusLine()/detailLine() render it. So the only thing
        // this must not do is skip the redraw on failure, which is why the
        // call is not wrapped in a success test.
        (void)OtaService::arm(nowMs);
        _cursor = 0;
        draw();
        return UPDATE_STAY;

    case ROW_CANCEL:
        OtaService::cancel();
        _cursor = 0;
        draw();
        return UPDATE_STAY;

    case ROW_RESTART:
        return UPDATE_DO_RESTART;
    }
    return UPDATE_STAY;
}

void UIUpdate::draw() {
    if (!_tft) return;
    _lastHash = visibleHash();
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    drawRect();
#endif
}

#if !HEXHOUND_PANEL_ROUND

namespace {

// Word-wrapped text for the rectangular panels. Breaks on spaces, falls back to
// a hard break for a word longer than the line, and stops at maxLines so a long
// refusal cannot run over the action rows below it.
//
// ── Why the ellipsis is not cosmetic ──────────────────────────────────────
//
// The longest refusal help strings run to about a hundred characters, and the
// 160x80 panel cannot show that many alongside a status line and two buttons.
// Something has to be cut. What must NOT happen is that the cut reads as a
// finished sentence, because most of these strings end in "Nothing was
// changed", which is the single fact the owner actually wants after a failed
// update. "The update was checked and was good" stopping dead there says
// almost the opposite of what the full sentence says. The ellipsis is what
// makes a truncated reassurance obviously truncated.
//
// Returns the y after the last line drawn.
int wrapText(TFT_eSPI& tft, int x, int y, int maxChars, int lineH,
             int size, uint16_t fg, const char* text, int maxLines) {
    if (text == nullptr || maxChars < 1 || maxLines < 1) return y;
    tft.setTextSize(size);
    tft.setTextColor(fg, TFT_BLACK);

    int lines = 0;
    const char* p = text;
    char buf[72];
    if (maxChars > (int)sizeof(buf) - 4) maxChars = (int)sizeof(buf) - 4;

    while (*p != '\0' && lines < maxLines) {
        const bool lastLine = (lines == maxLines - 1);

        int take = 0;
        int lastSpace = -1;
        while (p[take] != '\0' && take < maxChars) {
            if (p[take] == ' ') lastSpace = take;
            take++;
        }
        const bool more = (p[take] != '\0');
        // Only break at a space when the text actually continues past the line;
        // otherwise the tail fits and breaking it early wastes a row.
        if (more && lastSpace > 0 && !(lastLine && more)) take = lastSpace;

        memcpy(buf, p, (size_t)take);
        buf[take] = '\0';
        if (lastLine && more) {
            // Room for the marker is taken out of the text, not added past the
            // right edge, so this cannot push the line off the panel.
            int cut = take;
            if (cut > maxChars - 3) cut = maxChars - 3;
            if (cut < 0) cut = 0;
            buf[cut] = '\0';
            strcat(buf, "...");
        }
        tft.setCursor(x, y);
        tft.print(buf);
        y += lineH;
        lines++;

        p += take;
        while (*p == ' ') p++;
    }
    return y;
}

}  // namespace

void UIUpdate::drawRect() {
    const OtaSession& s = OtaService::session();
    const bool big = (SCREEN_H > 100);

    const int ts    = big ? 2 : 1;
    const int charW = big ? uilg::CHAR_W : 6;
    const int charH = big ? uilg::CHAR_H : 8;
    const int padX  = big ? uilg::PAD : 4;
    const int bodyY = big ? uilg::BODY_Y : 14;
    const int lineH = big ? 18 : 9;
    const int rowH  = big ? 20 : 9;

    // ── The compact panel has to give something up on a refusal ───────────
    //
    // 160x80 leaves 56 pixels between the header and footer rules, which is
    // six size-1 lines. A refused update needs a status line, up to four lines
    // of explanation and two action rows, and that is eight. So on THIS panel
    // only, and only while refused, the installed version line is dropped: it
    // is the least useful of the three at the moment an update has just been
    // turned away, and dropping it is what lets the explanation say whether
    // the device is still all right instead of being cut before it gets there.
    // The big and round panels have room for everything and lose nothing.
    // READY is in this set alongside the refusals, for the opposite reason: its
    // sentence is the one that tells the owner the update is going to happen at
    // the next boot regardless and that it rolls itself back if it misbehaves.
    // Truncating THAT to fit a version number they can read any other time
    // would be the wrong thing to keep.
    const bool needsWords = (s.state() == OtaSession::FAILED) ||
                            (s.state() == OtaSession::READY)  ||
                            idleAfterRefusal();
    const bool showVersion = big || !needsWords;

    _tft->fillScreen(TFT_BLACK);

    if (big) {
        uiBigHeader(*_tft, "\x04 UPDATE", COL_HEADER);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(COL_HEADER, TFT_BLACK);
        _tft->setCursor(4, 2);
        _tft->print("\x04 UPDATE");
        _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
    }

    int y = bodyY;

    // ── Installed version ─────────────────────────────────────────────────
    //
    // First line on the screen because it is the one fact the owner needs
    // before deciding whether an update is worth doing at all, and the only
    // one that is true regardless of what the session is up to. See
    // showVersion above for the single case where the compact panel gives it
    // up.
    if (showVersion) {
        char verBuf[24];
        snprintf(verBuf, sizeof(verBuf), "FW %u.%u.%u",
                 (unsigned)HEXHOUND_FW_VERSION_MAJOR,
                 (unsigned)HEXHOUND_FW_VERSION_MINOR,
                 (unsigned)HEXHOUND_FW_VERSION_PATCH);
        _tft->setTextSize(ts);
        _tft->setTextColor(COL_ITEM, TFT_BLACK);
        _tft->setCursor(padX, y);
        _tft->print(verBuf);

        // The rollback banner shares this row, right-aligned, so it is visible
        // on the compact panel without spending one of its six body lines.
        if (OtaHealth::bootedAfterRollback()) {
            const char* tag = big ? "ROLLED BACK" : "ROLLBACK";
            const int w = (int)strlen(tag) * charW;
            _tft->setTextColor(COL_WARN, TFT_BLACK);
            _tft->setCursor(SCREEN_W - w - padX, y);
            _tft->print(tag);
        }
        y += lineH;
    }

    // ── Status ────────────────────────────────────────────────────────────
    uint16_t statusCol = COL_ITEM;
    if (s.state() == OtaSession::FAILED)      statusCol = COL_BAD;
    else if (s.state() == OtaSession::READY)  statusCol = COL_GOOD;
    else if (s.state() != OtaSession::IDLE)   statusCol = COL_WARN;

    _tft->setTextSize(ts);
    _tft->setTextColor(statusCol, TFT_BLACK);
    _tft->setCursor(padX, y);
    _tft->print(statusLine());
    y += lineH;

    // ── Progress bar, only while it means something ───────────────────────
    const int pct = progressPct();
    if (pct >= 0) {
        const int barW = SCREEN_W - 2 * padX - (big ? 60 : 30);
        const int barH = big ? 12 : 5;
        _tft->fillRect(padX, y, barW, barH, COL_BAR_BG);
        const int filled = (pct * barW) / 100;
        if (filled > 0) _tft->fillRect(padX, y, filled, barH, COL_GOOD);
        _tft->setTextSize(ts);
        _tft->setTextColor(COL_ITEM, TFT_BLACK);
        _tft->setCursor(padX + barW + 4, y - (big ? 2 : 0));
        _tft->printf("%d%%", pct);
        y += big ? 18 : 8;
    }

    // ── The sentence ──────────────────────────────────────────────────────
    //
    // Size 1 even on the big panel: a refused update carries up to about a
    // hundred characters of explanation, and shrinking it to fit is better than
    // truncating the half that says whether the device is still all right.
    const int detailChars = (SCREEN_W - 2 * padX) / 6;
    const int rowsNeeded  = rowCount();
    const int rowsTop     = (big ? uilg::FOOTER_DIV : SCREEN_H - 12)
                            - rowsNeeded * rowH;
    int detailLines = (rowsTop - y) / 9;
    if (detailLines > 4) detailLines = 4;
    if (detailLines > 0) {
        wrapText(*_tft, padX, y, detailChars, 9, 1, COL_DIM,
                 detailLine(), detailLines);
    }

    // ── Action rows, anchored to the bottom ───────────────────────────────
    //
    // Anchored rather than flowed, so the button the owner is about to press
    // does not move when the sentence above it gets a line longer.
    Row rows[6];
    const int n = buildRows(rows);
    int ry = rowsTop;
    for (int i = 0; i < n; i++) {
        const bool sel = (i == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;
        if (sel) _tft->fillRect(0, ry, SCREEN_W, rowH, COL_CURSOR_BG);
        _tft->setTextSize(ts);
        _tft->setTextColor(sel ? COL_CURSOR : COL_ITEM, bg);
        _tft->setCursor(padX, ry + (rowH - charH) / 2);
        if (sel) _tft->print("> ");
        else     _tft->print("  ");
        _tft->print(rowLabel(rows[i]));
        ry += rowH;
    }

    // ── Footer ────────────────────────────────────────────────────────────
    if (big) {
        uiBigFooter(*_tft, "scroll", "hold=sel", COL_DIVIDER);
    } else {
        const int footerY = SCREEN_H - 10;
        _tft->fillRect(0, footerY - 2, SCREEN_W, SCREEN_H - footerY + 2, TFT_BLACK);
        _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
        _tft->setTextSize(1);
        _tft->setTextColor(COL_FOOTER, TFT_BLACK);
        _tft->setCursor(4, footerY);
        _tft->print("scroll");
        _tft->setCursor(SCREEN_W - 50, footerY);
        _tft->print("hold=sel");
    }
}

#endif  // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ── Round update screen (240x240) ─────────────────────────────────────────
//
// Centered throughout, and the progress readout is the rim ring rather than a
// bar: a horizontal bar on a circle has to be short enough for the narrowest
// row it might occupy, whereas the ring is exactly the shape of the glass and
// is readable from across a table.

void UIUpdate::drawRound() {
    const OtaSession& s = OtaService::session();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "UPDATE", COL_HEADER);

    const int pct = progressPct();
    if (pct >= 0) {
        uiround::progressRing(*_tft, pct, COL_GOOD, COL_BAR_BG);
    }

    int y = uiround::BODY_Y;

    uiround::centerPrintf(*_tft, y, 1, COL_ITEM, TFT_BLACK, "FW %u.%u.%u",
                          (unsigned)HEXHOUND_FW_VERSION_MAJOR,
                          (unsigned)HEXHOUND_FW_VERSION_MINOR,
                          (unsigned)HEXHOUND_FW_VERSION_PATCH);
    // Scaled pitches: unscaled, the version line and the status line collide
    // once the text doubles on the 480 panel.
    y += uiround::textH(1) + uiround::scaled(4);

    if (OtaHealth::bootedAfterRollback()) {
        uiround::centerText(*_tft, y, "ROLLED BACK", 1, COL_WARN, TFT_BLACK);
        y += uiround::textH(1) + uiround::scaled(4);
    }

    uint16_t statusCol = COL_ITEM;
    if (s.state() == OtaSession::FAILED)      statusCol = COL_BAD;
    else if (s.state() == OtaSession::READY)  statusCol = COL_GOOD;
    else if (s.state() != OtaSession::IDLE)   statusCol = COL_WARN;

    uiround::centerText(*_tft, y, statusLine(), 2, statusCol, TFT_BLACK);
    y += uiround::textH(2) + uiround::scaled(6);

    if (pct >= 0) {
        uiround::centerPrintf(*_tft, y, 2, COL_ITEM, TFT_BLACK, "%d%%", pct);
        y += uiround::textH(2) + uiround::scaled(6);
    }

    // Action rows first, from the bottom up, so the sentence gets whatever is
    // left rather than pushing a button under the bezel.
    Row rows[6];
    const int n = buildRows(rows);
    const int rowH = uiround::scaled(24);
    const int rowsTop = uiround::FOOTER_DIV - n * rowH;

    // textBlock wraps each line to the chord at its own row, so the paragraph
    // narrows naturally as it approaches the curve instead of being clipped.
    uiround::textBlock(*_tft, y, rowsTop - 2, detailLine(), 1,
                       COL_DIM, TFT_BLACK);

    int ry = rowsTop;
    for (int i = 0; i < n; i++) {
        const bool sel = (i == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;
        if (sel) uiround::rowFill(*_tft, ry, rowH, COL_CURSOR_BG, 4);
        uiround::centerText(*_tft, ry + (rowH - uiround::textH(2)) / 2,
                            rowLabel(rows[i]), 2,
                            sel ? COL_CURSOR : COL_ITEM, bg);
        ry += rowH;
    }

    uiround::footer(*_tft, "press=scroll  hold=select", COL_DIVIDER);
}

#endif  // HEXHOUND_PANEL_ROUND
