#include "ui_menu.h"
#include "ui_utils.h"
#if HEXHOUND_HAS_TOUCH
#include "touch_nav.h"   // uiRowAtY
#endif
#include "ui_round.h"
#include "../pet/pet_core.h"
#include "../modules/battery_module.h"
#include "../config.h"

// ── HexHound - Menu + Stats Implementation ──────────────────────

#define COL_HEADER    0x07FF  // cyan
#define COL_ITEM      0xFFFF  // white
#define COL_CURSOR    0x07FF  // cyan (selected indicator)
#define COL_CURSOR_BG 0x2945  // dark blue highlight
#define COL_DIM       0xFFFF  // white
#define COL_DIVIDER   0x07FF  // cyan
#define COL_FOOTER    0xFFFF  // white

#define COL_GREEN     0x07E0
#define COL_AMBER     0xFD20
#define COL_RED       0xF800
#define COL_BAR_BG    0x39E7
// Mid grey. A row that is visible but not yet earned reads as unavailable at a
// glance, without needing the suffix to be legible first.
#define COL_LOCKED    0x7BEF

// Indexed by MenuItem value, NOT by display order - MENU_ORDER decides what
// appears where. Every entry appended to the MenuItem enum needs a label here
// at the matching position or the draw path reads past the end of this array.
static const char* MENU_LABELS[] = {
    "PATROL",         // MENU_PATROL
    "JOURNAL",        // MENU_JOURNAL
    "CONFIG",         // MENU_CONFIG
    "PET STATS",      // MENU_STATS
    "FLIP DISPLAY",   // MENU_ROTATE_DISPLAY
    "MISSIONS",       // MENU_MISSIONS
    "QUESTS",         // MENU_QUESTS
    "GAMES",          // MENU_GAMES
    "INVENTORY",      // MENU_INVENTORY
    "DEN",            // MENU_DEN
    "ROAM",           // MENU_ROAM
    "REPORT",         // MENU_REPORT
    "UPDATE",         // MENU_UPDATE
    "CLOSET"          // MENU_CLOSET
};
static_assert(sizeof(MENU_LABELS) / sizeof(MENU_LABELS[0]) == MENU_ITEM_COUNT_ALL,
              "MENU_LABELS must have one entry per MenuItem, in enum order");

static const char* traitName(Trait trait) {
    static const char* traitNames[] = {
        "Curious", "Protect", "Chaotic", "Sleepy", "Greedy", "Brave"
    };
    return traitNames[trait];
}

static const char* perkName(PetPerk perk) {
    switch (perk) {
        case PERK_SIGNAL_CARTOGRAPHER: return "Cartographer";
        case PERK_BEACON_HUNTER:       return "Beacon Hunter";
        case PERK_ANOMALY_ARCHIVIST:   return "Archivist";
        case PERK_MISSION_OPERATOR:    return "Operator";
        case PERK_FIELD_SENTINEL:      return "Field Sentinel";
        default:                       return "";
    }
}

// Display order, top to bottom. Missions sits high because it is the headline
// capability once unlocked; Flip Display is a settings afterthought and goes
// last.
//
// This used to be a count ("show the first N items"), which only worked while
// the one gated entry happened to be last in the enum. Moving Missions up would
// have silently shown the wrong row, so visibility is now an explicit filter.
static const MenuItem MENU_ORDER[] = {
    MENU_PATROL,
    MENU_MISSIONS,        // shown always, LOCKED until Gremlin Mode
    MENU_QUESTS,          // daily quests: the reason to check in at all
    MENU_ROAM,
    MENU_GAMES,
    MENU_DEN,
    MENU_CLOSET,          // beside the den: one dresses the pet, one its room
    MENU_INVENTORY,
    MENU_REPORT,
    MENU_JOURNAL,
    MENU_CONFIG,
    MENU_STATS,
    MENU_UPDATE,          // rare, deliberate, and near the settings end
    MENU_ROTATE_DISPLAY,  // last
};

static bool menuItemVisible(MenuItem item) {
    if (item == MENU_MISSIONS) {
#if !HEXHOUND_HAS_USB_HID
        // Boards whose USB-C port is a UART bridge rather than a native USB
        // device port (Waveshare ESP32-S3-LCD-1.28) have no host to type into,
        // so the missions subsystem can never run there. Hide it rather than
        // offering a menu entry that dead-ends.
        //
        // This is the ONE case that stays hidden. It is not a progression gate
        // and no amount of XP will ever open it - the hardware cannot do it.
        return false;
#endif
    }
    return true;
}

