#include "quest_engine.h"
#include "../board/capabilities.h"

// ── HexHound - Quest Engine Implementation ───────────────────────

QuestEngine& QuestEngine::instance() {
    static QuestEngine engine;
    return engine;
}

bool QuestEngine::isOfferable(const QuestDef& def) {
    // Every bit the quest asks for must be present on this board. Caps::mask()
    // is constexpr, so on a board with no IMU this folds to a constant and the
    // motion quests are dropped without a runtime test.
    return (def.requiresCaps & ~Caps::mask()) == 0;
}

void QuestEngine::begin(uint32_t seed) {
    rollDaily(seed);
}

void QuestEngine::rollDaily(uint32_t seed) {
    // A new day replaces every slot, so any undrained completion bit now points
    // at a quest that no longer exists. Clearing is not optional: slot 2 today
    // is a different quest from slot 2 yesterday, and paying the new one for the
    // old one's completion is a silent, unreproducible XP leak.
    _justCompleted = 0;

    _rng = seed;
    _rerollsLeft = QUEST_REROLLS_PER_DAY;
    _count = 0;
    for (uint8_t i = 0; i < QUEST_MAX_ACTIVE; i++) {
        _quests[i] = QuestInstance();
    }

    // One quest per kind first, in kind order, so a day never turns into four
    // variations of "feed the pet". Kinds with nothing eligible on this board
    // are skipped here and their slot is filled from whatever is left.
    for (uint8_t kind = 0; kind < QUEST_KIND_COUNT && _count < QUEST_MAX_ACTIVE;
         kind++) {
        if (fillSlot(_count, kind)) _count++;
    }

    while (_count < QUEST_MAX_ACTIVE && fillSlot(_count, QUEST_KIND_COUNT)) {
        _count++;
    }
}

bool QuestEngine::fillSlot(uint8_t index, uint8_t preferredKind,
                           const char* excludeId) {
    if (index >= QUEST_MAX_ACTIVE) return false;

    const ContentStore& store = ContentStore::instance();
    const uint8_t total = store.questDefCount();

    // Count what is eligible, then take the nth. Two passes so nothing has to
    // be collected into a temporary list.
    uint8_t eligible = 0;
    for (uint8_t i = 0; i < total; i++) {
        const QuestDef* def = store.questDef(i);
        if (!def || !isOfferable(*def)) continue;
        if (preferredKind < QUEST_KIND_COUNT && def->kind != preferredKind) continue;
        if (excludeId && strcmp(def->id, excludeId) == 0) continue;
        if (isOffered(def->id, index)) continue;
        eligible++;
    }
    if (eligible == 0) return false;

    uint32_t wanted = Content::nextRandom(_rng) % eligible;

    for (uint8_t i = 0; i < total; i++) {
        const QuestDef* def = store.questDef(i);
        if (!def || !isOfferable(*def)) continue;
        if (preferredKind < QUEST_KIND_COUNT && def->kind != preferredKind) continue;
        if (excludeId && strcmp(def->id, excludeId) == 0) continue;
        if (isOffered(def->id, index)) continue;
        if (wanted-- > 0) continue;

        assign(index, *def);
        return true;
    }

    return false;
}

bool QuestEngine::isOffered(const char* id, uint8_t exceptIndex) const {
    for (uint8_t i = 0; i < _count; i++) {
        if (i == exceptIndex) continue;
        if (strcmp(_quests[i].id, id) == 0) return true;
    }
    return false;
}

void QuestEngine::assign(uint8_t index, const QuestDef& def) {
    QuestInstance& q = _quests[index];
    strlcpy(q.id, def.id, sizeof(q.id));
    q.progress = 0;
    q.target   = def.target;
    q.kind     = def.kind;
    q.status   = QUEST_OFFERED;
}

const QuestInstance* QuestEngine::quest(uint8_t index) const {
    if (index >= _count) return nullptr;
    return &_quests[index];
}

const QuestDef* QuestEngine::definitionFor(uint8_t index) const {
    if (index >= _count) return nullptr;
    return ContentStore::instance().questDefById(_quests[index].id);
}

