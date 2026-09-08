#include "ui_inventory.h"
#include "ui_utils.h"
#include "ui_round.h"
#include "animator.h"      // drawSprite()
#include "item_art.h"      // ITEM_ICONS, ITEM_ICON_PX - include from HERE ONLY
#include "../pet/pet_core.h"
#include "../config.h"

#include <stdio.h>
#include <string.h>

// ── HexHound - Inventory Screen Implementation ──────────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan
#define COL_CURSOR_BG 0x2945  // dark blue
#define COL_DIM       0xFFFF  // white
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white
#define COL_MAKE      0x07E0  // green  - enough materials, hold to craft
#define COL_SHORT     0xFD20  // amber  - known recipe, missing materials
#define COL_OWNED     0x8410  // grey   - already made
#define COL_LOCKED    0x632C  // dark grey - not at this stage yet
#define COL_EMPTY     0xC618  // light grey - the empty note

// The icon table and the item table are generated from the same source, but
// they are separate files and a stale one would index off the end.
static_assert(ITEM_ICON_COUNT <= ITEM_TYPE_COUNT,
              "item_art.h has more icons than PetState::items[] can address; "
              "re-run scripts/gen_item_icons.py");

// UI_ITEM_ICON_PX lets a screen size an icon slot without including item_art.h
// and baking a second copy of every icon. It is a mirror, so it has to be
// checked against the thing it mirrors.
static_assert(UI_ITEM_ICON_PX == ITEM_ICON_PX,
              "UI_ITEM_ICON_PX in ui_inventory.h no longer matches ITEM_ICON_PX "
              "in item_art.h");

// This translation unit is the ONLY one that includes item_art.h. Everything
// else goes through here. See the note in ui_inventory.h.
const uint16_t* itemIconFor(uint8_t itemId) {
    if (itemId >= ITEM_ICON_COUNT) return nullptr;
    return ITEM_ICONS[itemId];
}

// Stage abbreviations for a locked recipe's right-hand column. Three glyphs so
// the column costs the same on every row, indexed by PetStage - 1.
static const char* const STAGE_TAG[] = { "EGG", "PUP", "BST", "GRM", "SNT" };

static const char* stageTag(uint8_t stage) {
    if (stage < STAGE_EGG || stage > STAGE_SENTINEL) return "---";
    return STAGE_TAG[stage - 1];
}

// The note that stands in for the materials section when the pet holds nothing.
// A new pet is in this state, so this string is the FIRST thing most people
// will read on this screen; it says what to do, not that something is missing.
static const char* emptyNote() {
#if HEXHOUND_PANEL_ROUND
    return "No parts yet - go roaming";
#else
    return (SCREEN_H > 100) ? "No parts yet - go roaming" : "No parts yet";
#endif
}

// Copy src into dst, clipped to maxPx pixels at charPx per glyph, ending in
// ".." when it did not fit. Item names are short by design but the note line
// and the big-panel font are not, so truncation has to be visible rather than
// silent. Same helper shape as ui_quests.cpp.
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

// How many of a recipe's ingredients the pet already has enough of. This is
// what makes "1/2" mean something on a short recipe: it is progress toward the
// craft, not a count of one arbitrary ingredient.
static uint8_t inputsReady(const Recipe& r) {
    Inventory& inv = Inventory::instance();
    uint8_t ready = 0;
    for (uint8_t i = 0; i < r.inputCount; i++) {
        if (inv.count(r.input[i]) >= r.qty[i]) ready++;
    }
    return ready;
}

// Sort key. Craftable first, then things you are working towards, then things
// you already have, then things you cannot reach yet.
static uint8_t rankOf(uint8_t recipeIndex) {
    switch (Inventory::instance().check(recipeIndex)) {
        case CRAFT_OK:             return 0;
        case CRAFT_SHORT:          return 1;
        case CRAFT_ALREADY_OWNED:  return 2;
        case CRAFT_LOCKED:         return 3;
        default:                   return 4;
    }
}

UIInventory& UIInventory::instance() {
    static UIInventory ui;
    return ui;
}

void UIInventory::init(TFT_eSPI* tft) {
    _tft = tft;
}

void UIInventory::open() {
    _cursor = 0;
    _scroll = 0;
    buildOrder();
    draw();
}

