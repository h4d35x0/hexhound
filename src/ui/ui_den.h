#pragma once

#if defined(UNIT_TEST)
// Unit tests bring their own Arduino stubs and tft_compat.h brings a second,
// clashing set (String, min/max, digitalRead, strlcpy). The den's RULES are
// the inline free functions below and they need no display at all, so under
// UNIT_TEST a forward declaration is enough for the screen class's pointer
// member and test_den can include this header directly. Same guard shape as
// src/pet/pet_inventory.h.
#include <cstdint>
#include <cstring>
class TFT_eSPI;
#else
#include "../hal/tft_compat.h"
#endif

#include "../board/board_profile.h"    // SCREEN_W / SCREEN_H
#include "../board/capabilities.h"     // Caps::tier(), BoardTier
#include "../config.h"                 // DEN_SLOT_COUNT, DEN_SLOTS_CORE
#include "../pet/pet_core.h"           // PetState::denSlots[]
#include "../pet/pet_inventory.h"      // ownership queries, the item table

// ── HexHound - Den Screen ───────────────────────────────────────
//
// Renders SCREEN_DEN: the room the pet lives in, and the things the player has
// crafted for it. One button, exactly like ui_inventory and ui_quests: short
// press moves the cursor, long press acts on whatever it is on, and position 0
// is BACK.
//
// The den is NOT a list. It is a picture of a room - wall, floor, horizon, the
// pet standing in it - with the placement slots drawn where the objects would
// actually be: on a shelf, or on the floor. A list of "SLOT 1: DESK LAMP" would
// have been less code, and it would also have thrown away the only reason the
// feature exists, which is seeing the room fill up.
//
// ── The slot array, and why 0 means empty ─────────────────────────────────
//
// PetState::denSlots[i] holds a RAW ITEM ID, and 0 means the slot is empty.
// Item id 0 is a real item (mat.scrap), so that pun is only safe because of an
// invariant that is easy to break by accident:
//
//     EVERY den-placeable item id is >= 1.
//
// Today the den cosmetics are ids 12..16, so there is a lot of headroom, but
// the thing that MUST stay true is only that id 0 is never COSMETIC_DEN.
// ITEM_DEFS is APPEND ONLY (see src/content/item_defs.h), so id 0 can never
// become a den item by a reorder; it could only change if somebody edited row
// zero in place, which that file already forbids for a different reason.
//
// Two things keep this honest rather than hoping:
//
//   1. denZeroMeansEmptyIsSafe() below scans the item table at runtime and
//      answers the question directly. test_den pins it, so a future edit that
//      made mat.scrap a den cosmetic fails the suite instead of silently
//      turning every empty slot in every save into a piece of scrap on the
//      floor.
//   2. Nothing is ever written into a slot without going through
//      denIsPlaceable(), which requires ITEM_COSMETIC + COSMETIC_DEN. A
//      non-den item therefore cannot get into the array in the first place,
//      and a corrupt save carrying one is DRAWN as empty rather than trusted.
//
// A static_assert would be better than a runtime scan, but ItemDef is a class
// type and ITEM_DEFS is not constexpr (its rows hold const char*), so its
// members are not readable in a constant expression. The test is the assert.
//
// ── Tiering ───────────────────────────────────────────────────────────────
//
// TIER_RICH (PSRAM) shows all DEN_SLOT_COUNT slots; TIER_CORE shows the first
// DEN_SLOTS_CORE. The FULL array is persisted on every board either way, so a
// save that moves from a rich board to a core one keeps its whole room and
// gets it back on the way home - the extra slots are hidden, never cleared.
//
// The tier is read through Caps::tier(), never through an #if, and the slot
// count is a function OF a tier rather than of the build, so both answers are
// testable in one native build.
//
// ── An empty den is a first-class state ───────────────────────────────────
//
// A new pet has an empty room and owns nothing to put in it. That is the state
// this screen is seen in first and it has to read as a room the player has not
// furnished yet, not as a blank panel: the wall, floor, horizon and pet are all
// drawn, and every visible slot draws an empty bracket where an object would
// stand. Long-pressing an empty slot while owning nothing says "craft one" -
// it does not silently do nothing, which is the failure mode that teaches a
// player the button is broken.
//
// ── Nothing here changes a stat ───────────────────────────────────────────
//
// This screen writes exactly one thing: PetState::denSlots[]. Items are
// cosmetic; see the rule at the top of src/content/item_defs.h.

// ── Slot rules ────────────────────────────────────────────────────────────
// Free, inline and display-free on purpose: these are the den's RULES, and a
// rule that can only be exercised by drawing it is a rule that does not get
// tested. test_den includes this header and nothing else from src/ui.

// The empty marker in PetState::denSlots[]. Named rather than written as a
// bare 0 at each use, so the invariant above has something to point at.
#define DEN_SLOT_EMPTY  ((uint8_t)0)

