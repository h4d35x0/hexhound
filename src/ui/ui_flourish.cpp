#include "ui_flourish.h"

#include "animator.h"        // drawSprite()
#include "flourish_art.h"    // FLOURISH_ART_IDS / _DATA / _COUNT / _PX
#include "../board/board_profile.h"
#include "../pet/pet_core.h"
#include "../pet/pet_flourish.h"
#include "../pet/pet_inventory.h"

#if HEXHOUND_PANEL_ROUND
#include "ui_round.h"        // chordHalfW() - the glass is a circle
#endif

// ── HexHound - Flourish Rendering ───────────────────────────────
//
// See ui_flourish.h for the design and, in particular, for why the motion is
// stepped rather than smooth. Everything below is the arithmetic that makes a
// twelve-station ring land on integer pixels.

namespace {

// One beat. 500 ms, because the 480 round panel repaints its idle pet at about
// that rate and a flourish that advances faster than the pet does would be
// showing frames nobody sees. It is also slow enough that the step between
// stations reads as a deliberate move rather than a stutter.
constexpr uint32_t BEAT_MS  = 500;

// Stations round the ring. Twelve, for the same reason a clock face has twelve:
// it divides by 2, 3, 4 and 6, so a pattern of four motes, six motes or three
// motes all space evenly on the same table.
constexpr uint8_t  STATIONS = 12;

// Hard ceiling on motes alive at once, and the width of the erase record.
constexpr uint8_t  MAX_MOTES = 6;

// ── The ring is a WIDE ELLIPSE, and the shape was measured, not chosen ────
//
// Twelve stations at 30 degree steps, clockwise on the glass (screen y grows
// downwards), as cosine and sine in thousandths. The x and y radii are applied
// separately, which is the whole point: the ring has to be much wider than it
// is tall.
//
// WHY. A flourish shares the screen with a pet and with the rows of text above
// and below it, and the first cut - a square ring at 0.57 of the box, chosen so
// that every mote cleared the pet's box outright - was rendered on the 480
// round home screen and landed two packets on the "Sentinel" subtitle and
// two more on the "INITIATE R1" stats row. Measuring the clean screen said why,
// and the numbers are worth writing down because they are what these radii are:
//
//     y   6..79    outer ring and the title
//     y  92..105   the subtitle
//     y 150..284   the pet, including its shadow
//     y 312..325   the stats row
//
// The pet's BOX is y 124..300 (176 px at slotTop 124), so the gap between the
// subtitle and the box is 18 px and the gap between the box and the stats row
// is 11 px. A 20 px mote fits in neither. There is no radius at which a ring
// clears both the box and the text: that ring does not exist on this screen.
//
// What DOES exist is the gap between the text and the pet ART, which is 44 px
// above (106..149) and 27 px below (285..311), because the box is a good deal
// bigger than the picture in it. So the constraint is relaxed from "outside the
// box" to "outside the ART", and the vertical radius is set from those two
// corridors: 0.49 of the box puts the top mote at y 116..135 and the bottom one
// at 288..307, each with a few pixels of margin at both ends.
//
// That relaxation is only safe because of what the margin inside the box is
// made of. A mote is erased by painting TFT_BLACK over it, so a mote that
// overlapped the pet would punch a hole in it on any beat that was not
// immediately preceded by a pet repaint. Inside the box but outside the art
// there is nothing but black - on the 480 that region is black because the
// opaque HD idle frames paint it black every breath, and everywhere else
// because the chroma-keyed art leaves the screen background showing. So the
// erase is writing black over black, which is the only reason this is allowed
// to reach inside the box at all.
//
// The horizontal radius has no such fight: at the pet's own rows the free space
// runs from x 35 to 185 and from 294 to 445, so 0.80 of the box puts the side
// motes at x 89..108 and 371..390, clear of the pet and clear of the rim gauges
// at 8..34 and 446..472.
//
// On the 160x80 compact panel the pet sits in the top-right CORNER, so the
// three right-hand stations fall off the edge of the glass and moteFits() drops
// them. The flourish there is an arc beside the pet rather than a ring round
// it. That is the honest outcome of a 24 px pet 4 px from the panel edge, and
// it was looked at before it was accepted; what matters is that the motes that
// ARE drawn clear the stat bars, and they do.
struct Station { int16_t cos, sin; };
constexpr Station STATION[STATIONS] = {
    {  1000,     0 }, {   866,   500 }, {   500,   866 }, {     0,  1000 },
    {  -500,   866 }, {  -866,   500 }, { -1000,     0 }, {  -866,  -500 },
    {  -500,  -866 }, {     0, -1000 }, {   500,  -866 }, {   866,  -500 }
};

// How one flourish moves. Three rows, one per COSMETIC_FLOURISH item, in the
// order flourish_art.h emits them.
struct Pattern {
    uint8_t  motes;    // how many are on the ring at once
    uint8_t  spacing;  // stations between consecutive motes
    uint8_t  step;     // stations advanced per beat
    uint16_t rx;       // thousandths of the pet's box side, from its centre
    uint16_t ry;       // ditto, vertically. Always much the smaller of the two.
};

// The three patterns differ in COUNT, SPACING and STEP rather than in speed,
// because at one beat every 500 ms speed is not a dimension that survives:
// two stations per beat and one station per beat look like two different
// animations, not like a fast one and a slow one.
//
//   PACKET FLURRY  FIVE packets two stations apart, marching one station a
//                  beat. Reads as traffic circulating.
//   SOLDER SPARKS  four sparks on a diamond, stepping FIVE stations a beat.
//                  The diamond has fourfold symmetry and five is not a multiple
//                  of three, so the set takes three distinct orientations and
//                  cycles between them: the sparks snap to a new place every
//                  beat rather than sliding, which is what a spark does.
//   NOISE AURORA   four glows on ADJACENT stations, so they overlap into one
//                  band, sweeping round a station at a time.
//
// Five packets and not six, and this is the one number here that was got wrong
// first and caught by looking. Six motes two stations apart occupy every EVEN
// station; one step later they occupy every odd one; one more step and they are
// back where they started. The set maps onto itself every two beats, so what
// renders is not a march at all, it is a two-state blink, forever. Each mote
// really is advancing one station a beat - the per-mote motion was right and
// the picture was still wrong, because the eye reads the SET. Dropping to five
// leaves a gap in the ring, and the gap is what makes the rotation visible;
// the pattern now takes twelve beats to repeat instead of two.
//
// ry is the SAME for all three and is not a style choice: it is the corridor
// between the pet and the text, and there is only one of those. It was found by
// sweeping the value against a captured home screen from all FOUR panel
// families and asking whether any mote rectangle covers a lit pixel. The clear
// band is 495..510 and nothing outside it is safe:
//
//     490  nicks the pet's feet on the 320x172 panel (4 px)
//     515  nicks the pet's shadow on the 480 round panel (6 px)
//
// 502 is the middle of that band. It is a narrow window and the reason it is so
// narrow is that the same fraction has to work for a 24 px pet on a T-Dongle
// and a 176 px one on a T-RGB; the numbers above are what a change here has to
// be re-checked against, not a preference to be adjusted by eye.
//
// rx varies between the three, because sideways the room is real.
constexpr Pattern PATTERN[3] = {
    { 5, 2, 1, 800, 502 },   // cos.confetti
    { 4, 3, 5, 760, 502 },   // cos.sparks
    { 4, 1, 1, 700, 502 }    // cos.aurora
};

// Tied to the generated table rather than to the literal 3, so appending a
// fourth flourish to ITEM_DEFS and re-running the generator fails the BUILD
// here instead of silently indexing PATTERN out of range on the first frame
// somebody selects it.
static_assert(sizeof(PATTERN) / sizeof(PATTERN[0]) ==
                  sizeof(FLOURISH_ART_IDS) / sizeof(FLOURISH_ART_IDS[0]),
              "PATTERN must have one row per COSMETIC_FLOURISH item, in the "
              "order scripts/gen_flourish_art.py emits them");

// What is currently on the glass, so the next beat can take it off again.
// Positions rather than a redraw of the whole area: erasing six small squares
// is a fraction of the cost of repainting the band they sit in, and it cannot
// disturb anything else the screen has drawn there.
int16_t  s_prevX[MAX_MOTES];
int16_t  s_prevY[MAX_MOTES];
uint8_t  s_prevCount = 0;
uint32_t s_beat      = 0;
uint8_t  s_drawnId   = FLOURISH_NONE;
bool     s_haveBeat  = false;   // false until the first beat is on the glass

// Art index for an item, or -1.
//
// The generated table is keyed by STRING id and resolved to numeric ids once,
// lazily, on the first draw - the arrangement ui_worn.cpp uses and for the
// reason it gives there: item ids are indices into ITEM_DEFS, so a generated
// file holding numeric ones would be a second file to regenerate every time a
// row is appended, which is exactly the coupling item_defs.h warns about.
int artIndexFor(uint8_t itemId) {
    static uint8_t ids[FLOURISH_ART_COUNT];
    static bool    resolved = false;
    if (!resolved) {
        Inventory& inv = Inventory::instance();
        for (uint8_t i = 0; i < FLOURISH_ART_COUNT; i++) {
            ids[i] = inv.idFor(FLOURISH_ART_IDS[i]);
        }
        resolved = true;
    }
    for (uint8_t i = 0; i < FLOURISH_ART_COUNT; i++) {
        if (ids[i] != ITEM_ID_NONE && ids[i] == itemId) return (int)i;
    }
    return -1;
}

// Is a side x side square at (mx, my) somewhere it may be painted?
//
// Two separate questions, and both have bitten this codebase before. The
// rectangular one is the panel edge. The round one is the BEZEL: on a circular
// panel the corners of the framebuffer are behind plastic, so a mote that
// passes the rectangular test can still be drawn where nobody can see it - and
// worse, erased from somewhere nobody can see, which looks like nothing at all
// going wrong until a mote goes missing from the visible arc.
bool moteFits(int mx, int my, int side) {
    if (mx < 0 || my < 0) return false;
    if (mx + side > SCREEN_W || my + side > SCREEN_H) return false;
#if HEXHOUND_PANEL_ROUND
    // Both the top and the bottom row of the square have to be inside the
    // chord, because the narrower of the two is whichever is further from the
    // middle of the glass and testing only the centre row lets a corner poke
    // under the bezel.
    const int halfTop = uiround::chordHalfW(my);
    const int halfBot = uiround::chordHalfW(my + side - 1);
    const int half    = (halfTop < halfBot) ? halfTop : halfBot;
    if (half <= 0) return false;
    if (mx < uiround::CX - half) return false;
    if (mx + side > uiround::CX + half) return false;
#endif
    return true;
}

}  // namespace

