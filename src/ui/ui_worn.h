#pragma once

// tft_compat.h on every build, not just the simulator. <Arduino.h> does not
// declare TFT_eSPI, so guarding this the other way compiled fine in the sim and
// failed on all seven boards at once. Same include the other ui_*.h use.
#include "../hal/tft_compat.h"

#include "../config.h"   // PetStage

// ── HexHound - Drawing what the pet is wearing ──────────────────
//
// One entry point, called by every screen that draws the pet, right after the
// pet itself has been blitted. It reads PetState::wornSlots[] through the rules
// in pet/pet_wear.h, looks up the overlay art in ui/worn_art.h, and places it
// using the per-stage table in ui/wear_anchors.h.
//
// ── Why a free function and not a method on the animator ──────────────────
//
// The pet is drawn from five different files at five different sizes, and the
// only thing they agree on is a top-left corner and a square size. That is
// exactly the interface below. Putting it on Animator would have meant every
// caller holding an animator it does not otherwise use, and the round panels
// draw the pet from a static HD image with no animator involved at all.
//
// ── It draws nothing when nothing is worn ─────────────────────────────────
//
// The common case for a long time, and it costs one array read per anchor.
//
// ── Sizes ─────────────────────────────────────────────────────────────────
//
// `px` is the size the PET was drawn at, not the size of the overlay. The
// anchor table is in thousandths of the pet's box, so this works unchanged for
// a 24 px pet on a T-Dongle and a 320 px hero shot on the T-RGB, and there is
// no per-screen tuning anywhere.
//
// ── WHAT THIS DOES NOT COVER: the animated pixel sprite ───────────────────
//
// This function is only correct over the HD ART. It must not be called over a
// frame of the 16x16 (20x20 for the sentinel) pixel animation, and it
// deliberately is not.
//
// Where the pet has to wear something, the fix is to draw the HD art there
// instead of placing overlays on pixel art. ui_patrol_hud.cpp drawPetCorner()
// used to be a gap and is not any more: it now draws the HD still on every
// panel, rectangular included, because the sprite animation was already the
// least legible of the four channels reporting a discovery.
//
// What is left is BOUNDED, on every panel: ui_home.cpp's transient animations.
// The home screen draws HD art while the pet is at rest or HUNGRY, and the
// pixel animation for ANIM_HAPPY (2 to 5 s) and ANIM_ALERT (4 s), during which
// the pet is bare.
//
// ANIM_HUNGRY used to be in that list and was the one that mattered, because
// it is NOT an override with a deadline: main.cpp updateAnimState() reselects
// it for as long as hunger stays under 30, so a hungry pet was bare
// indefinitely. It has its own HD portrait now, from
// scripts/gen_hd_hungry.py. A state with no time limit cannot be bare; a
// four-second one can.
//
// Note that this was never rectangular-only, which is how it was first
// reported. drawRoundPet() has the same `if (resting)` fall-through, so a
// hungry pet on the 480 lost its cosmetics too.
//
// ── Why not a second anchor table measured against the pixel art ──────────
//
// Measured 2026-08-30 rather than assumed. Three findings, in the order that
// decided it:
//
//   * The two art sets do not frame the figure the same way inside their own
//     box. gen_hd_sprites.py resizes the whole 400x400 canvas, so the HD
//     figure's top sits well inside it; the pixel art is drawn to the edges of
//     its 16x16 grid. The gap between the two, in thousandths of the box, is
//     egg +170, pup +123, beast +108, gremlin +200, sentinel +118. Applying
//     this table to the pixel art unchanged puts the pup's cap over its eyes
//     and the beast's scarf across its face, exactly as expected.
//
//   * The obvious repair does not work. Re-expressing each anchor as the same
//     DEPTH INTO THE FIGURE and mapping that onto the pixel silhouette's own
//     extent is a fifth automatic heuristic, and it fails like the four in
//     scripts/gen_wear_anchors.py: it floats the pup's and gremlin's hats off
//     the top of the skull and drops every scarf onto the chin. The pixel
//     sprite is not a small copy of the HD art. It is a different drawing of
//     the same character, with its own head-to-body ratio.
//
//   * A per-stage table is the wrong SHAPE for animated art anyway, because
//     the silhouette moves between frames. gremlin_laugh and pup_hungry shift
//     the head down a whole pixel (62 thousandths) against their own idle
//     frames, and sentinel_guard walks the body 150 thousandths sideways
//     across its three frames. A fixed cx of 500 detaches the hat visibly on
//     exactly the frames worth looking at. Following it needs a PER-FRAME
//     table: 37 frames x 2 anchors x 3 numbers = 222 authored values against
//     the 30 here, every one placed by eye on a grid where a head is five
//     pixels tall, with no automatic check possible for the reason that
//     script's header already gives.
//
// Against that: the overlay would be drawn 3 to 19 px square. The pet is 32 px
// in the compact home and 64 px in the big-panel home, and a head anchor of
// 195 to 330 thousandths reduces to a 3x3 square for the beast at the 16 px
// the patrol HUD used to draw. The states are also the ones where the whole
// pet has already dropped from a shaded portrait to flat pixel art, so the
// cosmetics going with it reads as one stylistic change rather than as a hat
// falling off.
//
// The premise, not the table, is the thing to reconsider - and both remedies
// are the same one. Draw the HD art wherever the pet must be dressed: as a
// still where nothing needs to move (the patrol HUD corner, done; the hungry
// pet, done), or as baked motion frames where something does, the way
// scripts/gen_hd_idle.py did for the 480. Either way there is no second table
// to keep in step. Only the two timed animations are still on the pixel path,
// and that is a deliberate stopping point rather than an unfinished one.

// Draw the pet's worn cosmetics over art already blitted at (x, y) at px
// square. HD ART ONLY; see the section above before calling it anywhere new.
void drawWornOn(TFT_eSPI& tft, PetStage stage, int x, int y, int px);

// Changes whenever what the pet is wearing changes. Screens that skip a redraw
// while the pet is unchanged - the home screen caches on stage alone - fold
// this in so putting a hat on actually repaints the pet.
uint16_t wornSignature();
