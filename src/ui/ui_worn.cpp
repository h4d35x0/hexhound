#include "ui_worn.h"

#include "animator.h"        // drawSpriteResized()
#include "wear_anchors.h"    // WEAR_ANCHORS, wearAnchorsFor()
#include "worn_art.h"        // WORN_ART_IDS / WORN_ART_DATA / WORN_ART_PX
#include "../pet/pet_core.h"
#include "../pet/pet_wear.h"
#include "../pet/pet_inventory.h"

// ── HexHound - Worn Cosmetic Rendering ──────────────────────────

// Overlay art for an item, or nullptr.
//
// The generated table is keyed by STRING id, and the ids are resolved to
// numeric ones once, lazily, on the first draw. It is done this way round
// rather than by baking numeric ids into the generated header because item ids
// are indices into ITEM_DEFS, and a generated file holding those would be a
// second place that has to be regenerated whenever a row is appended - exactly
// the coupling item_defs.h warns about at the top of its table.
static const uint16_t* wornArtFor(uint8_t itemId) {
    static uint8_t ids[WORN_ART_COUNT];
    static bool    resolved = false;
    if (!resolved) {
        Inventory& inv = Inventory::instance();
        for (uint8_t i = 0; i < WORN_ART_COUNT; i++) {
            ids[i] = inv.idFor(WORN_ART_IDS[i]);
        }
        resolved = true;
    }
    for (uint8_t i = 0; i < WORN_ART_COUNT; i++) {
        if (ids[i] != ITEM_ID_NONE && ids[i] == itemId) return WORN_ART_DATA[i];
    }
    return nullptr;
}

// Place one overlay from an anchor point. Everything is in thousandths of the
// pet's own box, so the arithmetic is the same on every panel.
static void drawOne(TFT_eSPI& tft, const uint16_t* art,
                    const WearAnchorPoint& a, int x, int y, int px) {
    if (!art || px <= 0) return;

    // Round to nearest rather than truncating: at a 24 px pet on the T-Dongle
    // a whole thousandth is a quarter of a pixel, and truncating every one of
    // cx, cy and w in the same direction walked the overlay up and left by a
    // pixel on the smallest panel, which is the one that can least afford it.
    const int side = (a.w * px + WEAR_ANCHOR_UNIT / 2) / WEAR_ANCHOR_UNIT;
    if (side <= 0) return;
    const int cx = (a.cx * px + WEAR_ANCHOR_UNIT / 2) / WEAR_ANCHOR_UNIT;
    const int cy = (a.cy * px + WEAR_ANCHOR_UNIT / 2) / WEAR_ANCHOR_UNIT;

    // srcW/srcH are WORN_ART_PX because that is the size the art is BAKED at.
    // Passing the drawn size instead is the mistake that once read 153 KB past
    // the end of an array on this board; see the note in CONTRIBUTING.md.
    drawSpriteResized(tft, art, WORN_ART_PX, WORN_ART_PX,
                      x + cx - side / 2, y + cy - side / 2, side, side);
}

void drawWornOn(TFT_eSPI& tft, PetStage stage, int x, int y, int px) {
    const PetState& st = PetCore::instance().state();

    const uint8_t head = wornItemAt(st, WEAR_HEAD);
    const uint8_t neck = wornItemAt(st, WEAR_NECK);
    if (head == WORN_BARE && neck == WORN_BARE) return;

    const WearStageAnchors& an = wearAnchorsFor(stage);

    // NECK FIRST, then head. They overlap on the stages with short necks - the
    // gremlin has essentially none - and a hat drawn under a scarf reads as the
    // scarf being worn over the pet's face. Drawing order is the only depth
    // this renderer has, so it is the only place that can say a hat is in front.
    if (neck != WORN_BARE) drawOne(tft, wornArtFor(neck), an.neck, x, y, px);
    if (head != WORN_BARE) drawOne(tft, wornArtFor(head), an.head, x, y, px);
}

uint16_t wornSignature() {
    const PetState& st = PetCore::instance().state();
    // The ids themselves, not a hash: two bytes fit in the return value exactly,
    // so this cannot collide and a screen can compare it for equality safely.
    return (uint16_t)((uint16_t)wornItemAt(st, WEAR_HEAD) << 8)
         | (uint16_t)wornItemAt(st, WEAR_NECK);
}
