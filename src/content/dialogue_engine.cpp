#include "dialogue_engine.h"

#if defined(UNIT_TEST) || defined(SIMULATOR_BUILD)
#include <cstdio>
#endif

// ── HexHound - Dialogue Engine Implementation ────────────────────

// The substitution token content writes to mark where a number goes.
static const char DIALOGUE_TOKEN[] = "{n}";
static const uint8_t DIALOGUE_TOKEN_LEN = 3;

DialogueEngine& DialogueEngine::instance() {
    static DialogueEngine engine;
    return engine;
}

void DialogueEngine::begin(uint32_t seed) {
    _rng = seed;
    // 0xFF means "nothing shown yet for this context". Zero would be a real
    // index, and would quietly make line 0 unpickable on the first request.
    memset(_lastPicked, 0xFF, sizeof(_lastPicked));
}

void DialogueEngine::setMemoryAccessor(DialogueMemoryFn fn, void* user) {
    _memoryFn = fn;
    _memoryUser = user;
}

void DialogueEngine::setUnavailableSlots(uint32_t slotMask) {
    _unavailableSlots = slotMask;
}

int DialogueEngine::scoreLine(const DialogueLine& line, DialogueContext ctx,
                              uint8_t traitA, uint8_t traitB,
                              uint8_t stage, uint8_t form) const {
    if (line.context != ctx) return -1;

    // A line that needs a number is only eligible once someone can supply one.
    if (line.memorySlot != DIALOGUE_MEMORY_NONE) {
        if (!_memoryFn) return -1;
        if (line.memorySlot >= MEM_SLOT_COUNT) return -1;
        // Slots this hardware cannot compute make their line ineligible, so it
        // is never selected and then rendered with a filler number. Checked
        // against a mask rather than by asking the accessor, so eligibility
        // costs nothing and no legitimate count can be mistaken for "missing".
        if (_unavailableSlots & (1UL << line.memorySlot)) return -1;
    }

    bool traitTagged = (line.trait != 0xFF);
    if (traitTagged && line.trait != traitA && line.trait != traitB) return -1;

    bool stageTagged = (line.stage != 0xFF);
    if (stageTagged && line.stage != stage) return -1;

    // The form filter. A form-tagged line is only eligible for a pet that has
    // actually earned that form, so an undecided pet (DIALOGUE_FORM_UNSET)
    // never sees one. That is what keeps form DISCOVERABLE: the writing shifts
    // once the pet is something, rather than a screen announcing what it is
    // about to become or how close it is.
    bool formTagged = (line.form != 0xFF);
    if (formTagged && (form == DIALOGUE_FORM_UNSET || line.form != form)) {
        return -1;
    }

    // Specific beats generic, in the order trait, form, stage: the trait is the
    // pet's voice, the form is what it has become, the stage is only where it
    // is in its life. The weights are powers of two so the ranking is the same
    // whichever combination of tags a pack happens to use.
    return (traitTagged ? 4 : 0) + (formTagged ? 2 : 0) + (stageTagged ? 1 : 0);
}

const char* DialogueEngine::pick(DialogueContext ctx, uint8_t trait,
                                 uint8_t stage) {
    return pick(ctx, trait, 0xFF, stage, DIALOGUE_FORM_UNSET);
}

const char* DialogueEngine::pick(DialogueContext ctx, uint8_t traitA,
                                 uint8_t traitB, uint8_t stage, uint8_t form) {
    const ContentStore& store = ContentStore::instance();
    const uint8_t total = store.dialogueCount();

    // Pass 1: how specific is the best available match?
    int best = -1;
    for (uint8_t i = 0; i < total; i++) {
        const DialogueLine* line = store.dialogue(i);
        if (!line) continue;
        int score = scoreLine(*line, ctx, traitA, traitB, stage, form);
        if (score > best) best = score;
    }
    if (best < 0) return nullptr;   // nothing to say, and that is fine

    const uint8_t last = (ctx < DLG_CONTEXT_COUNT) ? _lastPicked[ctx] : 0xFF;

    // Pass 2: count the equally-good candidates, preferring not to repeat the
    // line this context showed last time.
    uint8_t fresh = 0;
    uint8_t matching = 0;
    for (uint8_t i = 0; i < total; i++) {
        const DialogueLine* line = store.dialogue(i);
        if (!line || scoreLine(*line, ctx, traitA, traitB, stage, form) != best) continue;
        matching++;
        if (i != last) fresh++;
    }

    const bool avoidLast = (fresh > 0);
    const uint8_t choices = avoidLast ? fresh : matching;
    uint32_t wanted = Content::nextRandom(_rng) % choices;

    // Pass 3: hand back the chosen one.
    for (uint8_t i = 0; i < total; i++) {
        const DialogueLine* line = store.dialogue(i);
        if (!line || scoreLine(*line, ctx, traitA, traitB, stage, form) != best) continue;
        if (avoidLast && i == last) continue;
        if (wanted-- > 0) continue;

        if (ctx < DLG_CONTEXT_COUNT) _lastPicked[ctx] = i;
        return expand(*line);
    }

    return nullptr;
}

bool DialogueEngine::hasLineFor(DialogueContext ctx, uint8_t traitA,
                                uint8_t traitB, uint8_t stage,
                                uint8_t form) const {
    const ContentStore& store = ContentStore::instance();
    for (uint8_t i = 0; i < store.dialogueCount(); i++) {
        const DialogueLine* line = store.dialogue(i);
        if (line && scoreLine(*line, ctx, traitA, traitB, stage, form) >= 0) {
            return true;
        }
    }
    return false;
}

const char* DialogueEngine::expand(const DialogueLine& line) {
    const char* src = line.text;
    char* dst = _line;
    char* const end = _line + sizeof(_line) - 1;
    bool substituted = false;

    while (*src && dst < end) {
        // Only the first token is substituted. A line asks for one number; a
        // second {n} in the same line is an authoring mistake, and leaving it
        // visible is better than silently printing the same figure twice.
        if (!substituted && line.memorySlot != DIALOGUE_MEMORY_NONE &&
            strncmp(src, DIALOGUE_TOKEN, DIALOGUE_TOKEN_LEN) == 0) {
            uint32_t value = _memoryFn ? _memoryFn(line.memorySlot, _memoryUser) : 0;
            int written = snprintf(dst, (size_t)(end - dst) + 1, "%lu",
                                   (unsigned long)value);
            if (written < 0) break;
            if (written > (int)(end - dst)) written = (int)(end - dst);
            dst += written;
            src += DIALOGUE_TOKEN_LEN;
            substituted = true;
            continue;
        }
        *dst++ = *src++;
    }

    *dst = '\0';
    return _line;
}