// Visible, but not yet earned. Drawn greyed with a LOCKED suffix, and
// selecting it says what would open it instead of doing nothing.
//
// This used to be folded into menuItemVisible(), so an unearned MISSIONS row
// simply was not there. That reads as a missing feature rather than a locked
// one - the owner's report was "I don't even see Missions in the Menu" - and a
// list whose LENGTH changes as the pet grows also silently repoints every row
// index below it. Splitting the two means the row keeps its place and says
// what it wants.
static bool menuItemLocked(MenuItem item) {
#if HEXHOUND_HAS_USB_HID
    if (item == MENU_MISSIONS) {
        return PetCore::instance().state().stage < STAGE_GREMLIN;
    }
#else
    (void)item;
#endif
    return false;
}

// The label a row draws, with the LOCKED suffix when it has one.
//
// Written into caller storage rather than a static buffer: three panel
// families call this from their own draw loops, and a shared buffer is exactly
// the kind of thing two of them would end up pointing at simultaneously.
static const char* rowLabel(MenuItem item, char* buf, size_t n) {
    if (!menuItemLocked(item)) {
        return MENU_LABELS[item];
    }
    snprintf(buf, n, "%s LOCKED", MENU_LABELS[item]);
    return buf;
}

// Fills `out` with the currently visible items in display order, returns count.
static int visibleMenuItems(MenuItem* out) {
    int n = 0;
    for (MenuItem item : MENU_ORDER) {
        if (menuItemVisible(item)) {
            out[n++] = item;
        }
    }
    return n;
}

static int activeMenuCount() {
    MenuItem items[MENU_ITEM_COUNT_ALL];
    return visibleMenuItems(items);
}

UIMenu& UIMenu::instance() {
    static UIMenu ui;
    return ui;
}

void UIMenu::init(TFT_eSPI* tft) {
    _tft = tft;
}

// How many rows fit between the header and the footer rule on this panel.
// Derived rather than hardcoded, so adding a menu entry can never silently
// push the last one under the footer the way it used to.
int UIMenu::rowsPerPage() {
#if HEXHOUND_PANEL_ROUND
    const int avail = uiround::FOOTER_DIV - uiround::BODY_Y;
    // Scaled, not literal: this decides how many rows fit on a PAGE, so leaving
    // it at 26 gave the 480 panel ten rows where the 240 panel gets five, and
    // then crammed doubled text into undoubled rows.
    const int rowH  = uiround::scaled(26);      // size-2 text plus breathing room
#else
    if (SCREEN_H > 100) {
        const int avail = uilg::FOOTER_DIV - uilg::BODY_Y;
        const int rowH  = 22;                   // size-2 text, matches the cap below
        int n = avail / rowH;
        return n < 1 ? 1 : n;
    }
    const int avail = (SCREEN_H - 10) - 16;     // footer rule at SCREEN_H-10, body at 16
    const int rowH  = 9;                        // size-1 text pitch
#endif
    int n = avail / rowH;
    return n < 1 ? 1 : n;
}

void UIMenu::ensureCursorVisible() {
    const int perPage = rowsPerPage();
    if (_cursor < _scroll) {
        _scroll = _cursor;
    } else if (_cursor >= _scroll + perPage) {
        _scroll = _cursor - perPage + 1;
    }
    if (_scroll < 0) _scroll = 0;
}

void UIMenu::open() {
    _cursor = 0;
    _scroll = 0;
    draw();
}

