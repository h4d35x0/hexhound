#include "ui_quests.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "../content/quest_engine.h"
#include "../config.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Daily Quest Screen Implementation ────────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan
#define COL_CURSOR_BG 0x2945  // dark blue
#define COL_DIM       0xFFFF  // white
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white
#define COL_DONE      0x07E0  // green - a completed quest
#define COL_EMPTY     0xC618  // light grey - empty state

// Kind tags. Three characters so the tag costs the same on every row and the
// text budget is a constant, and colour-coded so the kind is legible at 160x80
// where three characters is all the room there is.
#define COL_KIND_CARE    0xF81F  // magenta
#define COL_KIND_EXPLORE 0x07FF  // cyan
#define COL_KIND_CYBER   0xFD20  // amber
#define COL_KIND_LIFE    0xC618  // light grey

static const char* kindTag(QuestKind k) {
    switch (k) {
        case QUEST_CARE:    return "CAR";
        case QUEST_EXPLORE: return "EXP";
        case QUEST_CYBER:   return "CYB";
        case QUEST_LIFE:    return "LIF";
        default:            return "---";
    }
}

static uint16_t kindColor(QuestKind k) {
    switch (k) {
        case QUEST_CARE:    return COL_KIND_CARE;
        case QUEST_EXPLORE: return COL_KIND_EXPLORE;
        case QUEST_CYBER:   return COL_KIND_CYBER;
        case QUEST_LIFE:    return COL_KIND_LIFE;
        default:            return COL_DIM;
    }
}

// "2/3" while running, "DONE" once finished. A completed quest never shows a
// ratio: "3/3" reads as work still on the list, which is the opposite of what
// finishing it should feel like.
static void progressText(const QuestInstance& q, char* buf, size_t n) {
    if (q.status == QUEST_COMPLETE) {
        snprintf(buf, n, "DONE");
        return;
    }
    snprintf(buf, n, "%u/%u", (unsigned)q.progress, (unsigned)q.target);
}

// Copy src into dst, clipped to maxPx pixels at charPx per glyph, ending in
// ".." when it did not fit. Quest text is authored content up to 96 chars and
// every panel here is narrower than that, so truncation is the normal case and
// has to be visible rather than silent.
static void fitText(char* dst, size_t dstSz, const char* src,
                    int maxPx, int charPx) {
    if (!dst || dstSz == 0) return;
    dst[0] = '\0';
    if (!src || charPx <= 0) return;

    int maxChars = maxPx / charPx;
    if (maxChars < 1) return;
    if ((size_t)maxChars > dstSz - 1) maxChars = (int)dstSz - 1;

    int len = (int)strlen(src);
    if (len <= maxChars) {
        memcpy(dst, src, (size_t)len);
        dst[len] = '\0';
        return;
    }
    if (maxChars <= 2) {
        memcpy(dst, src, (size_t)maxChars);
        dst[maxChars] = '\0';
        return;
    }
    memcpy(dst, src, (size_t)(maxChars - 2));
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
}

// The label for a slot: the definition's text, falling back to the instance id
// if the pack that defined it is gone. Never an empty row.
static const char* questLabel(const QuestEngine& qe, int slot) {
    const QuestDef* def = qe.definitionFor((uint8_t)slot);
    if (def && def->text[0]) return def->text;
    const QuestInstance* q = qe.quest((uint8_t)slot);
    return (q && q->id[0]) ? q->id : "(unknown quest)";
}

UIQuests& UIQuests::instance() {
    static UIQuests ui;
    return ui;
}

void UIQuests::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIQuests::open() {
    _cursor = 0;
    _scroll = 0;
    draw();
}

int UIQuests::totalEntries() const {
    return (int)QuestEngine::instance().activeCount() + 1;  // BACK + quests
}

