#pragma once

#if defined(UNIT_TEST)
#include <cstdint>
#include <cstring>
#else
#include "../config.h"
#endif

#include "../content/item_defs.h"
#include "pet_core.h"
#include "pet_inventory.h"

// ── HexHound - Worn Cosmetics ───────────────────────────────────
//
// What the pet is wearing, and the rules for putting something on or taking it
// off. Free inline functions with no display in them, exactly like the den's
// rules in ui/ui_den.h: a rule that can only be exercised by drawing it is a
// rule that does not get tested. test_wear includes this header and nothing
// from src/ui.
//
// ── Why this exists ───────────────────────────────────────────────────────
//
// COSMETIC_WORN was authored into the item table with four items and four
// recipes, two of them reachable at Packet Pup, and then nothing in the
// firmware ever read it. Before this file, `COSMETIC_WORN` appeared in exactly
// one place in the whole codebase: a label on the Kit screen. A player could
// spend a Packet Pup's entire material budget on an ANTENNA HAT and receive a
// row in a list. Reported from the board as "how do you take items off the
// pet" - and the honest answer was that you could not put them on either.
//
// ── Two anchors, not one slot ─────────────────────────────────────────────
//
// See WearAnchor in item_defs.h. The head/neck split is what the content was
// already authored for.
//
// ── The invariant on item id 0 ────────────────────────────────────────────
//
// wornSlots[i] == 0 means BARE. That is safe for the same reason denSlots[]
// uses 0 for empty: id 0 is `mat.scrap`, a material, and wearIsWearable()
// rejects everything that is not an ITEM_COSMETIC with a real WearAnchor. The
// only writes into the array go through this header, and the loader in
// pet_core.cpp rejects any id outside the table before the renderer sees it.
//
// ── Nothing here touches a stat ───────────────────────────────────────────
//
// Wearing something changes what the pet LOOKS like and nothing else. There is
// no call to feedHunger(), changeMood(), changeEnergy() or addXP() in this
// file or in the closet screen, and there must never be one. See the rule at
// the top of src/content/item_defs.h.

#define WORN_BARE ((uint8_t)0)

// Anchor of an item, or WEAR_NONE. Never reads past the table.
inline WearAnchor wearAnchorOf(uint8_t itemId) {
    const ItemDef* d = Inventory::instance().definition(itemId);
    if (!d) return WEAR_NONE;
    if (d->kind != ITEM_COSMETIC || d->slot != COSMETIC_WORN) return WEAR_NONE;
    return (d->anchor < WEAR_ANCHOR_COUNT) ? (WearAnchor)d->anchor : WEAR_NONE;
}

// May this item be worn at all? The single gate on writes into wornSlots[].
inline bool wearIsWearable(uint8_t itemId) {
    return itemId != WORN_BARE && wearAnchorOf(itemId) != WEAR_NONE;
}

// Array index for an anchor. WEAR_NONE has no slot and answers WEAR_SLOT_COUNT,
// which every caller below treats as out of range.
inline uint8_t wearSlotOf(WearAnchor a) {
    return (a == WEAR_NONE || a >= WEAR_ANCHOR_COUNT)
               ? (uint8_t)WEAR_SLOT_COUNT
               : (uint8_t)(a - 1);
}

// What is actually on the pet at this anchor, or WORN_BARE.
//
// Reads THROUGH the rules, so a slot holding an id this firmware cannot wear -
// a save from a newer build, a corrupt byte, an item the owner no longer has -
// reports bare rather than being handed to the renderer. Ownership is part of
// that test: a cosmetic can only leave the inventory through a future "start
// over", but if it ever does, the pet must not keep wearing it.
inline uint8_t wornItemAt(const PetState& st, WearAnchor a) {
    const uint8_t slot = wearSlotOf(a);
    if (slot >= WEAR_SLOT_COUNT) return WORN_BARE;
    const uint8_t id = st.wornSlots[slot];
    if (!wearIsWearable(id)) return WORN_BARE;
    if (wearAnchorOf(id) != a) return WORN_BARE;
    if (Inventory::instance().count(id) == 0) return WORN_BARE;
    return id;
}