void UIMenu::draw() {
#if HEXHOUND_PANEL_ROUND
    drawRound();
#else
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);

    if (SCREEN_H > 100) {
        // ── Large panel (320x172): size-2 rows filling the body area ──────────
        uiBigHeader(*_tft, "\x04 HEXHOUND MENU", COL_HEADER);

        MenuItem items[MENU_ITEM_COUNT_ALL];
        int count = visibleMenuItems(items);
        int perPage = rowsPerPage();
        int shown = count < perPage ? count : perPage;
        int rowH  = (uilg::FOOTER_DIV - uilg::BODY_Y) / (shown > 0 ? shown : 1);
        if (rowH > 22) rowH = 22;               // keep 16px text snug
        int textOff = (rowH - uilg::CHAR_H) / 2;                 // vertical centering
        if (textOff < 0) textOff = 0;

        int first = _scroll;
        if (first > count - perPage) first = count - perPage;
        if (first < 0) first = 0;
        int last = first + perPage;
        if (last > count) last = count;

#if HEXHOUND_HAS_TOUCH
        _rowY0 = uilg::BODY_Y;
        _rowH = rowH;
        _rowFirst = first;
        _rowCount = last - first;
#endif

        _tft->setTextSize(2);
        int y = uilg::BODY_Y;
        for (int i = first; i < last; i++) {
            char lb[24];
            const char* label = rowLabel(items[i], lb, sizeof(lb));
            const bool locked = menuItemLocked(items[i]);
            if (i == _cursor) {
                _tft->fillRect(0, y, SCREEN_W, rowH, COL_CURSOR_BG);
                _tft->setTextColor(locked ? COL_LOCKED : COL_CURSOR, COL_CURSOR_BG);
                _tft->setCursor(uilg::PAD, y + textOff);
                _tft->print("> ");
                _tft->print(label);
            } else {
                _tft->setTextColor(locked ? COL_LOCKED : COL_ITEM, TFT_BLACK);
                _tft->setCursor(uilg::PAD + 2 * uilg::CHAR_W, y + textOff);
                _tft->print(label);
            }
            y += rowH;
        }

#if HEXHOUND_HAS_TOUCH
        // Same gap as the round menu above: this is the only screen with no
        // printed way out, and on a touch panel the way out is the footer.
        uiBigFooter(*_tft, "tap=open", "bot=back", COL_DIVIDER);
#else
        uiBigFooter(*_tft, "scroll", "hold=sel", COL_DIVIDER);
#endif
        return;
    }

    // Header
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 HEXHOUND MENU");
    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);

    // Menu items. Spacing tightens for longer lists, and anything past a
    // pageful scrolls rather than drawing over the footer.
    MenuItem items[MENU_ITEM_COUNT_ALL];
    int count = visibleMenuItems(items);
    int perPage = rowsPerPage();
    int sp = (count > 5) ? 9 : (count > 4 ? 11 : 13);
    int first = _scroll;
    if (first > count - perPage) first = count - perPage;
    if (first < 0) first = 0;
    int last = first + perPage;
    if (last > count) last = count;
    int y = 16;
    for (int i = first; i < last; i++) {
        char lb[24];
        const char* label = rowLabel(items[i], lb, sizeof(lb));
        const bool locked = menuItemLocked(items[i]);
        if (i == _cursor) {
            _tft->fillRect(0, y - 1, SCREEN_W, sp, COL_CURSOR_BG);
            _tft->setTextColor(locked ? COL_LOCKED : COL_CURSOR, COL_CURSOR_BG);
            _tft->setCursor(8, y);
            _tft->print("> ");
            _tft->print(label);
        } else {
            _tft->setTextColor(locked ? COL_LOCKED : COL_ITEM, TFT_BLACK);
            _tft->setCursor(14, y);
            _tft->print(label);
        }
        y += sp;
    }

    // Footer - clear the full footer strip to prevent menu item text bleed
    int footerY = SCREEN_H - 10;
    _tft->fillRect(0, footerY - 2, SCREEN_W, SCREEN_H - footerY + 2, TFT_BLACK);
    _tft->drawFastHLine(0, footerY - 2, SCREEN_W, COL_DIVIDER);
    _tft->setTextSize(1);
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(4, footerY);
    _tft->print("scroll");
    _tft->setCursor(SCREEN_W - 54, footerY);
    _tft->print("hold=sel");
#endif // !HEXHOUND_PANEL_ROUND
}

#if HEXHOUND_PANEL_ROUND

// ── Round menu (240x240) ──────────────────────────────────────────────────
// Centered rows with chord-clipped selection capsules. The label is centered
// rather than left-aligned with a "> " prefix: on a circle a left margin that
// works for the middle rows runs under the bezel on the top and bottom ones.