// The MOST slots a tier will ever show. Takes the tier as an ARGUMENT rather
// than calling Caps::tier() itself, so a single native test build can check
// both answers. This is a ceiling, not the room: see denRoomSlots().
constexpr uint8_t denSlotCap(BoardTier t) {
    return (t == TIER_RICH) ? (uint8_t)DEN_SLOT_COUNT : (uint8_t)DEN_SLOTS_CORE;
}

static_assert(DEN_SLOTS_CORE <= DEN_SLOT_COUNT,
              "DEN_SLOTS_CORE must be a prefix of denSlots[], not longer than it");

// May this item id be placed in the den? A material may not, a worn cosmetic
// may not, an id this firmware does not know may not, and DEN_SLOT_EMPTY may
// not. This is the single gate on writes into denSlots[].
inline bool denIsPlaceable(uint8_t itemId) {
    if (itemId == DEN_SLOT_EMPTY) return false;
    const ItemDef* d = Inventory::instance().definition(itemId);
    return d && d->kind == ITEM_COSMETIC && d->slot == COSMETIC_DEN;
}

// Does the item table still allow 0 to mean "empty"? True when item id 0 is
// not a den-placeable item. See the invariant note above; test_den pins this.
inline bool denZeroMeansEmptyIsSafe() {
    const ItemDef* d = Inventory::instance().definition(DEN_SLOT_EMPTY);
    return !d || d->slot != COSMETIC_DEN;
}

// Is this item standing in some slot other than `exceptSlot`? An item is one
// physical object, so it can be in one place at a time.
inline bool denIsPlacedElsewhere(const PetState& st, uint8_t itemId,
                                 uint8_t exceptSlot) {
    if (!denIsPlaceable(itemId)) return false;
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        if (i == exceptSlot) continue;
        if (st.denSlots[i] == itemId) return true;
    }
    return false;
}

// What is actually standing in a slot, or DEN_SLOT_EMPTY. Reads through
// denIsPlaceable(), so a slot holding a stale or corrupt id reports empty
// instead of drawing a piece of scrap on the shelf.
inline uint8_t denItemAt(const PetState& st, uint8_t slot) {
    if (slot >= DEN_SLOT_COUNT) return DEN_SLOT_EMPTY;
    const uint8_t id = st.denSlots[slot];
    return denIsPlaceable(id) ? id : DEN_SLOT_EMPTY;
}

// How many of the FIRST `visible` slots have something in them.
inline uint8_t denPlacedCount(const PetState& st, uint8_t visible) {
    if (visible > DEN_SLOT_COUNT) visible = DEN_SLOT_COUNT;
    uint8_t n = 0;
    for (uint8_t i = 0; i < visible; i++) {
        if (denItemAt(st, i) != DEN_SLOT_EMPTY) n++;
    }
    return n;
}

// How many den cosmetics the pet owns at all. Zero is normal early on and is
// the reason the place action has to be able to SAY so.
inline uint8_t denOwnedCount() {
    Inventory& inv = Inventory::instance();
    uint8_t n = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        if (denIsPlaceable(i) && inv.count(i) > 0) n++;
    }
    return n;
}

// The highest slot index in use, plus one; 0 when the room is empty. Used to
// size the room, so hiding a slot can never hide something standing in it -
// which is what would happen to a save carried back from a richer board.
inline uint8_t denHighestOccupied(const PetState& st) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        if (denItemAt(st, i) != DEN_SLOT_EMPTY) n = (uint8_t)(i + 1);
    }
    return n;
}

// How many slots the room actually SHOWS.
//
// The room grows with the collection instead of standing at its ceiling from
// the first boot. A rich board shows eight slots, and the whole game contains
// five COSMETIC_DEN items - three of them reachable before Beacon Beast - so a
// Packet Pup with everything it can make still faced five empty brackets it had
// no way to fill. That is not an empty state, it reads as missing content, and
// it was reported from the board twice: once as "3 of 5 are in the room" and
// once as "I cannot access the other 2 to place more".
//
// So: always one free place to put the next thing, never a wall of dead ones.
//   * never fewer than DEN_SLOTS_CORE, so a brand new den is still a ROOM with
//     places in it rather than a single bracket;
//   * never fewer than owned + 1, so there is always somewhere to put the thing
//     you just made;
//   * never fewer than denHighestOccupied(), so nothing already standing in the
//     room can be hidden by the room shrinking around it;
//   * never more than the tier's cap.
//
// Takes the tier AND the state, so both tiers stay checkable in one native
// build. Note it reads ownership from the Inventory singleton via
// denOwnedCount(), exactly as the rest of this header already does.
inline uint8_t denRoomSlots(BoardTier t, const PetState& st) {
    const uint8_t cap  = denSlotCap(t);
    uint8_t want = (uint8_t)DEN_SLOTS_CORE;

    // int arithmetic on purpose: owned is at most the item table's row count,
    // so owned + 1 cannot overflow, and the cap below bounds the result anyway.
    const int owned = (int)denOwnedCount();
    if (owned + 1 > (int)want) want = (uint8_t)(owned + 1);

    const uint8_t occupied = denHighestOccupied(st);
    if (occupied > want) want = occupied;

    return (want > cap) ? cap : want;
}