void UIInventory::buildOrder() {
    Inventory& inv = Inventory::instance();
    _orderCount = inv.recipeCount();
    if (_orderCount > RECIPE_MAX_DEFS) _orderCount = RECIPE_MAX_DEFS;

    for (uint8_t i = 0; i < _orderCount; i++) _order[i] = i;

    // Insertion sort, stable, at most RECIPE_MAX_DEFS elements. A stable sort
    // is the point: within a rank the table order is the author's order, and
    // shuffling that would make the list look random between opens.
    for (uint8_t i = 1; i < _orderCount; i++) {
        const uint8_t v = _order[i];
        const uint8_t key = rankOf(v);
        int j = (int)i - 1;
        while (j >= 0 && rankOf(_order[j]) > key) {
            _order[j + 1] = _order[j];
            j--;
        }
        _order[j + 1] = v;
    }
}

int UIInventory::materialRows() const {
    const uint8_t n = Inventory::instance().distinctMaterials();
    // Zero materials collapses to exactly ONE row, the note. The section never
    // disappears, so the list geometry is the same whether the pet is carrying
    // everything or nothing.
    return n > 0 ? (int)n : 1;
}

int UIInventory::recipeRows() const {
    return (int)_orderCount;
}

int UIInventory::totalEntries() const {
    return 1 + materialRows() + recipeRows();
}

uint8_t UIInventory::nthOwnedMaterial(int n) const {
    Inventory& inv = Inventory::instance();
    int seen = 0;
    for (uint8_t i = 0; i < inv.itemCount(); i++) {
        const ItemDef* d = inv.definition(i);
        if (!d || d->kind != ITEM_MATERIAL) continue;
        if (inv.count(i) == 0) continue;
        if (seen == n) return i;
        seen++;
    }
    return ITEM_ID_NONE;
}

InvRowKind UIInventory::selectedKind(uint8_t* payload) const {
    if (_cursor <= 0) return INV_ROW_BACK;

    const int mats = materialRows();
    if (_cursor <= mats) {
        if (Inventory::instance().distinctMaterials() == 0) return INV_ROW_NOTE;
        const uint8_t id = nthOwnedMaterial(_cursor - 1);
        if (id == ITEM_ID_NONE) return INV_ROW_NOTE;
        if (payload) *payload = id;
        return INV_ROW_MATERIAL;
    }

    const int r = _cursor - 1 - mats;
    if (r >= 0 && r < (int)_orderCount) {
        if (payload) *payload = _order[r];
        return INV_ROW_RECIPE;
    }
    // The list shifted underneath the cursor. Report the inert row rather than
    // BACK: a long press must never exit the screen by accident.
    return INV_ROW_NOTE;
}

CraftResult UIInventory::craftSelected() {
    uint8_t payload = 0;
    if (selectedKind(&payload) != INV_ROW_RECIPE) return CRAFT_NO_SUCH_RECIPE;

    const CraftResult res = Inventory::instance().craft(payload);
    if (res == CRAFT_OK) draw();
    return res;
}

// What the highlighted recipe still needs, as "NEED 4 SCRAP 2 COPPER".
//
// The recipe rows named an output and its state - MAKE, HAVE, a stage tag, or a
// "1/2" saying how many of its inputs were satisfied - and nowhere in the whole
// screen said WHAT those inputs were or how many. You could see that a Desk Lamp
// was not craftable and had no way to learn what would make it craftable, which
// is the one question a crafting screen exists to answer.
//
// Only the shortfall is listed, because that is the actionable half: an
// ingredient already in hand is not what is stopping you. Anything that does not
// fit is counted as "+N" rather than truncated, so the line never lies about how
// much is left.
bool UIInventory::needHint(char* buf, size_t n) const {
    if (!buf || n == 0) return false;
    uint8_t payload = 0;
    if (selectedKind(&payload) != INV_ROW_RECIPE) return false;

    Inventory& inv = Inventory::instance();
    const Recipe* r = inv.recipe(payload);
    if (!r || inv.check(payload) != CRAFT_SHORT) return false;

    // The footer is one line on a chord. 26 characters is what the narrowest
    // round footer holds at its own text size, and the rectangular panels are
    // wider, so budgeting for the circle is safe everywhere.
    const size_t COLS = 26;
    size_t used = (size_t)snprintf(buf, n, "NEED");
    int omitted = 0;

    for (uint8_t i = 0; i < r->inputCount; i++) {
        if (r->input[i] == ITEM_ID_NONE) continue;
        const uint16_t have = inv.count(r->input[i]);
        if (have >= r->qty[i]) continue;             // already covered

        char word[14];
        itemFirstWord(inv.nameFor(r->input[i]), word, sizeof(word));
        char piece[24];
        const int pn = snprintf(piece, sizeof(piece), " %u %s",
                                (unsigned)(r->qty[i] - have), word);
        if (pn <= 0) continue;
        if (used + (size_t)pn > COLS || used + (size_t)pn + 1 >= n) {
            omitted++;
            continue;
        }
        memcpy(buf + used, piece, (size_t)pn);
        used += (size_t)pn;
        buf[used] = '\0';
    }

    if (omitted > 0 && used + 4 < n) {
        snprintf(buf + used, n - used, " +%d", omitted);
    }
    return true;
}