void UIMenu::drawRound() {
    if (!_tft) return;

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "MENU", COL_HEADER);

    MenuItem items[MENU_ITEM_COUNT_ALL];
    int count = visibleMenuItems(items);
    if (count <= 0) return;

    int avail = uiround::FOOTER_DIV - uiround::BODY_Y;   // 132
    int perPage = rowsPerPage();
    int shown = count < perPage ? count : perPage;
    int rowH  = avail / (shown > 0 ? shown : 1);
    if (rowH > uiround::scaled(26)) rowH = uiround::scaled(26);
    int textOff = (rowH - uiround::textH(2)) / 2;
    if (textOff < 0) textOff = 0;

    int first = _scroll;
    if (first > count - perPage) first = count - perPage;
    if (first < 0) first = 0;
    int last = first + perPage;
    if (last > count) last = count;

    // Center the block vertically in the body area so short menus sit on the
    // widest part of the glass instead of hugging the header.
    int y = uiround::BODY_Y + (avail - rowH * shown) / 2;

#if HEXHOUND_HAS_TOUCH
    // Recorded here, where the numbers are known, so a tap is hit-tested
    // against the rows the owner can actually see. See rowIndexAtY().
    _rowY0 = y;
    _rowH = rowH;
    _rowFirst = first;
    _rowCount = last - first;
#endif

    for (int i = first; i < last; i++) {
        char lb[24];
        const char* label = rowLabel(items[i], lb, sizeof(lb));
        const bool locked = menuItemLocked(items[i]);
        if (i == _cursor) {
            uiround::rowFill(*_tft, y, rowH, COL_CURSOR_BG, 4);
            uiround::centerText(*_tft, y + textOff, label, 2,
                                locked ? COL_LOCKED : COL_CURSOR, COL_CURSOR_BG);
        } else {
            uiround::centerText(*_tft, y + textOff, label, 2,
                                locked ? COL_LOCKED : COL_ITEM, TFT_BLACK);
        }
        y += rowH;
    }

    // ── The one screen that never said how to leave it ────────────────────
    // Every LEAF screen prints "hold=back", so the footer-strip Back zone is an
    // affordance the owner has been reading since the first boot - except here,
    // on the one screen where it was never printed. The menu has no BACK row
    // (see MENU_ORDER), the round boards have no second button, and the menu is
    // opened with no timeout, so tapping this strip is the ONLY way back to the
    // pet, and it was the only screen not saying so. Reported from the field as
    // "from the menu how do you get back to the main screen?".
    //
    // Guarded so the boards with no touch panel keep the exact string they had:
    // there is no footer strip to tap there, and adding touch must not move a
    // byte of a board that has none.
#if HEXHOUND_HAS_TOUCH
    uiround::footer(*_tft, "tap=open   bottom=back", COL_DIVIDER);
#else
    uiround::footer(*_tft, "press=scroll  hold=select", COL_DIVIDER);
#endif
}

#endif // HEXHOUND_PANEL_ROUND

void UIMenu::scrollDown() {
    const int count = activeMenuCount();
    if (count <= 0) return;
    _cursor = (_cursor + 1) % count;
    // Wrapping to the top must bring the window with it, or the cursor lands
    // on row 0 while the screen still shows the bottom of the list.
    if (_cursor == 0) _scroll = 0;
    ensureCursorVisible();
    draw();
}

// ── Touch-only additions ──────────────────────────────────────────────────
// See the note in ui_menu.h for why these are guarded rather than left
// uncalled: --gc-sections drops them, but not byte-identically.

#if HEXHOUND_HAS_TOUCH
// Page the window by whole screenfuls. See the long note in ui_menu.h.
//
// The clamping itself lives in uiPageBy() in touch_nav.h, shared with the four
// sub-lists that page identically. It used to be a private copy here, which is
// the second copy of the same arithmetic that the row hit test already warns
// about: it drifts silently, because a list that pages one row short still
// looks like a list that works.
void UIMenu::pageBy(int deltaPages) {
    if (uiPageBy(_cursor, _scroll, activeMenuCount(), rowsPerPage(), deltaPages)) {
        draw();
    }
}

void UIMenu::pageDown() { pageBy(1); }
void UIMenu::pageUp()   { pageBy(-1); }

void UIMenu::setCursor(int index) {
    const int count = activeMenuCount();
    if (count <= 0) return;
    // Clamped rather than asserted. The index arrives from a hit test against a
    // list whose length is not constant - MISSIONS appears the moment the pet
    // reaches Gremlin - so an out-of-range row is a stale draw, not a bug worth
    // dropping the owner's tap over.
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    _cursor = index;
    ensureCursorVisible();
}

int UIMenu::rowIndexAtY(int y) const {
    const int i = uiRowAtY(y, _rowY0, _rowH, _rowCount);
    return i < 0 ? -1 : _rowFirst + i;
}

