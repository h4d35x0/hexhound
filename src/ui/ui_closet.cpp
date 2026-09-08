#include "ui_closet.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "ui_worn.h"
#include "ui_inventory.h"   // itemIconFor(), UI_ITEM_ICON_PX
#include "animator.h"       // drawSprite(), drawSpriteResized(), getStageHomeHD()

#include <stdio.h>
#include <string.h>

// ── HexHound - Closet Screen Implementation ─────────────────────
//
// The rules live in pet/pet_wear.h as free functions so they can be tested
// without a display. This file is only pixels.

#define COL_HEADER   0x07FF   // cyan
#define COL_DIVIDER  0x07FF   // cyan
#define COL_SEL      0x07FF   // cyan  - the cursor
#define COL_TEXT     0xFFFF   // white
#define COL_DIM      0xC618   // light grey - the empty note

// ── Singleton ─────────────────────────────────────────────────────────────

UICloset& UICloset::instance() {
    static UICloset ui;
    return ui;
}

void UICloset::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UICloset::open() {
    _cursor = 0;
    draw();
}

void UICloset::scrollDown() {
    const int total = entryCount();
    if (total <= 0) return;
    _cursor = (_cursor + 1) % total;
    draw();
}

ClosetRowKind UICloset::selectedKind(WearAnchor* anchor) const {
    return closetKindForCursor(_cursor, anchor);
}

ClosetResult UICloset::act() {
    WearAnchor a = WEAR_NONE;
    const ClosetRowKind kind = selectedKind(&a);

    if (kind == CLOSET_ROW_FLOURISH) {
        PetState& fst = PetCore::instance().state();
        const uint8_t before = flourishActive(fst);
        const uint8_t next   = flourishNext(fst);
        if (next == before) {
            if (fst.flourishSlot != next) flourishSet(fst, next);
            return CLOSET_NONE_OWNED;
        }
        flourishSet(fst, next);
        draw();
        if (next == FLOURISH_NONE)   return CLOSET_OK_REMOVED;
        if (before == FLOURISH_NONE) return CLOSET_OK_WORN;
        return CLOSET_OK_SWAPPED;
    }

    if (kind != CLOSET_ROW_ANCHOR) return CLOSET_NOT_AN_ANCHOR;

    PetState& st = PetCore::instance().state();
    const uint8_t before = wornItemAt(st, a);
    const uint8_t next   = wornNextForAnchor(st, a);

    if (next == before) {
        // Nothing to change. Normalise a slot holding an id this firmware
        // cannot wear - a save from a newer build, or a corrupt byte - so the
        // array matches the pet already being drawn, then say WHY nothing
        // moved. Owning nothing for an anchor is normal for a long time and
        // has to be reported, not swallowed: a long press that does nothing
        // and says nothing teaches the owner the button is broken.
        if (st.wornSlots[wearSlotOf(a)] != next) {
            wearSet(st, a, next);
        }
        return CLOSET_NONE_OWNED;
    }

    wearSet(st, a, next);
    draw();

    if (next == WORN_BARE)   return CLOSET_OK_REMOVED;
    if (before == WORN_BARE) return CLOSET_OK_WORN;
    return CLOSET_OK_SWAPPED;
}

const char* UICloset::actionHint() const {
    WearAnchor a = WEAR_NONE;
    const ClosetRowKind kind = selectedKind(&a);

    if (kind == CLOSET_ROW_FLOURISH) {
        const PetState& fst = PetCore::instance().state();
        if (flourishOwnedCount() == 0) return "craft one";
        const uint8_t next = flourishNext(fst);
        if (next == FLOURISH_NONE) {
            return flourishActive(fst) != FLOURISH_NONE ? "hold=off"
                                                        : "craft more";
        }
        static char fbuf[20];
        char word[14];
        itemFirstWord(Inventory::instance().nameFor(next), word, sizeof(word));
        snprintf(fbuf, sizeof(fbuf), "hold:%s", word);
        return fbuf;
    }

    if (kind != CLOSET_ROW_ANCHOR) return "hold=back";

    const PetState& st = PetCore::instance().state();
    if (wornOwnedCount(a) == 0) return "craft one";

    const uint8_t next = wornNextForAnchor(st, a);
    if (next == WORN_BARE) {
        // Cycling off the end of the list takes the item off, which is how a
        // one-verb interface removes something.
        return wornItemAt(st, a) != WORN_BARE ? "hold=off" : "craft more";
    }

    // NAME what a hold would put on. Without it the press looks like it simply
    // picks something for you and gives you no say - which is exactly how the
    // den's version of this was reported before it named the item.
    static char buf[20];
    char word[14];
    itemFirstWord(Inventory::instance().nameFor(next), word, sizeof(word));
    snprintf(buf, sizeof(buf), "hold:%s", word);
    return buf;
}

