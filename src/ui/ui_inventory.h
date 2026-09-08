#pragma once
#include "../hal/tft_compat.h"
#include "../board/board_profile.h"  // SCREEN_W / SCREEN_H
#include "touch_nav.h"        // RowHitBox
#include "../pet/pet_inventory.h"

// ── HexHound - Inventory Screen ─────────────────────────────────
//
// Renders SCREEN_INVENTORY: what the pet is carrying, and what it can make out
// of it. One scrolling list, one button, exactly like ui_missions and
// ui_quests: short press moves the cursor, long press acts on the highlighted
// row, and row 0 is BACK.
//
// ── Layout of the list ────────────────────────────────────────────────────
//
//   row 0                BACK
//   materials section    one row per material the pet actually holds, or a
//                        single note row when it holds none
//   recipes section      EVERY recipe, always, ordered so the useful ones are
//                        at the top: craftable, then short of materials, then
//                        already owned, then still locked by stage
//
// Recipes are never hidden. A locked recipe is the only way a player learns
// that a Beacon Lamp exists to work towards, and a screen that shows nothing
// until you have already earned it teaches nothing.
//
// ── An empty inventory is a first-class state ─────────────────────────────
//
// A NEW PET OWNS NOTHING. That is the state this screen is seen in first, and
// it has to read as "here is what you are working towards", not as a fault or
// a blank panel. Concretely: the materials section collapses to one honest
// note row rather than vanishing, the recipe rows are all still there with
// their costs, and the footer says "nothing carried" instead of "0/0". The
// empty case is not routed around and it is not hidden behind a menu gate.
//
// ── Nothing here changes a stat ───────────────────────────────────────────
//
// The only mutation this screen can cause is Inventory::craft(), which spends
// materials and grants a cosmetic. See src/content/item_defs.h.

// Loop / scroll bound only (never an array size), so a runtime expression is
// safe. Matches MISSIONS_PER_PAGE and QUESTS_PER_PAGE.
#define INVENTORY_PER_PAGE ((SCREEN_H > 100) ? 5 : 4)

// ── Item icons, for the OTHER screens that show items ─────────────────────
//
// src/ui/item_art.h says "include from ONE .cpp only", and it means it: the
// icon tables are `static const`, so every translation unit that included the
// header would bake its own private copy into flash - 2,560 bytes per copy on
// a 160x80 panel, 10,240 on the big and 240 round ones, and 40,960 on the 480
// round one.
//
// ui_inventory.cpp is that one .cpp. Any other screen wanting an item icon
// (the den, at the time of writing) asks for it here instead of including the
// header a second time. Returns nullptr for an id this firmware has no icon
// for, which a caller must treat as "draw the row without an icon" rather than
// as an error: a save can legitimately carry an item id from a newer build.
const uint16_t* itemIconFor(uint8_t itemId);

// Edge length of the icons itemIconFor() returns, in pixels. Mirrors
// ITEM_ICON_PX from item_art.h so a caller can size a slot without including
// that header; ui_inventory.cpp static_asserts the two agree.
//
// Every panel family has a set baked at the size it DRAWS them, so this is both
// the source and the destination size: a caller sizes its slot from this and
// blits the art 1:1. The 480 round panel used to draw the 16 px set at double
// size, which turned every antialiased edge pixel into a 2x2 block; it now has
// its own 32 px set and nothing enlarges an icon anywhere.
#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)
#define UI_ITEM_ICON_PX 32
#elif HEXHOUND_PANEL_ROUND || (SCREEN_H > 100)
#define UI_ITEM_ICON_PX 16
#else
#define UI_ITEM_ICON_PX 8
#endif

// What the highlighted row is. The screen is a flat list because the button is
// a flat input, but the rows are not all the same thing and the long-press
// behaviour and the footer hint both depend on which kind is under the cursor.
enum InvRowKind : uint8_t {
    INV_ROW_BACK = 0,
    INV_ROW_NOTE,       // "no parts yet" - only ever present when none are held
    INV_ROW_MATERIAL,
    INV_ROW_RECIPE
};

class UIInventory {
public:
    static UIInventory& instance();

    void init(TFT_eSPI* tft);

    // Open the screen. Resets the cursor and rebuilds the recipe ordering.
    // The order is fixed for as long as the screen is open, deliberately: a
    // list that resorted itself under the cursor after every craft would move
    // the row you were about to press.
    void open();