#endif // HEXHOUND_HAS_TOUCH

// Whether the row under the cursor is visible-but-not-yet-earned.
//
// Asked by the caller BEFORE it acts on select(), so a locked row explains
// itself instead of opening a screen the pet has not earned. Deliberately not
// folded into select(): select() answers "which row", and a caller that
// forgets to ask this gets the old behaviour rather than a wrong row.
bool UIMenu::selectedIsLocked() const {
    MenuItem items[MENU_ITEM_COUNT_ALL];
    const int count = visibleMenuItems(items);
    if (count <= 0) return false;
    int idx = _cursor;
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    return menuItemLocked(items[idx]);
}

MenuItem UIMenu::select() const {
    MenuItem items[MENU_ITEM_COUNT_ALL];
    int count = visibleMenuItems(items);
    if (count <= 0) return MENU_PATROL;
    int idx = _cursor;
    if (idx < 0 || idx >= count) idx = 0;
    return items[idx];
}

// ══════════════════════════════════════════════════════════════════════════
//  PET STATS SCREEN
// ══════════════════════════════════════════════════════════════════════════

void UIMenu::drawStats() {
#if HEXHOUND_PANEL_ROUND
    drawStatsRound();
#else
    if (!_tft) return;

    const auto& pet = PetCore::instance();
    const auto& st  = pet.state().stats;
    int batteryPct = BatteryModule::instance().isAvailable()
        ? BatteryModule::instance().percent()
        : -1;
    uint32_t masteryCurrent = pet.state().masteryXP - pet.masteryForCurrentRank();
    uint32_t masteryNeeded = pet.masteryForNextRank() - pet.masteryForCurrentRank();

    _tft->fillScreen(TFT_BLACK);

    if (SCREEN_H > 100) {
        // ── Large panel (320x172): size-2 layout across the full screen ───────
        uiBigHeader(*_tft, "\x04 PET STATS", COL_HEADER);

        // Battery in the header row (right-aligned, size 2)
        if (batteryPct >= 0) {
            char batStr[12];
            snprintf(batStr, sizeof(batStr), "BAT %d%%", batteryPct);
            int bw = (int)strlen(batStr) * uilg::CHAR_W;
            _tft->setTextSize(2);
            _tft->setTextColor(COL_DIM, TFT_BLACK);
            _tft->setCursor(SCREEN_W - bw - uilg::PAD, uilg::TITLE_Y);
            _tft->print(batStr);
        }

        // Stage line (size 2)
        _tft->setTextSize(2);
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y);
        _tft->print("Stage: ");
        _tft->setTextColor(COL_AMBER, TFT_BLACK);
        _tft->print(STAGE_NAMES[pet.state().stage - 1]);

        // XP line (size 2)
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y + 18);
        _tft->print("XP: ");
        _tft->setTextColor(COL_CURSOR, TFT_BLACK);
        if (pet.state().stage >= STAGE_SENTINEL) {
            _tft->printf("%lu MAX", st.xp);
        } else {
            _tft->printf("%lu/%lu", st.xp, pet.xpForNextStage());
        }

        // Mastery line (size 2)
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(uilg::PAD, uilg::BODY_Y + 36);
        _tft->print("Mastery: ");
        _tft->setTextColor(COL_CURSOR, TFT_BLACK);
        if (pet.state().stage >= STAGE_SENTINEL) {
            _tft->printf("%s R%u", pet.masteryTitle(), pet.state().masteryRank + 1);
        } else {
            _tft->print("LOCKED");
        }

        // Stat bars - two columns, taller/wider, size-2 labels.
        // Left column: Hunger, Mood, Energy   Right: Trust, Mischief, Health
        int barY  = uilg::BODY_Y + 58;   // 92
        int rowSp = 18;
        int barW  = 92;
        int barH  = 10;
        int colL  = uilg::PAD;
        int colR  = SCREEN_W / 2 + uilg::PAD;
        drawStatBar(colL, barY,           barW, barH, st.hunger,   "Hun");
        drawStatBar(colL, barY + rowSp,   barW, barH, st.mood,     "Mod");
        drawStatBar(colL, barY + rowSp*2, barW, barH, st.energy,   "Nrg");
        drawStatBar(colR, barY,           barW, barH, st.trust,    "Tru");
        drawStatBar(colR, barY + rowSp,   barW, barH, st.mischief, "Mis");
        drawStatBar(colR, barY + rowSp*2, barW, barH, st.health,   "Hlt");

        // Secondary info at size 1 (Traits / Codex / Perks) below the bars.
        int infoY = barY + rowSp*3 + 2;  // ~148 -> keep under footer rule
        if (infoY > uilg::FOOTER_DIV - 8) infoY = uilg::FOOTER_DIV - 8;
        _tft->setTextSize(1);
        _tft->setTextColor(COL_DIM, TFT_BLACK);
        _tft->setCursor(uilg::PAD, infoY);
        _tft->printf("Traits: %s, %s  Codex WiFi:%u BLE:%u Open:%u  Perks:%u",
                     traitName(pet.state().traits[0]),
                     traitName(pet.state().traits[1]),
                     pet.state().seenWifiCount,
                     pet.state().seenBleCount,
                     pet.state().threatOpenCount,
                     pet.unlockedPerkCount());

        uiBigFooter(*_tft, nullptr, "hold=back", COL_FOOTER);
        return;
    }

    // Header
    _tft->setTextSize(1);
    _tft->setTextColor(COL_HEADER, TFT_BLACK);
    _tft->setCursor(4, 2);
    _tft->print("\x04 PET STATS");
    _tft->drawFastHLine(0, 12, SCREEN_W, COL_DIVIDER);

    // Stage
    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    _tft->setCursor(4, 16);
    _tft->print("Stage:");
    _tft->setTextColor(COL_CURSOR, TFT_BLACK);
    _tft->setCursor(44, 16);
    _tft->print(STAGE_NAMES[pet.state().stage - 1]);

    if (batteryPct >= 0) {
        _tft->setTextColor(COL_ITEM, TFT_BLACK);
        _tft->setCursor(SCREEN_W - 54, 16);
        _tft->printf("BAT %d%%", batteryPct);
    }

    // Compact 160x80 layout: prioritize readability and avoid overlap.
    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    _tft->setCursor(4, 26);
    if (pet.state().stage >= STAGE_SENTINEL) {
        _tft->printf("MR:%u %lu/%lu", pet.state().masteryRank + 1,
                     masteryCurrent,
                     masteryNeeded);
    } else {
        _tft->printf("XP:%lu/%lu", st.xp, pet.xpForNextStage());
    }

    // Stat bars - two columns, compact layout (fits 160x80)
    // Left column: Hunger, Mood, Energy
    // Right column: Trust, Mischief, Health
    int barY = 38;
    int sp   = 8;
    int barW = 28;
    int barH = 3;
    drawStatBar(2,  barY,        barW, barH, st.hunger,   "Hun");
    drawStatBar(2,  barY + sp,   barW, barH, st.mood,     "Mod");
    drawStatBar(2,  barY + sp*2, barW, barH, st.energy,   "Nrg");
    drawStatBar(82, barY,        barW, barH, st.trust,    "Tru");
    drawStatBar(82, barY + sp,   barW, barH, st.mischief, "Mis");
    drawStatBar(82, barY + sp*2, barW, barH, st.health,   "Hlt");

    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    _tft->setCursor(4, 64);
    _tft->printf("Cx:%u Pk:%u",
                 pet.totalThreatFindings(),
                 pet.unlockedPerkCount());

    // Footer
    _tft->setTextColor(COL_FOOTER, TFT_BLACK);
    _tft->setCursor(SCREEN_W - 54, SCREEN_H - 8);
    _tft->print("hold=back");
