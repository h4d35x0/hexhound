#include "pet_forms.h"

// ── HexHound - Behavioural Form Implementation ───────────────────

namespace PetForms {

namespace {

// Nouns, indexed by (PetForm - 1). One word each: the title has to fit beside a
// stage adjective on a 160x80 panel, and a two-word form name does not.
const char* const FORM_NAMES[FORM_BEHAVIOUR_COUNT] = {
    "Pathfinder",
    "Guardian",
    "Archivist",
    "Cipher",
    "Gremlin",
    "Packmaster"
};

// Stage as an adjective, indexed by (PetStage - 1). These describe how long the
// pet has been at it, never how good it is, because the stage is age and saying
// otherwise would turn the display-only title into a ranking.
const char* const STAGE_ADJECTIVE[5] = {
    "Nascent",     // Egg
    "Fledgling",   // Packet Pup
    "Seasoned",    // Beacon Beast
    "Veteran",     // Gremlin Mode
    "Legendary"    // Sentinel
};

// RGB565 accents, indexed by (PetForm - 1). Chosen to be separable on a cheap
// TFT rather than merely distinct in hex: six hues spread around the wheel, all
// bright enough to read against the black UI background. None of them is the
// 0xF81F chroma key that drawSprite() treats as transparent.
const uint16_t FORM_ACCENT[FORM_BEHAVIOUR_COUNT] = {
    0x0739,   // Pathfinder - teal
    0x3C7F,   // Guardian    - azure
    0xB3DF,   // Archivist   - violet
    0xB7E6,   // Cipher      - lime
    0xFA79,   // Gremlin     - magenta
    0xFCC6    // Packmaster  - orange
};

// Neutral grey for a pet that has not earned a form. Matches the egg entry in
// STAGE_COLOR, so an undecided pet looks unremarkable rather than broken.
const uint16_t ACCENT_UNSET = 0x8410;

// Longest title is "Legendary Packmaster" at 20 characters. 32 leaves room for
// a longer stage adjective without anyone having to re-count.
char s_title[32] = {};

}  // namespace

uint8_t formIndex(PetForm form) {
    if (form <= FORM_UNSET || form >= FORM_COUNT) return 0xFF;
    return (uint8_t)(form - 1);
}

PetForm evaluate(const uint16_t* behaviour, PetForm current) {
    if (!behaviour) return current;

    // Single pass for the two largest counters. Ties go to the lower index,
    // which only decides which of two equal values is called the leader; an
    // exact tie can never clear the lead bar against itself anyway.
    uint16_t leadValue = 0;
    uint16_t runnerUp  = 0;
    uint8_t  leadIndex = 0xFF;

    for (uint8_t i = 0; i < FORM_BEHAVIOUR_COUNT; i++) {
        const uint16_t v = behaviour[i];
        if (v > leadValue) {
            runnerUp  = leadValue;
            leadValue = v;
            leadIndex = i;
        } else if (v > runnerUp) {
            runnerUp = v;
        }
    }

    // Not enough behaviour to say anything. Declaring an identity off three
    // data points would be noise dressed as insight.
    //
    // This branch answers FORM_UNSET rather than keeping `current`, unlike the
    // lead check below. Because evidence never decays, a pet that once cleared
    // the floor cannot fall back under it during normal play, so the only way
    // to reach here holding a form is a save whose counters were lost or
    // rewritten. In that case the evidence really is gone, and "undecided" is
    // the honest answer; carrying the old label forward would be the firmware
    // asserting something it can no longer support.
    if (leadIndex == 0xFF || leadValue < FORM_MIN_EVIDENCE) return FORM_UNSET;

    // The lead bar. uint32 because 65535 * 140 overflows 16 bits, and a pet
    // that has genuinely maxed a counter must not wrap into a wrong answer.
    const uint32_t lead      = (uint32_t)leadValue * 100u;
    const uint32_t threshold = (uint32_t)runnerUp * (uint32_t)FORM_LEAD_PERCENT;
    if (lead < threshold) return current;

    return (PetForm)(leadIndex + 1);
}

bool refresh(PetState& pet) {
    const PetForm next = evaluate(pet.behaviour, pet.form);
    if (next == pet.form) return false;
    pet.form = next;
    pet.dirty = true;
    return true;
}

void addEvidence(PetState& pet, PetForm form, uint16_t weight) {
    const uint8_t idx = formIndex(form);
    if (idx == 0xFF || weight == 0) return;

    // Saturating. A counter that wrapped would erase a year of evidence in one
    // tick, and silently hand the form to whoever was second.
    uint16_t& slot = pet.behaviour[idx];
    if (slot > (uint16_t)(0xFFFF - weight)) {
        slot = 0xFFFF;
    } else {
        slot = (uint16_t)(slot + weight);
    }

    pet.dirty = true;
    refresh(pet);
}

// The six named entry points. Each is one line on purpose: the rules layer says
// what happened, this file decides which identity that is evidence for, and the
// mapping stays in one place if it ever needs to change.

void recordWalk(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_PATHFINDER, weight);
}

void recordGuard(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_GUARDIAN, weight);
}

void recordCollect(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_ARCHIVIST, weight);
}

void recordPuzzle(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_CIPHER, weight);
}

void recordPlay(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_GREMLIN, weight);
}

void recordSocial(uint16_t weight) {
    addEvidence(PetCore::instance().state(), FORM_PACKMASTER, weight);
}

const char* formName(PetForm form) {
    const uint8_t idx = formIndex(form);
    return idx == 0xFF ? "" : FORM_NAMES[idx];
}

const char* formTitle(PetStage stage, PetForm form) {
    s_title[0] = '\0';

    const uint8_t idx = formIndex(form);
    if (idx == 0xFF) return s_title;

    const char* adjective = "";
    if (stage >= STAGE_EGG && stage <= STAGE_SENTINEL) {
        adjective = STAGE_ADJECTIVE[(uint8_t)stage - 1];
    }

    // Built by hand rather than with snprintf: this is called from a redraw
    // path on a board with 250 KB of RAM and no printf worth pulling in for two
    // string copies.
    char* dst = s_title;
    char* const end = s_title + sizeof(s_title) - 1;

    for (const char* p = adjective; *p && dst < end; p++) *dst++ = *p;
    if (*adjective && dst < end) *dst++ = ' ';
    for (const char* p = FORM_NAMES[idx]; *p && dst < end; p++) *dst++ = *p;
    *dst = '\0';

    return s_title;
}

uint16_t formAccent(PetForm form) {
    const uint8_t idx = formIndex(form);
    return idx == 0xFF ? ACCENT_UNSET : FORM_ACCENT[idx];
}

}  // namespace PetForms