const char* UICloset::resultText(ClosetResult r) {
    switch (r) {
        case CLOSET_OK_WORN:        return "wearing it";
        case CLOSET_OK_SWAPPED:     return "swapped";
        case CLOSET_OK_REMOVED:     return "took it off";
        case CLOSET_NOT_AN_ANCHOR:  return "nothing there";
        case CLOSET_NONE_OWNED:     return "craft something to wear";
    }
    return "-";
}

void UICloset::footerLeft(char* buf, size_t n) const {
    const PetState& st = PetCore::instance().state();

    WearAnchor a = WEAR_NONE;
    const ClosetRowKind kind = selectedKind(&a);

    if (kind == CLOSET_ROW_FLOURISH) {
        const uint8_t id = flourishActive(st);
        if (id != FLOURISH_NONE) {
            snprintf(buf, n, "%s", Inventory::instance().nameFor(id));
        } else if (flourishOwnedCount() == 0) {
            snprintf(buf, n, "FX none made");
        } else {
            snprintf(buf, n, "FX off");
        }
        return;
    }

    if (kind == CLOSET_ROW_ANCHOR) {
        const uint8_t id = wornItemAt(st, a);
        if (id != WORN_BARE) {
            snprintf(buf, n, "%s", Inventory::instance().nameFor(id));
        } else if (wornOwnedCount(a) == 0) {
            snprintf(buf, n, "%s bare", wearAnchorName(a));
        } else {
            snprintf(buf, n, "%s empty", wearAnchorName(a));
        }
        return;
    }

    // Cursor on BACK: how much of what you own is actually ON. Counted in
    // ITEMS, never in anchors, for the reason the den's footer records: a
    // denominator of slots reads as an accusation that something is missing.
    // The flourish counts too, now that it is a row on this screen. Counting
    // only the worn items said "all 2 on" with three things equipped, which is
    // the same class of wrong number the den's footer was corrected for: a
    // count that does not match what the screen is showing.
    const uint8_t owned = (uint8_t)(wornOwnedTotal() + flourishOwnedCount());
    const uint8_t on    = (uint8_t)(wornCount(st) +
                                    (flourishActive(st) != FLOURISH_NONE ? 1 : 0));
    if (owned == 0) {
        snprintf(buf, n, "nothing to wear");
    } else if (on >= owned) {
        snprintf(buf, n, "all %u on", (unsigned)owned);
    } else {
        snprintf(buf, n, "%u of %u on", (unsigned)on, (unsigned)owned);
    }
}

void UICloset::draw() {
    if (!_tft) return;
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    drawRect();
#endif
}

#if HEXHOUND_PANEL_ROUND

// One anchor's row: its name, what is on it, and its icon. Shared shape with
// the den's slot so the two screens read as the same kind of thing.
//
// Round only. The rectangular panels split the peg and the item onto two lines
// (see drawRect), so they have no use for the joined form, and leaving it at
// file scope made it an unused static on five of the eight boards.
static void anchorLabel(uint8_t row, char* buf, size_t n) {
    const PetState& st = PetCore::instance().state();
    const uint8_t id = closetRowItem(st, row);
    if (id == 0) {
        snprintf(buf, n, "%s  -", closetRowName(row));
    } else {
        snprintf(buf, n, "%s  %s", closetRowName(row),
                 Inventory::instance().nameFor(id));
    }
}

#endif  // HEXHOUND_PANEL_ROUND

#if !HEXHOUND_PANEL_ROUND

// The longest wearable name in the item table, in character cells: RECON
// GOGGLES is 13. Stated once here because it is what caps the pet's size, and
// a layout that silently truncated the one name it cannot fit would look like
// a bug in the item table rather than in this file.
static constexpr int CLOSET_NAME_CELLS = 13;