const char* UIInventory::actionHint() const {
    uint8_t payload = 0;
    switch (selectedKind(&payload)) {
        case INV_ROW_BACK:     return "hold=back";
        case INV_ROW_NOTE:     return "go roam";
        case INV_ROW_MATERIAL: return "material";
        case INV_ROW_RECIPE:
            switch (Inventory::instance().check(payload)) {
                case CRAFT_OK:            return "hold=make";
                case CRAFT_SHORT:         return "short";
                case CRAFT_ALREADY_OWNED: return "owned";
                case CRAFT_LOCKED:        return "locked";
                default:                  return "-";
            }
        default: return "-";
    }
}

uint16_t UIInventory::rowDetail(InvRowKind kind, uint8_t payload,
                                char* buf, size_t n) const {
    buf[0] = '\0';
    Inventory& inv = Inventory::instance();

    if (kind == INV_ROW_MATERIAL) {
        snprintf(buf, n, "x%u", (unsigned)inv.count(payload));
        const ItemDef* d = inv.definition(payload);
        return d ? d->color : COL_ITEM;
    }

    if (kind != INV_ROW_RECIPE) return COL_EMPTY;

    const Recipe* r = inv.recipe(payload);
    if (!r) return COL_EMPTY;

    switch (inv.check(payload)) {
        case CRAFT_OK:
            snprintf(buf, n, "MAKE");
            return COL_MAKE;
        case CRAFT_ALREADY_OWNED: {
            // WHERE it goes, not just that you have it.
            //
            // Every cosmetic used to report a bare "HAVE", so five made things
            // read as five things for the den - and two of them were ANTENNA HAT
            // and SIGNAL SCARF, which the den can never hold. That was reported
            // from the board as "I cannot access the other 2 to place more":
            // nothing on this screen or the den's had ever said the two were
            // worn rather than furniture. The destination is the only fact a
            // finished cosmetic still has to give.
            const ItemDef* od = inv.definition(r->output);
            const char* where = "";
            if (od) {
                switch (od->slot) {
                    case COSMETIC_DEN:      where = ":DEN";  break;
                    case COSMETIC_WORN:     where = ":WORN"; break;
                    case COSMETIC_FLOURISH: where = ":FX";   break;
                    default: break;
                }
            }
            snprintf(buf, n, "HAVE%s", where);
            return COL_OWNED;
        }
        case CRAFT_LOCKED:
            snprintf(buf, n, "%s", stageTag(r->minStage));
            return COL_LOCKED;
        case CRAFT_SHORT:
        default:
            snprintf(buf, n, "%u/%u", (unsigned)inputsReady(*r),
                     (unsigned)r->inputCount);
            return COL_SHORT;
    }
}

// Label for a row. Never empty: an unknown item id still draws as "?" rather
// than leaving a blank line the player cannot explain.
static const char* rowLabel(InvRowKind kind, uint8_t payload) {
    Inventory& inv = Inventory::instance();
    switch (kind) {
        case INV_ROW_BACK:     return "BACK";
        case INV_ROW_NOTE:     return emptyNote();
        case INV_ROW_MATERIAL: return inv.nameFor(payload);
        case INV_ROW_RECIPE: {
            const Recipe* r = inv.recipe(payload);
            return r ? inv.nameFor(r->output) : "?";
        }
    }
    return "?";
}