#endif // !HEXHOUND_PANEL_ROUND
}

#if HEXHOUND_PANEL_ROUND

// ── Round pet stats (240x240) ─────────────────────────────────────────────
// Six stat rows stacked down the middle of the glass. Each row is a fixed
// 150px content block centered on CX, which is inside the chord for every row
// the block occupies (narrowest is y=182: 190px usable).

void UIMenu::drawStatsRound() {
    if (!_tft) return;

    const auto& pet = PetCore::instance();
    const auto& st  = pet.state().stats;
    int batteryPct = BatteryModule::instance().isAvailable()
        ? BatteryModule::instance().percent()
        : -1;
    uint32_t masteryCurrent = pet.state().masteryXP - pet.masteryForCurrentRank();
    uint32_t masteryNeeded  = pet.masteryForNextRank() - pet.masteryForCurrentRank();

    _tft->fillScreen(TFT_BLACK);
    uiround::header(*_tft, "PET STATS", COL_HEADER);

    uiround::centerText(*_tft, uiround::scaled(56), STAGE_NAMES[pet.state().stage - 1], 1,
                        COL_AMBER, TFT_BLACK);

    if (pet.state().stage >= STAGE_SENTINEL) {
        uiround::centerPrintf(*_tft, uiround::scaled(68), 1, COL_CURSOR, TFT_BLACK,
                              "%s R%u  %lu/%lu",
                              pet.masteryTitle(), pet.state().masteryRank + 1,
                              (unsigned long)masteryCurrent,
                              (unsigned long)masteryNeeded);
    } else {
        uiround::centerPrintf(*_tft, uiround::scaled(68), 1, COL_CURSOR, TFT_BLACK,
                              "XP %lu/%lu",
                              (unsigned long)st.xp,
                              (unsigned long)pet.xpForNextStage());
    }

    int y = uiround::scaled(84);
    const int sp = uiround::scaled(15);
    drawStatRowRound(y,          st.hunger,   "Hun");
    drawStatRowRound(y + sp,     st.mood,     "Mod");
    drawStatRowRound(y + sp * 2, st.energy,   "Nrg");
    drawStatRowRound(y + sp * 3, st.trust,    "Tru");
    drawStatRowRound(y + sp * 4, st.mischief, "Mis");
    drawStatRowRound(y + sp * 5, st.health,   "Hlt");

    // Keep clear of the footer rule at y=190: the last stat row ends at 167.
    uiround::centerPrintf(*_tft, y + sp * 6 + uiround::scaled(4), 1, COL_DIM, TFT_BLACK,
                          "Cx %u   Perks %u",
                          pet.totalThreatFindings(),
                          pet.unlockedPerkCount());

    if (batteryPct >= 0) {
        char hint[24];
        snprintf(hint, sizeof(hint), "BAT %d%%   hold=back", batteryPct);
        uiround::footer(*_tft, hint, COL_FOOTER);
    } else {
        uiround::footer(*_tft, "hold=back", COL_FOOTER);
    }
}