void drawFlourish(TFT_eSPI& tft, int x, int y, int px, uint32_t nowMs) {
    const uint8_t id = flourishActive(PetCore::instance().state());

    const uint32_t beat = nowMs / BEAT_MS;
    // The whole cost of a call between beats: one item lookup, a divide and two
    // comparisons. This is called from the main loop on every board.
    if (s_haveBeat && beat == s_beat && id == s_drawnId) return;

    // Take the previous beat off the glass first, whatever happens next. Doing
    // this before the early return for "nothing selected" is what makes
    // turning a flourish OFF actually clear it.
    for (uint8_t i = 0; i < s_prevCount; i++) {
        tft.fillRect(s_prevX[i], s_prevY[i], FLOURISH_ART_PX, FLOURISH_ART_PX,
                     TFT_BLACK);
    }
    s_prevCount = 0;
    s_beat      = beat;
    s_drawnId   = id;
    s_haveBeat  = true;

    if (id == FLOURISH_NONE || px <= 0) return;
    const int art = artIndexFor(id);
    if (art < 0) return;

    const Pattern& p = PATTERN[art];
    const int side   = FLOURISH_ART_PX;
    const int cx     = x + px / 2;
    const int cy     = y + px / 2;

    const uint8_t motes = (p.motes < MAX_MOTES) ? p.motes : MAX_MOTES;
    for (uint8_t m = 0; m < motes; m++) {
        const uint8_t st =
            (uint8_t)(((uint32_t)beat * p.step + (uint32_t)m * p.spacing)
                      % STATIONS);

        // int32 throughout: the widest product is 1000 * 800 * 176 on the T-RGB,
        // which is 140 million and overflows a 16-bit intermediate by four
        // orders of magnitude. Rounded to nearest rather than truncated, the
        // same argument ui_worn.cpp makes - on a 24 px pet a thousandth is a
        // quarter of a pixel and truncating every axis the same way walks the
        // whole ring up and left.
        const int32_t sx = (int32_t)STATION[st].cos * (int32_t)p.rx
                         * (int32_t)px;
        const int32_t sy = (int32_t)STATION[st].sin * (int32_t)p.ry
                         * (int32_t)px;
        const int ox = (int)((sx + (sx >= 0 ? 500000 : -500000)) / 1000000);
        const int oy = (int)((sy + (sy >= 0 ? 500000 : -500000)) / 1000000);

        const int mx = cx + ox - side / 2;
        const int my = cy + oy - side / 2;
        if (!moteFits(mx, my, side)) continue;

        drawSprite(tft, FLOURISH_ART_DATA[art], mx, my, side, side);
        // Recorded ONLY when it was actually drawn. A mote skipped by
        // moteFits() must not enter the erase list, or the next beat clears a
        // rectangle this module never painted - which on the compact panel is
        // somebody else's stat bar.
        s_prevX[s_prevCount] = (int16_t)mx;
        s_prevY[s_prevCount] = (int16_t)my;
        s_prevCount++;
    }
}

uint8_t flourishSignature() {
    return flourishActive(PetCore::instance().state());
}

void clearFlourish(TFT_eSPI& tft) {
    for (uint8_t i = 0; i < s_prevCount; i++) {
        tft.fillRect(s_prevX[i], s_prevY[i], FLOURISH_ART_PX, FLOURISH_ART_PX,
                     TFT_BLACK);
    }
    s_prevCount = 0;
    s_haveBeat  = false;
}

void flourishForget() {
    // Drop the erase record without erasing: the caller is telling us those
    // pixels are already gone. s_haveBeat goes false so the next call paints
    // immediately rather than waiting out the remainder of the current beat,
    // which after a screen switch would be up to half a second of nothing.
    s_prevCount = 0;
    s_haveBeat  = false;
}