// The icon a row shows, or nullptr when it has none (BACK and the note).
static const uint16_t* rowIcon(InvRowKind kind, uint8_t payload) {
    uint8_t id = ITEM_ID_NONE;
    if (kind == INV_ROW_MATERIAL) {
        id = payload;
    } else if (kind == INV_ROW_RECIPE) {
        const Recipe* r = Inventory::instance().recipe(payload);
        if (r) id = r->output;
    }
    return itemIconFor(id);
}

void UIInventory::draw() {
    // Lazily build the ordering if the caller drew before open(). Cheap, and it
    // removes a footgun where a wired screen silently shows no recipes.
    if (_orderCount == 0 && Inventory::instance().recipeCount() > 0) {
        buildOrder();
    }

#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;

    Inventory& inv = Inventory::instance();
    const int total = totalEntries();
    const int mats  = materialRows();

    _tft->fillScreen(TFT_BLACK);
    drawHeader();

    // Panel-family geometry. Everything below derives from these values, so the
    // 160x80 and the 320x172 layouts are the same code at two scales.
    const bool big   = (SCREEN_H > 100);
    const int  ts    = big ? 2 : 1;
    const int  charW = big ? uilg::CHAR_W : 6;
    const int  charH = big ? uilg::CHAR_H : 8;
    const int  rowH  = big ? 22 : 13;
    const int  bodyY = big ? uilg::BODY_Y : 14;
    const int  padX  = big ? uilg::PAD : 4;

    const int textOff = (rowH - charH) / 2;
    const int iconOff = (rowH - ITEM_ICON_PX) / 2;

    int visible = min(INVENTORY_PER_PAGE, total - _scroll);
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

        // Decode this row exactly as selectedKind() would for the cursor.
        InvRowKind kind = INV_ROW_BACK;
        uint8_t    payload = 0;
        if (idx > 0) {
            if (idx <= mats) {
                const uint8_t id = nthOwnedMaterial(idx - 1);
                kind = (id == ITEM_ID_NONE) ? INV_ROW_NOTE : INV_ROW_MATERIAL;
                payload = id;
            } else {
                const int r = idx - 1 - mats;
                if (r < (int)_orderCount) {
                    kind = INV_ROW_RECIPE;
                    payload = _order[r];
                } else {
                    continue;   // shifted under us; leave the row blank
                }
            }
        }

        if (sel) _tft->fillRect(0, y, SCREEN_W, rowH, COL_CURSOR_BG);

        _tft->setTextSize(ts);
        if (sel) {
            _tft->setTextColor(COL_CURSOR, bg);
            _tft->setCursor(padX, y + textOff);
            _tft->print("> ");
        }

        int x = padX + 2 * charW;

        // Right-hand column first: the name gets whatever is left, and that
        // width is not fixed (a count of 999 is wider than a count of 2).
        char dbuf[12];
        const uint16_t dcol = rowDetail(kind, payload, dbuf, sizeof(dbuf));
        int px = SCREEN_W - padX;
        if (dbuf[0]) {
            px -= (int)strlen(dbuf) * charW;
            _tft->setTextColor(dcol, bg);
            _tft->setCursor(px, y + textOff);
            _tft->print(dbuf);
        }

        if (const uint16_t* icon = rowIcon(kind, payload)) {
            drawSprite(*_tft, icon, x, y + iconOff,
                       ITEM_ICON_PX, ITEM_ICON_PX);
            x += ITEM_ICON_PX + 3;
        }

        uint16_t fg = sel ? COL_CURSOR : COL_ITEM;
        if (kind == INV_ROW_NOTE) {
            fg = COL_EMPTY;
        } else if (kind == INV_ROW_RECIPE) {
            const CraftResult st = inv.check(payload);
            if (st == CRAFT_LOCKED)             fg = COL_LOCKED;
            else if (st == CRAFT_ALREADY_OWNED) fg = COL_OWNED;
        }

        char tbuf[40];
        fitText(tbuf, sizeof(tbuf), rowLabel(kind, payload),
                px - x - charW, charW);
        _tft->setTextColor(fg, bg);
        _tft->setCursor(x, y + textOff);
        _tft->print(tbuf);
    }

    drawFooter();
#endif  // !HEXHOUND_PANEL_ROUND
}

#if !HEXHOUND_PANEL_ROUND