bool QuestEngine::reportProgress(QuestKind kind, uint16_t amount) {
    if (amount == 0) return false;

    bool completedSomething = false;
    for (uint8_t i = 0; i < _count; i++) {
        QuestInstance& q = _quests[i];
        if (q.kind != kind || q.status == QUEST_COMPLETE) continue;

        // Saturate rather than wrap: a burst of events on a small target must
        // not roll progress back around to nearly zero.
        uint32_t next = (uint32_t)q.progress + amount;
        q.progress = (next > q.target) ? q.target : (uint16_t)next;
        q.status = (q.progress >= q.target) ? QUEST_COMPLETE : QUEST_ACTIVE;
        if (q.status == QUEST_COMPLETE) {
            completedSomething = true;
            _justCompleted |= (uint8_t)(1u << i);
        }
    }
    return completedSomething;
}

bool QuestEngine::reportProgressById(const char* id, uint16_t amount) {
    if (!id || amount == 0) return false;

    for (uint8_t i = 0; i < _count; i++) {
        QuestInstance& q = _quests[i];
        if (q.status == QUEST_COMPLETE || strcmp(q.id, id) != 0) continue;

        uint32_t next = (uint32_t)q.progress + amount;
        q.progress = (next > q.target) ? q.target : (uint16_t)next;
        q.status = (q.progress >= q.target) ? QUEST_COMPLETE : QUEST_ACTIVE;
        if (q.status == QUEST_COMPLETE) {
            _justCompleted |= (uint8_t)(1u << i);
            return true;
        }
        return false;
    }
    return false;
}

// Drain the "completed since you last asked" set.
//
// The engine reports WHICH slots finished and nothing about what they are
// worth. That split is deliberate and is the rule stated at the patrol call
// site in main.cpp: "the engine only tracks progress; the reward is paid by the
// rules layer, which is what stops a content pack granting its own XP." A quest
// definition is untrusted data - it can arrive from a downloaded pack - so the
// number it names must be read and clamped by code the pack cannot influence.
//
// Draining rather than peeking is what makes the payout exactly-once. The
// completion TRANSITION is the payable event, not the COMPLETE status, so a
// reboot cannot re-pay a quest that is still sitting completed in a restored
// save.
uint8_t QuestEngine::takeJustCompleted(uint8_t* out, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < _count && n < max; i++) {
        if (_justCompleted & (1u << i)) {
            out[n++] = i;
        }
    }
    _justCompleted = 0;
    return n;
}

bool QuestEngine::reroll(uint8_t index, uint32_t seed) {
    if (index >= _count) return false;
    if (_rerollsLeft == 0) return false;
    // Finished work does not get thrown away, and the reward is not re-rollable
    // into a second one.
    if (_quests[index].status == QUEST_COMPLETE) return false;
    // Same reason as rollDaily(): this slot is about to hold a different quest.
    _justCompleted &= (uint8_t)~(1u << index);

    _rng ^= seed;

    // Copy the id out first: fillSlot overwrites the slot, and the rejected
    // quest has to stay excluded from its own replacement.
    char rejected[CONTENT_MAX_ID_LEN];
    strlcpy(rejected, _quests[index].id, sizeof(rejected));

    // Try the same kind first so the day keeps its spread of kinds; fall back
    // to anything eligible if that kind has nothing else to offer.
    const uint8_t sameKind = (uint8_t)_quests[index].kind;
    if (!fillSlot(index, sameKind, rejected) &&
        !fillSlot(index, QUEST_KIND_COUNT, rejected)) {
        return false;
    }

    _rerollsLeft--;
    return true;
}

uint8_t QuestEngine::completedCount() const {
    uint8_t done = 0;
    for (uint8_t i = 0; i < _count; i++) {
        if (_quests[i].status == QUEST_COMPLETE) done++;
    }
    return done;
}

bool QuestEngine::allComplete() const {
    return _count > 0 && completedCount() == _count;
}