// How many wearable cosmetics the pet owns for this anchor. Zero is normal for
// a long time and is why the closet has to be able to SAY so.
inline uint8_t wornOwnedCount(WearAnchor a) {
    Inventory& inv = Inventory::instance();
    uint8_t n = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        if (wearAnchorOf(i) == a && inv.count(i) > 0) n++;
    }
    return n;
}

// Everything wearable the pet owns, across every anchor.
inline uint8_t wornOwnedTotal() {
    Inventory& inv = Inventory::instance();
    uint8_t n = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        if (wearIsWearable(i) && inv.count(i) > 0) n++;
    }
    return n;
}

// How many anchors have something on them.
inline uint8_t wornCount(const PetState& st) {
    uint8_t n = 0;
    for (uint8_t a = 1; a < WEAR_ANCHOR_COUNT; a++) {
        if (wornItemAt(st, (WearAnchor)a) != WORN_BARE) n++;
    }
    return n;
}

// The item a long press would put on this anchor, or WORN_BARE for "take it
// off".
//
// One button means one verb, and that verb is CYCLE - the den's rule, because
// the den is where a player has already learned it. An anchor walks through
// every owned item that fits it, then through bare, then round again. That
// single motion is wear, swap and remove, and there is no mode to be in.
//
// Selection is by item id order, which is the authored order of ITEM_DEFS, so
// the cycle is the same every time rather than depending on what was worn when.
//
// There is no "placed elsewhere" test, unlike the den: an anchor is a body
// position, not a shelf, and no item fits two of them. An item is on at most
// one anchor because it only HAS one.
inline uint8_t wornNextForAnchor(const PetState& st, WearAnchor a) {
    if (wearSlotOf(a) >= WEAR_SLOT_COUNT) return WORN_BARE;

    Inventory& inv = Inventory::instance();
    const uint8_t current = wornItemAt(st, a);

    // `seen` gates on "strictly after current" without a second array: it
    // starts true for a bare anchor, so the very first candidate is taken.
    bool seen = (current == WORN_BARE);

    for (uint8_t id = 0; id < inv.itemCount(); id++) {
        if (wearAnchorOf(id) != a || inv.count(id) == 0) continue;
        if (seen) return id;
        if (id == current) seen = true;
    }

    // Ran off the end: the anchor becomes bare, and the next press starts the
    // cycle over. A bare anchor reaching here found no candidate at all, and
    // staying bare is the honest answer to that.
    return WORN_BARE;
}

// Put `id` on its own anchor, or WORN_BARE to strip that anchor. Returns false
// and writes nothing when the item is not wearable or is not owned, so the
// array cannot be corrupted by a caller that skipped the rules.
inline bool wearSet(PetState& st, WearAnchor a, uint8_t id) {
    const uint8_t slot = wearSlotOf(a);
    if (slot >= WEAR_SLOT_COUNT) return false;
    if (id != WORN_BARE) {
        if (wearAnchorOf(id) != a) return false;
        if (Inventory::instance().count(id) == 0) return false;
    }
    if (st.wornSlots[slot] == id) return false;
    st.wornSlots[slot] = id;
    st.dirty = true;
    return true;
}

// Does the item table still allow 0 to mean "bare"? True when item id 0 is not
// wearable. See the invariant note above; test_wear pins this.
inline bool wornZeroMeansBareIsSafe() {
    return !wearIsWearable(WORN_BARE);
}

// Short label for an anchor, for a footer that has room for one word.
inline const char* wearAnchorName(WearAnchor a) {
    switch (a) {
        case WEAR_HEAD: return "HEAD";
        case WEAR_NECK: return "NECK";
        default:        return "-";
    }
}
