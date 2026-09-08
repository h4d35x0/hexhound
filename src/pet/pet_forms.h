#pragma once

#include <stdint.h>

#include "pet_core.h"

// ── HexHound - Behavioural Form ──────────────────────────────────
//
// The SECOND identity axis. PetStage is age and is bought with XP; PetForm is
// character and is earned by what the owner actually does. They are orthogonal
// on purpose: a Legendary Pathfinder and a Legendary Cipher are the same age
// and nothing alike, which is the only way two people end up with pets that
// are not the same pet.
//
// Four rules shape everything in this file.
//
//  1. AGE IS UNTOUCHED. Nothing here reads or writes stage, XP, or the
//     evolution path. PetCore::checkEvolution() is not called, not wrapped and
//     not consulted. An old save that has no form field loads as FORM_UNSET,
//     which is the honest answer for a pet nobody has watched yet.
//
//  2. FORM IS DISPLAY ONLY. A title, an accent colour, a small badge. There is
//     deliberately no stat multiplier, no unlock and no bonus anywhere in this
//     header. The moment a form changes a number the pet stops being a
//     companion and becomes a build to optimise, and the honest way to get the
//     good form becomes farming rather than living with it.
//
//  3. EVIDENCE ONLY ACCUMULATES. The counters saturate and are never decayed.
//     An identity is earned across a pet's whole life; letting it rot would
//     punish someone for a quiet fortnight, and would mean the pet you put
//     down in March is not the pet you pick up in June.
//
//  4. A CLAIMED FORM IS STICKY. See evaluate() for exactly what that means and
//     why a purely stateless rule flip-flops.

namespace PetForms {

// ── Recorders ─────────────────────────────────────────────────────────────
//
// The rules layer calls these as things happen. Each adds saturating evidence
// to one behaviour, re-evaluates the form, and marks the pet dirty so the new
// evidence survives a power cut. `weight` lets a caller report a batch (twenty
// steps, four collectibles) without twenty calls; it is not a difficulty dial.
//
// They are deliberately six named functions rather than one record(kind, n).
// The call site then reads as the thing that happened rather than as a lookup
// into an enum, and a caller cannot pass an index that is out of range.

void recordWalk(uint16_t weight = 1);    // travel, roam, new environments
void recordGuard(uint16_t weight = 1);   // watching, defending, triage
void recordCollect(uint16_t weight = 1); // collections, journal, discoveries
void recordPuzzle(uint16_t weight = 1);  // puzzles, challenges, learning
void recordPlay(uint16_t weight = 1);    // minigames, mischief, missions
void recordSocial(uint16_t weight = 1);  // encounters, co-op, other pets

// ── Selection ─────────────────────────────────────────────────────────────

// Pick the form the evidence supports.
//
// `behaviour` is FORM_BEHAVIOUR_COUNT counters indexed by (PetForm - 1), i.e.
// PetState::behaviour. `current` is the form already claimed, and is what makes
// this stable rather than jittery.
//
// The rule:
//   * Leader below FORM_MIN_EVIDENCE          -> FORM_UNSET.
//   * Leader not FORM_LEAD_PERCENT ahead of
//     the runner-up                           -> keep `current`, which is
//                                                FORM_UNSET until a form has
//                                                actually been earned.
//   * Otherwise                               -> the leader.
//
// Keeping `current` rather than reverting to FORM_UNSET is the whole point. A
// stateless "leader wins, else unset" rule churns at the boundary: with the
// floor at 25 and the lead bar at 140%, counters of 100 and 71 name a form,
// the runner-up ticks to 72 and the pet is suddenly nobody, and one more tick
// on the leader names it again. Requiring a challenger to clear the same bar
// against the incumbent makes the round trip cost a 1.96x swing, so a near-tie
// sits still instead of oscillating, and a form is never taken away from
// someone who has not changed how they play.
//
// Pure: same arguments, same answer, no singleton, no clock.
PetForm evaluate(const uint16_t* behaviour, PetForm current);

// Re-run evaluate() over the pet and store the result. Returns true when the
// form actually changed, which is for the caller's redraw bookkeeping and NOT
// a cue to announce anything - see formTitle().
bool refresh(PetState& pet);

// Add evidence to one behaviour of an arbitrary state and re-evaluate. The
// no-argument recorders above are thin wrappers over this on the singleton;
// this form exists so tests, and any future second pet, can drive it directly.
void addEvidence(PetState& pet, PetForm form, uint16_t weight);

// ── Presentation ──────────────────────────────────────────────────────────

// "Pathfinder". Empty string for FORM_UNSET and for anything out of range,
// never a placeholder like "Unknown": a caller that prints this blind shows
// nothing rather than a label the pet has not earned.
const char* formName(PetForm form);

// "Legendary Pathfinder" - the stage as an adjective, the form as the noun.
//
// Returns "" while the form is FORM_UNSET, on purpose. There is no "Legendary
// Nobody" line and no progress bar toward one. A form is meant to be noticed
// by the person who caused it, not advertised as a goal to chase, so until it
// exists the title area shows the stage name the UI already had.
//
// The result lives in a static buffer, valid until the next call. Same
// convention as DialogueEngine::pick(): draw it or copy it.
const char* formTitle(PetStage stage, PetForm form);

// RGB565 accent for the form. Falls back to a neutral grey for FORM_UNSET so a
// caller always has a colour to draw with. Never returns the 0xF81F chroma key.
uint16_t formAccent(PetForm form);

// Index into PetState::behaviour, and the badge index in src/ui/form_art.h.
// Returns 0xFF for FORM_UNSET and out-of-range values, which every caller must
// check before indexing.
uint8_t formIndex(PetForm form);

}  // namespace PetForms
