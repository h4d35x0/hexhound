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

// ── HexHound - Flourishes ───────────────────────────────────────
//
// Which flourish the pet is showing, and the rules for choosing one or turning
// it off. Free inline functions with no display in them, exactly like the worn
// rules in pet/pet_wear.h and the den's in ui/ui_den.h: a rule that can only be
// exercised by drawing it is a rule that does not get tested. test_flourish
// includes this header and nothing from src/ui.
//
// ── Why this exists ───────────────────────────────────────────────────────
//
// COSMETIC_FLOURISH was the LAST slot in the item table nothing read. Three
// items (PACKET FLURRY, SOLDER SPARKS, NOISE AURORA) and three recipes were
// authored for it, the most expensive of them a Sentinel-gated project costing
// twelve materials, and a player who crafted all three received three rows in
// a list. Exactly the complaint worn cosmetics drew from the board - "how do
// you take items off the pet", answered by the fact that you could not put
// them on either - only one slot later and one stage more expensive.
//
// ── One at a time, and why it is a byte and not an array ──────────────────
//
// A worn cosmetic belongs to a body part, so there are two anchors and two
// items can be on at once without either being in the other's way. A flourish
// is not attached to anything: it is the whole picture AROUND the pet. Two of
// them at once would be two particle systems fighting over one ring of pixels
// at two cadences, on a panel that repaints the idle pet about twice a second.
// That is noise rather than decoration, so the rule is one at a time and
// PetState::flourishSlot is one byte that says so.
//
// ── The invariant on item id 0 ────────────────────────────────────────────
//
// flourishSlot == 0 means NONE. That is safe for the same reason it is safe in
// wornSlots[] and denSlots[]: id 0 is `mat.scrap`, a material, and
// flourishIsFlourish() rejects everything that is not an ITEM_COSMETIC in the
// COSMETIC_FLOURISH slot. The only writes go through this header, and the
// loader in pet_core.cpp bounds the byte before the renderer ever sees it.
// flourishZeroMeansNoneIsSafe() states the invariant and test_flourish pins it,
// so editing row zero of ITEM_DEFS breaks the build's tests rather than
// silently switching a flourish on for every save in the field.
//
// ── Nothing here touches a stat ───────────────────────────────────────────
//
// Choosing a flourish changes what the pet LOOKS like and nothing else. There
// is no call to feedHunger(), changeMood(), changeEnergy() or addXP() in this
// file, and there must never be one. See the rule at the top of
// src/content/item_defs.h.

#define FLOURISH_NONE ((uint8_t)0)

// Is this item a flourish? The single gate on writes into flourishSlot.
// Never reads past the item table, so it is safe to ask about a byte that came
// off a save file.
inline bool flourishIsFlourish(uint8_t itemId) {
    if (itemId == FLOURISH_NONE) return false;
    const ItemDef* d = Inventory::instance().definition(itemId);
    if (!d) return false;
    return d->kind == ITEM_COSMETIC && d->slot == COSMETIC_FLOURISH;
}

// What the pet is ACTUALLY showing, or FLOURISH_NONE.
//
// Reads THROUGH the rules, so a slot holding an id this firmware cannot show -
// a save from a newer build, a corrupt byte, an item the owner no longer has -
// reports none rather than being handed to the renderer. Ownership is part of
// that test for the same reason it is in wornItemAt(): a cosmetic can only
// leave the inventory through a future "start over", but if it ever does, the
// pet must not keep showing it.
inline uint8_t flourishActive(const PetState& st) {
    const uint8_t id = st.flourishSlot;
    if (!flourishIsFlourish(id)) return FLOURISH_NONE;
    if (Inventory::instance().count(id) == 0) return FLOURISH_NONE;
    return id;
}

// How many flourishes the pet owns. Zero is normal for a long time - the
// cheapest one is Gremlin-gated - and is why the screen that offers them has
// to be able to SAY so rather than showing an empty cycle.
inline uint8_t flourishOwnedCount() {
    Inventory& inv = Inventory::instance();
    uint8_t n = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        if (flourishIsFlourish(i) && inv.count(i) > 0) n++;
    }
    return n;
}

// The flourish a long press would select next, or FLOURISH_NONE for "off".
//
// One button means one verb, and that verb is CYCLE - the den's rule and the
// closet's, because those are where a player has already learned it. The
// selection walks every owned flourish, then off, then round again. That single
// motion is choose, swap and turn off, and there is no mode to be in.
//
// Selection is by item id order, which is the authored order of ITEM_DEFS, so
// the cycle is the same every time rather than depending on what was showing
// when. Same `seen` trick wornNextForAnchor() uses: it starts true when nothing
// is selected, so the first candidate is taken, and running off the end lands
// on none, which is the honest answer for an owner who owns nothing.
inline uint8_t flourishNext(const PetState& st) {
    Inventory& inv = Inventory::instance();
    const uint8_t current = flourishActive(st);

    bool seen = (current == FLOURISH_NONE);
    for (uint8_t id = 0; id < inv.itemCount(); id++) {
        if (!flourishIsFlourish(id) || inv.count(id) == 0) continue;
        if (seen) return id;
        if (id == current) seen = true;
    }
    return FLOURISH_NONE;
}

// Select `id`, or FLOURISH_NONE to turn the flourish off. Returns false and
// writes nothing when the item is not a flourish or is not owned, so the byte
// cannot be corrupted by a caller that skipped the rules.
inline bool flourishSet(PetState& st, uint8_t id) {
    if (id != FLOURISH_NONE) {
        if (!flourishIsFlourish(id)) return false;
        if (Inventory::instance().count(id) == 0) return false;
    }
    if (st.flourishSlot == id) return false;
    st.flourishSlot = id;
    st.dirty = true;
    return true;
}

// Does the item table still allow 0 to mean "none"? True when item id 0 is not
// a flourish. See the invariant note above; test_flourish pins this.
inline bool flourishZeroMeansNoneIsSafe() {
    return !flourishIsFlourish(FLOURISH_NONE);
}

// Display name for the selected flourish, for a footer with room for one line.
// "NONE" rather than "-" because this row is a CHOICE the owner made, and a
// dash reads as "unavailable" where the word reads as "off".
inline const char* flourishName(uint8_t id) {
    if (id == FLOURISH_NONE) return "NONE";
    const ItemDef* d = Inventory::instance().definition(id);
    return (d && d->name) ? d->name : "NONE";
}
