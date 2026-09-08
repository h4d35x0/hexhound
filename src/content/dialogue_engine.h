#pragma once

#if defined(UNIT_TEST)
// Unit tests provide their own stubs
#include <cstdint>
#include <cstring>
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

#include "content_types.h"
#include "content_store.h"

// ── HexHound - Dialogue Engine ───────────────────────────────────
//
// Picks one line for a context, given who the pet currently is.
//
// Three rules shape everything here:
//
//  * Nothing matches is a normal outcome. A pack may simply have no line for
//    DLG_GAME_LOSE, and every caller is a UI that can draw nothing. pick()
//    returns nullptr and the caller shows no bubble. It must never crash and
//    must never invent filler.
//  * Content declares intent, the firmware owns the numbers. A line that wants
//    to say "we have walked 14 patrols" declares memorySlot = MEM_SLOT_PATROLS
//    and writes {n} in its text. The engine asks the accessor the caller
//    installed. If no accessor is installed, that line is not eligible at all,
//    so content can never fabricate a statistic.
//  * A specific line beats a generic one. A line tagged with the pet's trait
//    outranks an untagged line for the same context, otherwise the generic
//    filler drowns out the characterful writing as a pack grows. Three tags
//    exist - trait, form and stage - and they rank in that order: the trait is
//    the pet's voice, the form is what it has become, the stage is only how old
//    it is.
//
// No allocation anywhere. The returned pointer is into a single internal
// buffer, valid until the next pick(); callers draw it immediately or copy it.

// Supplies the value for one memory slot. `user` is whatever the caller passed
// to setMemoryAccessor, so the accessor can reach pet state without this engine
// knowing anything about it.
typedef uint32_t (*DialogueMemoryFn)(uint8_t slot, void* user);

class DialogueEngine {
public:
    static DialogueEngine& instance();

    // `seed` only needs to vary between boots; it selects which line of an
    // equally-good set comes out first.
    void begin(uint32_t seed);

    // Install the number source. Until this is called, lines that declare a
    // memory slot are skipped rather than shown with a zero in them.
    void setMemoryAccessor(DialogueMemoryFn fn, void* user);

    // Bitmask of MemorySlot values this device cannot compute. A line asking
    // for one of them is ineligible, so it is never rendered with filler.
    //
    // This is a mask set once at init rather than a magic return value from
    // the accessor, for two reasons. First, every uint32 is a legitimate
    // count, including 0xFFFFFFFF, so any in-band sentinel would eventually
    // misread a real number as "missing". Second, which slots are unknowable
    // is a property of the hardware, not of the moment, so asking per call
    // would mean calling the accessor during eligibility scoring as well as
    // during substitution.
    //
    // Live example: MEM_SLOT_DAYS_AWAY. There is no RTC and millis() resets
    // each boot, so the device cannot tell four minutes unplugged from four
    // months. Rendering "You were gone 0 days. I counted every one." would be
    // a confident, wrong, faintly unsettling statistic. Content stays authored
    // for hardware that will one day have a clock; today the line simply never
    // comes up.
    void setUnavailableSlots(uint32_t slotMask);

    // Returns the line to show, or nullptr when nothing matches.
    // `trait` and `stage` accept 0xFF for "unknown", which then matches only
    // untagged lines.
    const char* pick(DialogueContext ctx, uint8_t trait, uint8_t stage);

    // The pet carries two traits; a line tagged with either one suits it.
    //
    // `form` is the pet's PetForm, the third filter. It defaults to
    // DIALOGUE_FORM_UNSET so that a caller which has not been taught about
    // forms yet gets exactly the behaviour it had before: form-tagged lines are
    // simply never eligible for it. Adding the argument can therefore not
    // silently change what an existing screen says.
    const char* pick(DialogueContext ctx, uint8_t traitA, uint8_t traitB,
                     uint8_t stage, uint8_t form = DIALOGUE_FORM_UNSET);

    // True when at least one line would match. Lets a caller decide whether to
    // reserve screen space before it commits to drawing.
    bool hasLineFor(DialogueContext ctx, uint8_t traitA, uint8_t traitB,
                    uint8_t stage, uint8_t form = DIALOGUE_FORM_UNSET) const;

private:
    DialogueEngine() = default;

    // -1 when the line does not apply. Higher is more specific.
    int scoreLine(const DialogueLine& line, DialogueContext ctx,
                  uint8_t traitA, uint8_t traitB, uint8_t stage,
                  uint8_t form) const;

    // Copies text into _line, expanding {n} if the line declares a slot.
    const char* expand(const DialogueLine& line);

    // Room for the longest line plus the widest number {n} can become.
    char _line[CONTENT_MAX_TEXT_LEN + 12] = {};

    // Last index shown per context, so the same line does not come back twice
    // in a row while other equally-good ones are waiting.
    uint8_t _lastPicked[DLG_CONTEXT_COUNT] = {};

    DialogueMemoryFn _memoryFn = nullptr;
    void*            _memoryUser = nullptr;
    // Slots this hardware cannot compute. Default 0: everything is available
    // until a caller says otherwise, so an engine used without wiring behaves
    // exactly as before.
    uint32_t         _unavailableSlots = 0;
    uint32_t         _rng = 0;
};