bool UIQuests::canRerollSelected() const {
    auto& qe = QuestEngine::instance();
    int slot = selectedQuest();
    if (slot < 0 || slot >= (int)qe.activeCount()) return false;
    if (qe.rerollsLeft() == 0) return false;
    const QuestInstance* q = qe.quest((uint8_t)slot);
    return q && q->status != QUEST_COMPLETE;
}

bool UIQuests::rerollSelected(uint32_t seed) {
    if (!canRerollSelected()) return false;
    if (!QuestEngine::instance().reroll((uint8_t)selectedQuest(), seed)) {
        return false;
    }
    draw();
    return true;
}

// Right-hand footer hint. It states what a long press does on THIS row rather
// than a generic "hold=select", because the answer genuinely differs per row
// and a hint that lies is worse than no hint.
const char* UIQuests::actionHint() const {
    auto& qe = QuestEngine::instance();
    if (backSelected())        return "hold=back";
    if (canRerollSelected())   return "hold=roll";
    if (qe.rerollsLeft() == 0) return "no reroll";
    return "complete";
}

void UIQuests::draw() {
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;

    auto& qe = QuestEngine::instance();
    const int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);
    drawHeader();

    // Panel-family geometry. Everything below derives from these four values,
    // so the 160x80 and 320x172 layouts are the same code at two scales.
    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  charW = big ? uilg::CHAR_W : 6;
    const int  charH = big ? uilg::CHAR_H : 8;
    const int  rowH  = big ? 22 : 13;
    const int  bodyY = big ? uilg::BODY_Y : 14;
    const int  padX  = big ? uilg::PAD : 4;

    const int textOff = (rowH - charH) / 2;
    int visible = min(QUESTS_PER_PAGE, total - _scroll);
    if (visible < 0) visible = 0;
#if HEXHOUND_HAS_TOUCH
    // Recorded here, where the numbers are known, so a tap is hit-tested
    // against the rows the owner can actually SEE. See rowIndexAtY().
    _rows.note(bodyY, rowH, visible, _scroll);
#endif

    for (int i = 0; i < visible; i++) {
        const int idx = _scroll + i;
        const int y   = bodyY + i * rowH;
        const bool sel = (idx == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;

        _tft->setTextSize(ts);
        if (sel) {
            _tft->fillRect(0, y, SCREEN_W, rowH, COL_CURSOR_BG);
            _tft->setTextColor(COL_CURSOR, bg);
            _tft->setCursor(padX, y + textOff);
            _tft->print("> ");
        }

        int x = padX + 2 * charW;

        if (idx == 0) {
            _tft->setTextColor(sel ? COL_CURSOR : COL_ITEM, bg);
            _tft->setCursor(x, y + textOff);
            _tft->print("BACK");
            continue;
        }

        const int slot = idx - 1;
        const QuestInstance* q = qe.quest((uint8_t)slot);
        if (!q) continue;   // slot vanished under us; leave the row blank

        const bool done = (q->status == QUEST_COMPLETE);
        const uint16_t fg = done ? COL_DONE : (sel ? COL_CURSOR : COL_ITEM);

        // Progress, right-aligned. Drawn first because the text budget is
        // whatever is left after it, and that width is not fixed: a target of
        // 200 needs seven characters where a target of 3 needs three.
        char pbuf[12];
        progressText(*q, pbuf, sizeof(pbuf));
        const int pw = (int)strlen(pbuf) * charW;
        const int px = SCREEN_W - padX - pw;
        _tft->setTextColor(fg, bg);
        _tft->setCursor(px, y + textOff);
        _tft->print(pbuf);

        // Kind tag.
        _tft->setTextColor(done ? COL_DONE : kindColor(q->kind), bg);
        _tft->setCursor(x, y + textOff);
        _tft->print(kindTag(q->kind));
        x += 4 * charW;   // three glyphs plus a space

        // Quest text, clipped to the gap between the tag and the progress.
        char tbuf[CONTENT_MAX_TEXT_LEN];
        fitText(tbuf, sizeof(tbuf), questLabel(qe, slot),
                px - x - charW, charW);
        _tft->setTextColor(fg, bg);
        _tft->setCursor(x, y + textOff);
        _tft->print(tbuf);
    }

    // Zero quests is a legitimate day, not a fault: a pack can leave this board
    // with no eligible definitions. Say so instead of drawing an empty box.
    if (qe.activeCount() == 0) {
        const char* msg = "No quests today";
        const int w = (int)strlen(msg) * charW;
        _tft->setTextSize(ts);
        _tft->setTextColor(COL_EMPTY, TFT_BLACK);
        _tft->setCursor((SCREEN_W - w) / 2, bodyY + rowH + textOff);
        _tft->print(msg);
    }

    drawFooter();
#endif // !HEXHOUND_PANEL_ROUND
}

