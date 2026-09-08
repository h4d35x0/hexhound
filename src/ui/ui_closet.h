#pragma once

#if defined(UNIT_TEST)
#include <cstdint>
#include <cstring>
class TFT_eSPI;
#else
#include "../hal/tft_compat.h"
#endif

#include "../board/board_profile.h"
#include "../config.h"
#include "../pet/pet_core.h"
#include "../pet/pet_wear.h"
#include "../pet/pet_flourish.h"

// ── HexHound - Closet Screen ────────────────────────────────────
//
// Where the owner puts things ON the pet and takes them OFF again. The den is
// the room; this is the wardrobe.
//
// It exists because COSMETIC_WORN was authored into the item table with four
// items and four recipes - two of them reachable at Packet Pup - and then
// nothing ever read it. Crafting an ANTENNA HAT produced a row in a list and
// no way to wear it. See the header of src/pet/pet_wear.h.
//
// ── The same verb the den uses ────────────────────────────────────────────
//
// Short press moves the cursor, long press acts, position 0 is BACK. A long
// press on an anchor CYCLES it: bare, then each owned item that fits there,
// then bare again. That single motion is wear, swap and take off. It is not a
// new interaction to learn - it is the one the den already taught - and the
// footer names what the next press will do rather than saying "hold=select".
//
// ── Two rows, always ──────────────────────────────────────────────────────
//
// HEAD and NECK are both shown even when the pet owns nothing for either. An
// empty wardrobe with two labelled, empty pegs reads as "there are two places
// things go and you have not made anything yet"; a blank screen reads as
// broken. This is the same argument the den's empty brackets make, and unlike
// the den's slot count these two rows can never outnumber the content: every
// anchor has items authored for it at two different stages.
//
// ── Nothing here changes a stat ───────────────────────────────────────────
//
// This screen writes exactly one thing: PetState::wornSlots[], through
// wearSet(). Items are cosmetic; see the rule at the top of item_defs.h.

// What the cursor is on. BACK and an anchor are not the same thing and the long
// press differs, so they are not one flat index.
//
// Declared before the row rules below because they name it.
enum ClosetRowKind : uint8_t {
    CLOSET_ROW_BACK = 0,
    CLOSET_ROW_ANCHOR,
    CLOSET_ROW_FLOURISH
};

// Rows below BACK: one per body anchor, then the flourish.
//
// The flourish lives here rather than on a screen of its own because it is the
// same question the other rows answer - what is this pet wearing - and because
// a cosmetic slot with no screen to select it from is exactly the defect this
// screen was built to fix. COSMETIC_WORN sat unread in the item table for
// months behind precisely that gap.
#define CLOSET_ROWS ((uint8_t)(WEAR_SLOT_COUNT + 1))

// ── The row rules ─────────────────────────────────────────────────────────
//
// Free, inline and display-free on purpose, exactly like the den's rules in
// ui_den.h and for the same stated reason: a rule that can only be exercised
// by drawing it is a rule that does not get tested. test_closet includes this
// header and nothing from the rest of src/ui.
//
// They were static functions inside ui_closet.cpp until the FX row was added,
// at which point the row mapping stopped being obvious - an off-by-one here
// silently points the flourish row at a body anchor - and there was no way to
// pin it without a display.

// The peg name for a row: HEAD, NECK, then FX. One place, so the two draw
// paths and the footer cannot drift on what a row is called.
inline const char* closetRowName(uint8_t row) {
    if (row >= WEAR_SLOT_COUNT) return "FX";
    return wearAnchorName((WearAnchor)(row + 1));
}

// What is currently ON row `row`, or 0 for nothing. Both "bare" sentinels are
// item id 0 by the same argument (item 0 is a material and neither a wearable
// nor a flourish), so one return value serves both kinds of row.
inline uint8_t closetRowItem(const PetState& st, uint8_t row) {
    if (row >= WEAR_SLOT_COUNT) return flourishActive(st);
    return wornItemAt(st, (WearAnchor)(row + 1));
}

// Which row a cursor position is on. Cursor 0 is BACK; 1..WEAR_SLOT_COUNT are
// the body anchors; the next one is the flourish; anything beyond is BACK,
// because a cursor past the end must never index a row that is not drawn.
inline ClosetRowKind closetKindForCursor(int cursor, WearAnchor* anchor) {
    if (cursor <= 0) return CLOSET_ROW_BACK;
    const int i = cursor - 1;
    if (i == (int)WEAR_SLOT_COUNT) return CLOSET_ROW_FLOURISH;
    if (i < (int)WEAR_SLOT_COUNT) {
        // +1 because WEAR_NONE occupies 0 and is not an anchor. The same
        // "enum minus its NONE row" mapping wearSlotOf() undoes.
        if (anchor) *anchor = (WearAnchor)(i + 1);
        return CLOSET_ROW_ANCHOR;
    }
    return CLOSET_ROW_BACK;
}

// What a long press did, or why it could not. The screen needs the reason:
// "you own nothing for your head" and "that is everything you have" are
// different sentences and a bool cannot tell them apart.
enum ClosetResult : uint8_t {
    CLOSET_OK_WORN = 0,
    CLOSET_OK_SWAPPED,
    CLOSET_OK_REMOVED,
    CLOSET_NOT_AN_ANCHOR,
    CLOSET_NONE_OWNED     // nothing craftable for this anchor is owned yet
};

class UICloset {
public:
    static UICloset& instance();

    void init(TFT_eSPI* tft);
    void open();
    void draw();

    // Short press: advance the cursor. BACK -> HEAD -> NECK -> wrap.
    void scrollDown();

    bool backSelected() const { return _cursor == 0; }

    // What the cursor is on, and which anchor when it is on one. Delegates to
    // closetKindForCursor() so the mapping itself is testable.
    ClosetRowKind selectedKind(WearAnchor* anchor = nullptr) const;

    // Long press. Cycles the selected anchor and redraws on success. On any
    // non-OK result NOTHING was written and nothing was drawn.
    ClosetResult act();

    // Right-hand footer hint: what a long press does on THIS position, stated
    // as the real answer rather than a generic one.
    const char* actionHint() const;

    // One short line for a toast, for every result including the failures.
    static const char* resultText(ClosetResult r);

private:
    UICloset() = default;

    // Cursor positions: BACK plus one per anchor.
    int entryCount() const { return 1 + (int)CLOSET_ROWS; }

    // Left-hand footer text: what the highlighted position IS.
    void footerLeft(char* buf, size_t n) const;

#if HEXHOUND_PANEL_ROUND
    void drawRound();
#else
    void drawRect();
#endif

    TFT_eSPI* _tft    = nullptr;
    int       _cursor = 0;   // 0 = BACK, then anchors, then the flourish
};