void UICloset::drawRect() {
    const bool big   = (SCREEN_H > 100);
    const int  pad   = big ? uilg::PAD : 4;
    const int  charW = big ? uilg::CHAR_W : 6;
    const int  charH = big ? uilg::CHAR_H : 8;
    const int  ts    = big ? 2 : 1;

    _tft->fillScreen(TFT_BLACK);

    // The pet on the left, wearing whatever is on, and the two anchors listed
    // beside it. Seeing the change land is the entire point of the screen, so
    // the pet is drawn on it rather than only on the home screen.
    //
    // It gets the height the body can give it rather than a fixed 64 px. The
    // fixed size put two short rows in the top corner and a small pet low on
    // the left, and left a band of black across the bottom of both rectangular
    // panels doing nothing. What bounds the picture is not the art, it is the
    // widest item name that still has to fit beside it: RECON GOGGLES, 13
    // cells. Everything below is derived from that rather than hand-tuned, so
    // the two panels cannot drift apart the way the old pair of row pitches
    // did.
    const PetStage stage = PetCore::instance().state().stage;
    const int bodyTop = (big ? uilg::HEADER_DIV : 12) + 1;
    const int bodyBot = big ? uilg::FOOTER_DIV : (SCREEN_H - 12);
    const int bodyH   = bodyBot - bodyTop;

    int petPx = bodyH - (big ? 12 : 6);
    const int petMax = SCREEN_W - 2 * pad - pad - CLOSET_NAME_CELLS * charW;
    if (petPx > petMax) petPx = petMax;
    if (petPx < 16) petPx = 16;
    const int petX = pad;
    const int petY = bodyTop + (bodyH - petPx) / 2;
    drawSpriteResized(*_tft, getStageHomeHD(stage),
                      HD_HOME_SRC_PX, HD_HOME_SRC_PX, petX, petY, petPx, petPx);
    drawWornOn(*_tft, stage, petX, petY, petPx);

    // Each anchor is a two-line block: the peg, then what is hanging on it.
    // One line each was what forced the pet to stay small - "NECK  SIGNAL
    // SCARF" needs 18 cells beside the picture, where the name alone needs 13
    // - and it is also what left the rows huddled at the top. Split, the block
    // is taller, so the pair centres against the pet and fills the band the
    // old layout wasted.
    const int labelH = 8;                  // the peg name is always size 1
    // The gap INSIDE a block, between the peg and its item. One pixel on the
    // compact panel: three blocks at two lines each is 55 px of the 55 px it
    // has, so every pixel here is one the stack does not have.
    const int innerH = big ? 2 : 1;
    const int blockH = labelH + innerH + charH;
    const int listX  = petX + petPx + pad;
    const int listW  = SCREEN_W - listX - pad;

    // The gap BETWEEN blocks is what gives, and it is derived rather than
    // fixed. It was a constant 6 on the compact panel, which was right for two
    // rows and overflowed the moment a third arrived: the FX row printed
    // through the footer rule and its item name landed on top of "hold=back".
    // Deriving it means the next row costs a little air rather than a defect.
    int gapH = big ? 16 : 6;
    const int rows  = (int)CLOSET_ROWS;
    const int spare = bodyH - blockH * rows;
    if (rows > 1 && spare / (rows - 1) < gapH) {
        gapH = spare > 0 ? spare / (rows - 1) : 0;
    }
    const int totalH = blockH * rows + gapH * (rows - 1);

    int y = bodyTop + (bodyH - totalH) / 2;
    if (y < bodyTop) y = bodyTop;

    const PetState& pst = PetCore::instance().state();
    const int maxChars = listW / charW;

    for (uint8_t i = 0; i < CLOSET_ROWS; i++) {
        const bool sel = (_cursor == (int)i + 1);
        if (sel) {
            _tft->fillRect(listX - 2, y - 2, SCREEN_W - listX + 2,
                           blockH + 4, 0x18E3);
        }
        const uint16_t bg = sel ? 0x18E3 : TFT_BLACK;

        // The peg, dim, so the eye lands on the item and not on the word HEAD.
        _tft->setTextSize(1);
        _tft->setTextColor(sel ? COL_SEL : COL_DIM, bg);
        _tft->setCursor(listX, y);
        _tft->print(closetRowName(i));

        // What is on it. A bare anchor keeps the dash the joined form used, so
        // an empty peg still reads as a peg rather than as a missing row.
        const uint8_t id = closetRowItem(pst, i);
        char buf[24];
        if (id == WORN_BARE) {
            snprintf(buf, sizeof(buf), "-");
        } else {
            snprintf(buf, sizeof(buf), "%s", Inventory::instance().nameFor(id));
        }
        // Clip to the room actually left, rather than trusting the name to fit.
        if (maxChars > 0 && (int)strlen(buf) > maxChars) buf[maxChars] = '\0';
        _tft->setTextSize(ts);
        _tft->setTextColor((id == WORN_BARE) ? COL_DIM : COL_TEXT, bg);
        _tft->setCursor(listX, y + labelH + innerH);
        _tft->print(buf);

        y += blockH + gapH;
    }

    if (big) {
        uiBigHeader(*_tft, "CLOSET", COL_HEADER);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(COL_HEADER, TFT_BLACK);
        _tft->setCursor(4, 2);
        _tft->print("CLOSET");
        _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
    }

    char left[28];
    footerLeft(left, sizeof(left));
    const char* hint = actionHint();
    const uint16_t leftCol =
        (wornOwnedTotal() + flourishOwnedCount() == 0) ? COL_DIM : COL_TEXT;

    if (big) {
        uiBigFooter(*_tft, left, hint, leftCol);
        return;
    }

    const int footerY = SCREEN_H - 10;
    const int hintW   = (int)strlen(hint) * 6;
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    char lbuf[28];
    snprintf(lbuf, sizeof(lbuf), "%s", left);
    const int room = (SCREEN_W - hintW - 12) / 6;
    if (room > 0 && (int)strlen(lbuf) > room) lbuf[room] = '\0';
    _tft->setTextSize(1);
    _tft->setTextColor(leftCol, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print(lbuf);
    _tft->setTextColor(COL_TEXT, TFT_BLACK);
    _tft->setCursor(SCREEN_W - hintW - 4, footerY);
    _tft->print(hint);
}

#endif  // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

void UICloset::drawRound() {
    _tft->fillScreen(TFT_BLACK);

    const PetStage stage = PetCore::instance().state().stage;

    // The pet fills the middle of the glass with the two anchor rows beneath
    // it. On a circle the pet IS the interface: the rows say what is on, and
    // the picture says what that looks like.
    const int rowH   = uiround::textH(1) + uiround::scaled(6);
    const int listY  = uiround::FOOTER_DIV - uiround::scaled(4)
                     - rowH * (int)CLOSET_ROWS;
    const int petTop = uiround::BODY_Y + uiround::scaled(2);
    int petPx = listY - petTop - uiround::scaled(4);
    const int petMax = uiround::scaled(96);
    if (petPx > petMax) petPx = petMax;
    if (petPx < uiround::scaled(24)) petPx = uiround::scaled(24);

    const int petX = uiround::CX - petPx / 2;
    drawSpriteResized(*_tft, getStageHomeHD(stage),
                      HD_HOME_SRC_PX, HD_HOME_SRC_PX,
                      petX, petTop, petPx, petPx);
    drawWornOn(*_tft, stage, petX, petTop, petPx);

    int y = listY;
    for (uint8_t i = 0; i < CLOSET_ROWS; i++) {
        const bool sel = (_cursor == (int)i + 1);
        if (sel) uiround::rowFill(*_tft, y, rowH, 0x18E3, uiround::scaled(4));

        char buf[28];
        anchorLabel(i, buf, sizeof(buf));
        const int ty = y + (rowH - uiround::textH(1)) / 2;

        // The icon and the text are centred together as one block, the same
        // way the Kit centres its rows, so the pair stays optically centred on
        // a chord instead of the text drifting off it.
        const uint8_t id = closetRowItem(PetCore::instance().state(), i);
        const uint16_t* icon = (id != 0) ? itemIconFor(id) : nullptr;
        const int iconW = icon ? (UI_ITEM_ICON_PX + uiround::scaled(4)) : 0;

        const int halfW = uiround::chordHalfW(ty);
        const int maxChars = (halfW * 2 - uiround::scaled(16) - iconW)
                           / uiround::cellW(1);
        if (maxChars > 0 && (int)strlen(buf) > maxChars) buf[maxChars] = '\0';

        const int textW = (int)strlen(buf) * uiround::cellW(1);
        int x0 = uiround::CX - (iconW + textW) / 2;
        if (icon) {
            drawSprite(*_tft, icon, x0,
                       ty - (UI_ITEM_ICON_PX - uiround::textH(1)) / 2,
                       UI_ITEM_ICON_PX, UI_ITEM_ICON_PX);
            x0 += iconW;
        }
        // Raw setTextSize bypasses the uiround helpers, so it needs ts() by
        // hand; without it this row stays size 1 while everything around it
        // doubles on the 480 panel.
        _tft->setTextSize(uiround::ts(1));
        _tft->setTextColor(sel ? COL_SEL : COL_TEXT, sel ? 0x18E3 : TFT_BLACK);
        _tft->setCursor(x0, ty);
        _tft->print(buf);

        y += rowH;
    }

    uiround::header(*_tft, "CLOSET", COL_HEADER);

    char left[28];
    footerLeft(left, sizeof(left));
    char hintBuf[56];
    snprintf(hintBuf, sizeof(hintBuf), "%s  %s", left, actionHint());
    uiround::footer(*_tft, hintBuf,
                    (wornOwnedTotal() + flourishOwnedCount()) == 0 ? COL_DIM
                                                                   : COL_DIVIDER);
}

#endif  // HEXHOUND_PANEL_ROUND
