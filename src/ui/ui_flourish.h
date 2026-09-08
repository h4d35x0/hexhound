#pragma once

// tft_compat.h on every build, not just the simulator. <Arduino.h> does not
// declare TFT_eSPI, so guarding this the other way compiled fine in the sim and
// failed on all eight boards at once. Same include the other ui_*.h use, and
// the same mistake ui_worn.h records having made.
#include "../hal/tft_compat.h"

#include <cstdint>

// ── HexHound - Drawing the pet's flourish ───────────────────────
//
// One entry point, called by every screen that draws the resting pet, right
// after the pet and its worn cosmetics have been blitted. It reads
// PetState::flourishSlot through the rules in pet/pet_flourish.h and rides a
// few copies of the item's mote (ui/flourish_art.h) around the pet's box.
//
// ── A flourish is not a worn overlay ──────────────────────────────────────
//
// ui_worn.h places art ON a body part and is finished. This is the other kind
// of decoration: nothing is attached to the pet, the picture is what happens
// AROUND it, and it only exists while it is moving. So the two modules share an
// interface shape (a box, a size, one call after the pet is drawn) and nothing
// else, and this one owns a clock and a scrap of memory of what it last put on
// the glass.
//
// ── The frame rate this was designed for: 2 fps ───────────────────────────
//
// The 480 round panel repaints its idle pet roughly TWICE A SECOND. That is
// the number this animation is built around, and it rules out the obvious
// design: a cloud of particles on smooth continuous paths, sampled at 2 Hz,
// does not read as slow motion, it reads as a system that is broken. Every
// frame lands somewhere unrelated to the last one and the eye gets no motion
// out of it at all.
//
// So the motion is STEPPED on purpose. Motes do not drift; they OCCUPY twelve
// fixed stations on a ring, and on each 500 ms beat every mote moves to
// another station. A chase light, a rotating radar sweep and a clock hand all
// work this way and all read correctly at one or two frames a second, because
// the discreteness is the design rather than an artefact of missing frames.
// The same code at 30 fps would look identical; it just would not be redrawing.
//
// The consequences of that decision, which are the reason it is worth stating:
//
//   * At most six motes exist, in a fixed array. No allocation, ever - this
//     runs on a board with no PSRAM and a heap the renderer must not fragment.
//   * A beat costs at most twelve small blits (six erases, six draws) every
//     500 ms. Between beats drawFlourish() reads a clock, compares two values
//     and returns, so it is safe to call from the main loop at any rate.
//   * Positions are integer arithmetic off a 12-entry table. No trigonometry,
//     no floating point, nothing that has to be recomputed per frame.
//
// ── It draws nothing when nothing is selected ─────────────────────────────
//
// The common case for a long time: the cheapest flourish is Gremlin-gated.
// It costs one byte read and a comparison.
//
// ── Sizes ─────────────────────────────────────────────────────────────────
//
// `px` is the size the PET was drawn at, not the size of a mote. The ring
// radius is a fraction of the pet's own box, so this works unchanged for a
// 24 px pet on a T-Dongle and a 176 px portrait on the T-RGB, with no
// per-screen tuning anywhere. The mote itself is drawn at the size it was
// BAKED at for this panel family (FLOURISH_ART_PX), never resampled, which is
// the rule item_art.h settled on: bake at the size you draw.
//
// ── The background it erases to, and the one thing a caller must do ───────
//
// The previous beat's motes are erased by painting TFT_BLACK over them. Every
// screen that draws the pet defines its own COL_BG as TFT_BLACK (ui_home.cpp,
// ui_patrol_hud.cpp), so that is not so much an assumption as the one colour
// the UI has; a screen with a different background would need a bg parameter
// here, not a special case.
//
// The ring is placed so that a mote never lands on the pet ITSELF, only on
// screen background or on the black margin between the pet art and the edge of
// its box. That is a hard constraint rather than a nicety: black over the pet
// would be a hole punched in it. See the geometry note in ui_flourish.cpp for
// the measurements the radii come from.
//
// THE CALLER'S ONE OBLIGATION: whenever you repaint the pet, call
// flourishForget() before the next drawFlourish(). A pet frame paints over the
// motes standing in its box, and without being told, this module would erase
// the rectangles they used to be in - which by then belong to the pet. It is
// the same obligation ui_home.cpp already meets for worn overlays, where the
// comment reads "over each breath frame, not once".

// Draw the pet's flourish around art already blitted at (x, y) at px square.
// Cheap to call every frame: it advances only when its own beat has ticked.
void drawFlourish(TFT_eSPI& tft, int x, int y, int px, uint32_t nowMs);

// Changes whenever the SELECTED flourish changes. Screens that skip a redraw
// while the pet is unchanged fold this into their cache alongside
// wornSignature(), so choosing a flourish repaints the pet block.
//
// Deliberately NOT a function of the animation phase. A signature that ticked
// with the beat would make every one of those screens repaint the whole pet
// twice a second, which is exactly the flashing the battery-noise fix was
// written to stop.
uint8_t flourishSignature();

// Forget what is on the glass WITHOUT erasing it. Call when something else has
// already covered the motes - a pet frame, a full screen repaint, a screen
// switch - so the next drawFlourish() paints a fresh set instead of erasing
// pixels that by now belong to whatever is there instead, and instead of
// waiting out the rest of the beat before it shows anything.
void flourishForget();

// Take the motes off the glass and forget them. The other half of the pair:
// flourishForget() is for "somebody else painted over them", this is for "the
// pet has stopped resting and they should not be there at all". Cheap and safe
// to call when nothing is showing.
void clearFlourish(TFT_eSPI& tft);