void UIInventory::drawHeader() {
    const bool big = (SCREEN_H > 100);
    const int total = totalEntries();

    if (big) {
        uiBigHeader(*_tft, "\x04 KIT", COL_HEADER);
    } else {
        _tft->setTextSize(1);
        _tft->setTextColor(COL_HEADER, TFT_BLACK);
        _tft->setCursor(4, 2);
        _tft->print("\x04 KIT");
        _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);
    }

    if (total > INVENTORY_PER_PAGE) {
        const int page = (_scroll / INVENTORY_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / INVENTORY_PER_PAGE) + 1;
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

void UIInventory::drawFooter() {
    Inventory& inv = Inventory::instance();

    // Left: what the pet is carrying, at a glance. Right: what a long press
    // does on the highlighted row.
    char left[28];
    // Same rule as the round footer: a recipe you cannot make yet gets the line
    // to itself, because what it needs is the only actionable thing on it.
    if (!needHint(left, sizeof(left))) {
        if (inv.isEmpty()) {
            snprintf(left, sizeof(left), "nothing carried");
        } else {
            snprintf(left, sizeof(left), "%u kinds %u made",
                     (unsigned)inv.distinctMaterials(),
                     (unsigned)inv.ownedCosmetics());
        }
    }
    const char* hint = actionHint();
    const uint16_t col = inv.isEmpty() ? COL_EMPTY : COL_FOOTER;

    if (SCREEN_H > 100) {
        uiBigFooter(*_tft, left, hint, col);
        return;
    }

    const int footerY = SCREEN_H - 10;
    _tft->fillRect(0, footerY - 2, SCREEN_W, SCREEN_H - footerY + 2, TFT_BLACK);
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(col, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print(left);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(SCREEN_W - (int)strlen(hint) * 6 - 4, footerY);
    _tft->print(hint);
}

#endif  // !HEXHOUND_PANEL_ROUND

#if HEXHOUND_PANEL_ROUND

// ── Round inventory list (240x240) ────────────────────────────────────────
// Rows are centred rather than left-aligned with a "> " prefix: on a circle a
// left margin that works for the middle rows runs under the bezel on the top
// and bottom ones. Each row is the icon and the name on one size-1 line, with
// the detail on a second, because a size-2 item name is wider than most chords
// on this glass.

void UIInventory::drawHeader() {}
void UIInventory::drawFooter() {}

void UIInventory::drawRound() {
    if (!_tft) return;

    Inventory& inv = Inventory::instance();
    const int total = totalEntries();
    const int mats  = materialRows();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "KIT", COL_HEADER);

    if (total > INVENTORY_PER_PAGE) {
        const int page = (_scroll / INVENTORY_PER_PAGE) + 1;
        const int totalPages = ((total - 1) / INVENTORY_PER_PAGE) + 1;
        uiround::centerPrintf(*_tft, uiround::BODY_Y - uiround::scaled(8), 1, COL_DIM,
                              TFT_BLACK, "%d/%d", page, totalPages);
    }

    const int avail = uiround::FOOTER_DIV - uiround::BODY_Y;   // 132
    int visible = min(INVENTORY_PER_PAGE, total - _scroll);
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

        InvRowKind kind = INV_ROW_BACK;
        uint8_t    payload = 0;
        if (idx > 0) {
            if (idx <= mats) {
                const uint8_t id = nthOwnedMaterial(idx - 1);
                kind = (id == ITEM_ID_NONE) ? INV_ROW_NOTE : INV_ROW_MATERIAL;
                payload = id;
            } else {
                const int r = idx - 1 - mats;
                if (r >= (int)_orderCount) { y += rowH; continue; }
                kind = INV_ROW_RECIPE;
                payload = _order[r];
            }
        }

        if (sel) uiround::rowFill(*_tft, y, rowH, COL_CURSOR_BG, 4);

        if (kind == INV_ROW_BACK) {
            const int ty = y + (rowH - uiround::textH(2)) / 2;
            uiround::centerText(*_tft, ty, "BACK", 2,
                                sel ? COL_CURSOR : COL_ITEM, bg);
            y += rowH;
            continue;
        }

        // Two size-1 lines (8 px each) plus a 2 px gap = 18 px inside the row.
        const int ty = y + (rowH - 18) / 2;

        uint16_t fg = sel ? COL_CURSOR : COL_ITEM;
        if (kind == INV_ROW_NOTE) {
            fg = COL_EMPTY;
        } else if (kind == INV_ROW_RECIPE) {
            const CraftResult st = inv.check(payload);
            if (st == CRAFT_LOCKED)             fg = COL_LOCKED;
            else if (st == CRAFT_ALREADY_OWNED) fg = COL_OWNED;
        }

        // The icon and the name are centred together as one block, so the pair
        // stays optically centred on a chord instead of the text drifting.
        const uint16_t* icon = rowIcon(kind, payload);
        const int halfW = uiround::chordHalfW(ty);
        // Baked at the size it is drawn. This used to be ITEM_ICON_PX scaled by
        // TEXT_SCALE, which on the 480 panel meant blitting the 16 px art at 32
        // and turning every graded edge pixel into a 2x2 block. That panel now
        // has its own 32 px set, so the drawn size is unchanged and the source
        // size caught up with it. The pair is still spelled out because
        // drawHDArt() takes SOURCE and DESTINATION and confusing the two is how
        // this project once read 153 KB past the end of an array.
        const int iconPx = ITEM_ICON_PX;
        const int iconW = icon ? (iconPx + uiround::scaled(4)) : 0;

        char tbuf[40];
        fitText(tbuf, sizeof(tbuf), rowLabel(kind, payload),
                halfW * 2 - uiround::scaled(16) - iconW, uiround::cellW(1));

        const int textW = (int)strlen(tbuf) * uiround::cellW(1);
        int x0 = uiround::CX - (iconW + textW) / 2;
        if (icon) {
            drawHDArt(*_tft, icon, x0, ty - (iconPx - uiround::textH(1)) / 2,
                      ITEM_ICON_PX, iconPx);
            x0 += iconW;
        }
        // Raw setTextSize bypasses the uiround helpers, so it needs ts() by
        // hand - without it this row stayed size 1 while everything around it
        // doubled, which is exactly how the empty-kit note ended up tiny.
        _tft->setTextSize(uiround::ts(1));
        _tft->setTextColor(fg, bg);
        _tft->setCursor(x0, ty);
        _tft->print(tbuf);

        char dbuf[12];
        const uint16_t dcol = rowDetail(kind, payload, dbuf, sizeof(dbuf));
        if (dbuf[0]) uiround::centerText(*_tft, ty + uiround::scaled(10), dbuf, 1, dcol, bg);

        y += rowH;
    }

    char hintBuf[48];
    // A recipe you cannot make yet answers the only question worth asking on
    // this screen, so it takes the whole line. The carried/made summary is
    // still there for every other row.
    if (!needHint(hintBuf, sizeof(hintBuf))) {
        if (inv.isEmpty()) {
            snprintf(hintBuf, sizeof(hintBuf), "nothing carried  %s", actionHint());
        } else {
            snprintf(hintBuf, sizeof(hintBuf), "%u kinds %u made  %s",
                     (unsigned)inv.distinctMaterials(),
                     (unsigned)inv.ownedCosmetics(), actionHint());
        }
    }
    uiround::footer(*_tft, hintBuf,
                    inv.isEmpty() ? COL_EMPTY : COL_DIVIDER);
}

#endif  // HEXHOUND_PANEL_ROUND

#if HEXHOUND_HAS_TOUCH
void UIInventory::setCursor(int index) {
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
    } else if (_cursor >= _scroll + INVENTORY_PER_PAGE) {
        _scroll = _cursor - INVENTORY_PER_PAGE + 1;
    }
}
#endif

#if HEXHOUND_HAS_TOUCH
// See uiPageBy() in touch_nav.h. Guarded rather than left uncalled: an
// unreferenced method IS dropped by --gc-sections, but dropping it did not
// leave the non-touch images byte-identical when this was measured on the
// menu, so the guard is the only version that can be verified.
void UIInventory::pageDown() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), INVENTORY_PER_PAGE, 1)) draw();
}

void UIInventory::pageUp() {
    if (uiPageBy(_cursor, _scroll, totalEntries(), INVENTORY_PER_PAGE, -1)) draw();
}
#endif

void UIInventory::scrollDown() {
    const int total = totalEntries();
    if (total <= 0) return;

    _cursor = (_cursor + 1) % total;

    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + INVENTORY_PER_PAGE) {
        _scroll = _cursor - INVENTORY_PER_PAGE + 1;
    }

    draw();
}