#if !HEXHOUND_PANEL_ROUND

void UIQuests::drawHeader() {
    const bool big = (SCREEN_H > 100);
    const int total = totalEntries();

    if (big) {
        uiBigHeader(*_tft, "\x04 QUESTS", COL_HEADER);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(COL_HEADER, TFT_BLACK);
        _tft->setCursor(4, 2);
        _tft->print("\x04 QUESTS");
        _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
    }

    if (total > QUESTS_PER_PAGE) {
        const int page = (_scroll / QUESTS_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / QUESTS_PER_PAGE) + 1;
        char pgBuf[8];
        snprintf(pgBuf, sizeof(pgBuf), "%d/%d", page, totalPages);
        const int charW = big ? uilg::CHAR_W : 6;
        const int pgW = (int)strlen(pgBuf) * charW;
        _tft->setTextSize(big ? 2 : 1);
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(SCREEN_W - pgW - (big ? uilg::PAD : 4),
                        big ? uilg::TITLE_Y : 2);
        _tft->print(pgBuf);
    }
}

void UIQuests::drawFooter() {
    auto& qe = QuestEngine::instance();

    // Left: the day at a glance. Right: what a long press does here.
    char doneBuf[16];
    snprintf(doneBuf, sizeof(doneBuf), "%u/%u done",
             (unsigned)qe.completedCount(), (unsigned)qe.activeCount());
    const char* hint = actionHint();
    const uint16_t col = qe.allComplete() ? COL_DONE : COL_FOOTER;

    if (SCREEN_H > 100) {
        uiBigFooter(*_tft, doneBuf, hint, col);
        return;
    }

    const int footerY = SCREEN_H - 10;
    _tft->fillRect(0, footerY - 2, SCREEN_W, SCREEN_H - footerY + 2, TFT_BLACK);
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(col, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print(doneBuf);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(SCREEN_W - (int)strlen(hint) * 6 - 4, footerY);
    _tft->print(hint);
}

#endif // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ── Round quest list (240x240) ────────────────────────────────────────────
// Rows are centered rather than left-aligned with a "> " prefix: on a circle a
// left margin that works for the middle rows runs under the bezel on the top
// and bottom ones. Each quest row is two size-1 lines - the text, then the
// kind and progress - because a size-2 quest text is far wider than any chord
// on this glass.

void UIQuests::drawHeader() {}
void UIQuests::drawFooter() {}

void UIQuests::drawRound() {
    if (!_tft) return;

    auto& qe = QuestEngine::instance();
    const int total = totalEntries();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "QUESTS", COL_HEADER);

    if (total > QUESTS_PER_PAGE) {
        const int page = (_scroll / QUESTS_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / QUESTS_PER_PAGE) + 1;
        uiround::centerPrintf(*_tft, uiround::BODY_Y - uiround::scaled(8), 1, COL_DIM,
                              TFT_BLACK, "%d/%d", page, totalPages);
    }

    const int avail = uiround::FOOTER_DIV - uiround::BODY_Y;   // 132
    int visible = min(QUESTS_PER_PAGE, total - _scroll);
    if (visible < 1) visible = 1;

    int rowH = avail / visible;
    if (rowH > uiround::scaled(30)) rowH = uiround::scaled(30);

    // Centre the block vertically so a short list sits on the widest part of
    // the glass instead of hugging the header.
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
        const int idx = _scroll + i;
        const bool sel = (idx == _cursor);
        const uint16_t bg = sel ? COL_CURSOR_BG : TFT_BLACK;

        if (sel) uiround::rowFill(*_tft, y, rowH, COL_CURSOR_BG, 4);

        if (idx == 0) {
            const int ty = y + (rowH - uiround::textH(2)) / 2;
            uiround::centerText(*_tft, ty, "BACK", 2,
                                sel ? COL_CURSOR : COL_ITEM, bg);
            y += rowH;
            continue;
        }

        const int slot = idx - 1;
        const QuestInstance* q = qe.quest((uint8_t)slot);
        if (!q) { y += rowH; continue; }

        const bool done = (q->status == QUEST_COMPLETE);
        const uint16_t fg = done ? COL_DONE : (sel ? COL_CURSOR : COL_ITEM);

        // Two size-1 lines plus a small gap, measured rather than hardcoded so
        // the block stays centred when the text scales with the panel.
        const int twoLineH = 2 * uiround::textH(1) + uiround::scaled(2);
        const int ty = y + (rowH - twoLineH) / 2;

        char tbuf[CONTENT_MAX_TEXT_LEN];
        const int halfW = uiround::chordHalfW(ty);
        fitText(tbuf, sizeof(tbuf), questLabel(qe, slot),
                halfW * 2 - uiround::scaled(16), uiround::cellW(1));
        uiround::centerText(*_tft, ty, tbuf, 1, fg, bg);

        char pbuf[12];
        progressText(*q, pbuf, sizeof(pbuf));
        char mbuf[24];
        snprintf(mbuf, sizeof(mbuf), "%s  %s", kindTag(q->kind), pbuf);
        uiround::centerText(*_tft, ty + uiround::textH(1) + uiround::scaled(2), mbuf, 1,
                            done ? COL_DONE : kindColor(q->kind), bg);

        y += rowH;
    }

    if (qe.activeCount() == 0) {
        uiround::centerText(*_tft, uiround::CY + uiround::scaled(40), "No quests today", 1,
                            COL_EMPTY, TFT_BLACK);
    }

    char hintBuf[40];
    snprintf(hintBuf, sizeof(hintBuf), "%u/%u done  %s",
             (unsigned)qe.completedCount(), (unsigned)qe.activeCount(),
             actionHint());
    uiround::footer(*_tft, hintBuf,
                    qe.allComplete() ? COL_DONE : COL_DIVIDER);
}

#endif // HEXHOUND_PANEL_ROUND

#if HEXHOUND_HAS_TOUCH
void UIQuests::setCursor(int index) {
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
    } else if (_cursor >= _scroll + QUESTS_PER_PAGE) {
        _scroll = _cursor - QUESTS_PER_PAGE + 1;
    }
}
#endif

#if HEXHOUND_HAS_TOUCH
// See uiPageBy() in touch_nav.h. Guarded rather than left uncalled: an
// unreferenced method IS dropped by --gc-sections, but dropping it did not
// leave the non-touch images byte-identical when this was measured on the
// menu, so the guard is the only version that can be verified.
void UIQuests::pageDown() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), QUESTS_PER_PAGE, 1)) draw();
}

void UIQuests::pageUp() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), QUESTS_PER_PAGE, -1)) draw();
}
#endif

void UIQuests::scrollDown() {
    const int total = totalEntries();
    if (total <= 0) return;

    _cursor = (_cursor + 1) % total;

    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + QUESTS_PER_PAGE) {
        _scroll = _cursor - QUESTS_PER_PAGE + 1;
    }

    draw();
}