void UIMenu::drawStatRowRound(int y, int val, const char* label) {
    // label(18) + gap(4) + bar(96) + gap(6) + value(18) = 142, centered.
    // Every figure is the 240 tuning, scaled for the panel actually in use.
    const int barW = uiround::scaled(96);
    const int barH = uiround::scaled(6);
    const int blockW = uiround::scaled(142);
    const int x0 = uiround::CX - blockW / 2;

    _tft->setTextSize(uiround::ts(1));
    _tft->setTextColor(COL_DIM, TFT_BLACK);
    _tft->setCursor(x0, y);
    _tft->print(label);

    const int barX = x0 + uiround::scaled(22);
    const int barY = y + uiround::scaled(1);
    _tft->fillRect(barX, barY, barW, barH, COL_BAR_BG);

    uint16_t barColor;
    if (val > 60)       barColor = COL_GREEN;
    else if (val >= 30) barColor = COL_AMBER;
    else                barColor = COL_RED;

    int filled = (val * barW) / 100;
    if (filled > 0) {
        _tft->fillRect(barX, barY, filled, barH, barColor);
    }

    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    _tft->setCursor(barX + barW + uiround::scaled(6), y);
    _tft->printf("%d", val);
}

#endif // HEXHOUND_PANEL_ROUND

void UIMenu::drawStatBar(int x, int y, int w, int h, int val, const char* label) {
    _tft->setTextSize(1);

    // Label (3 chars)
    _tft->setTextColor(COL_DIM, TFT_BLACK);
    _tft->setCursor(x, y - 1);
    _tft->print(label);

    // Bar background
    int barX = x + 22;
    _tft->fillRect(barX, y, w, h, COL_BAR_BG);

    // Color based on value
    uint16_t barColor;
    if (val > 60)      barColor = COL_GREEN;
    else if (val >= 30) barColor = COL_AMBER;
    else                barColor = COL_RED;

    // Filled portion
    int filled = (val * w) / 100;
    if (filled > 0) {
        _tft->fillRect(barX, y, filled, h, barColor);
    }

    // Value text
    _tft->setTextColor(COL_ITEM, TFT_BLACK);
    _tft->setCursor(barX + w + 3, y - 1);
    _tft->printf("%d", val);
}