// The item a long press would put in `slot`, or DEN_SLOT_EMPTY for "clear it".
//
// One button means one verb, so that verb is CYCLE: a slot walks through every
// owned den item that is not already standing somewhere else, then through
// empty, then round again. That single motion is place, swap and remove, and
// there is no mode to be in.
//
// Selection is by item id order, which is the authored order of ITEM_DEFS, so
// the cycle is the same every time rather than depending on what was placed
// when.
inline uint8_t denNextForSlot(const PetState& st, uint8_t slot) {
    if (slot >= DEN_SLOT_COUNT) return DEN_SLOT_EMPTY;

    Inventory& inv = Inventory::instance();
    const uint8_t current = denItemAt(st, slot);

    // Candidates are owned den items free to stand here. The slot's own item
    // counts as free, which is what lets the cycle step off it and back on.
    // `seen` gates on "strictly after current" without needing a second array:
    // it starts true for an empty slot, so the very first candidate is taken.
    bool seen = (current == DEN_SLOT_EMPTY);

    for (uint8_t id = 0; id < inv.itemCount(); id++) {
        if (!denIsPlaceable(id) || inv.count(id) == 0) continue;
        if (denIsPlacedElsewhere(st, id, slot)) continue;
        if (seen) return id;
        if (id == current) seen = true;
    }

    // Ran off the end: the slot becomes empty, and the next press starts the
    // cycle over. An empty slot reaching here found no candidate at all, and
    // staying empty is the honest answer to that.
    return DEN_SLOT_EMPTY;
}

// What the cursor is on. A flat cursor because the button is a flat input, but
// BACK and a slot are not the same thing and the long press differs.
enum DenRowKind : uint8_t {
    DEN_ROW_BACK = 0,
    DEN_ROW_SLOT
};

// What a long press did, or why it could not. The screen needs the reason and
// not just a bool: "you have not crafted anything for the den yet" and "every
// one of them is already out" are different sentences, and doing nothing while
// saying nothing is the one outcome that is not allowed.
enum DenResult : uint8_t {
    DEN_OK_PLACED = 0,   // an empty slot now holds something
    DEN_OK_SWAPPED,      // a filled slot now holds something else
    DEN_OK_CLEARED,      // a filled slot is now empty; the item is back in the kit
    DEN_NOT_A_SLOT,      // the cursor was on BACK
    DEN_NONE_OWNED,      // the pet owns no den cosmetics at all
    DEN_NONE_FREE        // it owns some, but every one is already standing somewhere
};

class UIDen {
public:
    static UIDen& instance();

    void init(TFT_eSPI* tft);

    // Open the screen. Resets the cursor to BACK and draws.
    void open();

    // Draw current state. Repaints the whole room; there is no partial path,
    // because a slot changing changes the cursor, the footer and the header
    // count together and three partial repaints cost more than one full one.
    void draw();

    // Short press: advance the cursor. BACK -> slot 0 -> ... -> wrap.
    void scrollDown();

    bool backSelected() const { return _cursor == 0; }

    // What the cursor is on, and its slot index when it is on a slot.
    DenRowKind selectedKind(uint8_t* slot = nullptr) const;

    // Long press. Cycles the selected slot (see denNextForSlot) and redraws on
    // success. On any non-OK result NOTHING was written and nothing was drawn,
    // and the reason is the same one the footer is already showing.
    DenResult act();

    // Right-hand footer hint: what a long press does on THIS position. States
    // the real answer per position rather than a generic "hold=select",
    // because the answer genuinely differs and a hint that lies is worse than
    // no hint at all.
    const char* actionHint() const;

    // One short line for a toast, for every result including the failures.
    // Static so a caller can report an outcome it stored earlier.
    static const char* resultText(DenResult r);

    // Slots this board shows right now. The rest of denSlots[] is still
    // persisted, and the count grows with the collection: see denRoomSlots().
    static uint8_t visibleSlots() {
        return denRoomSlots(Caps::tier(), PetCore::instance().state());
    }

private:
    UIDen() = default;

    // Cursor positions: BACK plus one per visible slot.
    int entryCount() const { return 1 + (int)visibleSlots(); }

    // Pull the cursor back inside entryCount(), which can shrink under it now
    // that the room grows and shrinks with the collection.
    void clampCursor();

    // Left-hand footer text: what the highlighted position IS.
    void footerLeft(char* buf, size_t n) const;

#if HEXHOUND_PANEL_ROUND
    void drawRound();
#else
    void drawRect();
#endif

    TFT_eSPI* _tft    = nullptr;
    int       _cursor = 0;    // 0 = BACK, 1..visibleSlots() = slot 0..n-1
};