    // Draw current state.
    void draw();

    // Short press: advance cursor.
    void scrollDown();

    // ── Touch-only additions ──────────────────────────────────────────────
    // Guarded rather than merely left uncalled, for the reason spelled out in
    // ui_menu.h: --gc-sections DOES drop an unreferenced method, and dropping
    // it still did not leave the six non-touch boards' images byte-identical.
    // Adding touch must not move a byte of a board that has no touch panel.
#if HEXHOUND_HAS_TOUCH
    // Which list index a touch at screen y landed on, or -1 for the dead space
    // above or below the rows. Answered from the geometry the LAST draw()
    // actually used, not recomputed. See RowHitBox.
    int rowIndexAtY(int y) const { return _rows.indexAtY(y); }

    // Page the WINDOW a whole screenful, carrying the cursor into it. This is
    // the swipe verb; scrollDown() above is the button verb and steps one row.
    // Both are needed - the boards with a touch panel still have a button.
    // Clamping lives once, in uiPageBy() in touch_nav.h.
    void pageDown();
    void pageUp();

    // Put the cursor straight on a row. This is the one thing a touch panel can
    // do that a single button cannot: a tap already NAMES the row, so walking
    // to it one short press at a time is pure ceremony - which is exactly the
    // complaint this exists to answer.
    //
    // Does NOT draw. A tap keeps the screen up and needs the repaint; a hold
    // opens the row and the repaint would be a wasted full-screen blit, which
    // on the 480x480 panel is the entire 39 ms frame budget. The caller knows
    // which case it is in and the two must not both pay for it.
    //
    // Out-of-range is ignored rather than clamped. A clamp would silently move
    // the cursor somewhere the finger never was.
    void setCursor(int index);
#endif

    bool backSelected() const { return _cursor == 0; }

    // What kind of row the cursor is on, and its payload: an item id for
    // INV_ROW_MATERIAL, a recipe index for INV_ROW_RECIPE, undefined otherwise.
    InvRowKind selectedKind(uint8_t* payload = nullptr) const;

    // Long press on a recipe row. Returns CRAFT_OK and redraws on success. On
    // any failure nothing was spent and nothing was drawn, and the returned
    // reason is the same one the footer is already showing, so a caller can
    // toast it without re-deriving the rules. Returns CRAFT_NO_SUCH_RECIPE
    // when the cursor is not on a recipe row at all.
    CraftResult craftSelected();

    // Right-hand footer hint. States what a long press does on THIS row rather
    // than a generic "hold=select", because the answer genuinely differs per
    // row and a hint that lies is worse than no hint.
    const char* actionHint() const;

    // What the highlighted recipe still needs, e.g. "NEED 4 SCRAP 2 COPPER".
    // False when the cursor is not on a recipe, or the recipe is ready to make.
    bool needHint(char* buf, size_t n) const;

private:
    UIInventory() = default;

    int  materialRows() const;   // owned material types, or 1 for the note row
    int  recipeRows() const;
    int  totalEntries() const;   // BACK + materials + recipes

    // Item id of the nth material the pet actually holds, or ITEM_ID_NONE.
    uint8_t nthOwnedMaterial(int n) const;

    // Sorts _order so craftable recipes come first. Called only by open().
    void buildOrder();

    void drawHeader();
    void drawFooter();
    // One row's right-hand column: "x12" for a material, "MAKE" / "1/2" /
    // "OWNED" / a stage tag for a recipe. Returns the colour to draw it in.
    uint16_t rowDetail(InvRowKind kind, uint8_t payload,
                       char* buf, size_t n) const;
#if HEXHOUND_PANEL_ROUND
    void drawRound();
#endif

    TFT_eSPI* _tft    = nullptr;
    int       _cursor = 0;
#if HEXHOUND_HAS_TOUCH
    // Row geometry as LAST DRAWN, for rowIndexAtY(). Recorded by draw() at the
    // one point that knows it. Compiled out on the boards with no touch panel,
    // so their object layout and their draw path are unchanged.
    RowHitBox _rows;
#endif
    int       _scroll = 0;   // top visible index
    uint8_t   _order[RECIPE_MAX_DEFS] = {};
    uint8_t   _orderCount = 0;
};
