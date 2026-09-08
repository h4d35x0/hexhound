#include "pet_core.h"
#include "../events/event_bus.h"
#include <ArduinoJson.h>

// ── HexHound - Pet Core Implementation ───────────────────────────

namespace {

static const char* const MASTERY_TITLES[] = {
    "INITIATE",
    "WATCHER",
    "HUNTER",
    "ANALYST",
    "WARDEN",
    "SOVEREIGN"
};

static const PetPerk ALL_PERKS[] = {
    PERK_SIGNAL_CARTOGRAPHER,
    PERK_BEACON_HUNTER,
    PERK_ANOMALY_ARCHIVIST,
    PERK_MISSION_OPERATOR,
    PERK_FIELD_SENTINEL
};

bool rememberCapture(const char* token,
                     char* table,
                     uint16_t& count,
                     uint16_t maxCount,
                     size_t slotSize) {
    if (!token || strlen(token) == 0) {
        return false;
    }

    for (uint16_t i = 0; i < count; i++) {
        const char* existing = table + (i * slotSize);
        if (strcmp(existing, token) == 0) {
            return false;
        }
    }

    // ── The table is full: forget the OLDEST, not the newest ──────────────
    //
    // This used to `return false`, which meant the pet remembered the first 64
    // identifiers it ever met and was then incapable of learning another one
    // for the rest of its life. Two consequences, both silent:
    //
    //   * Every capture reward stops permanently. XP_NEW_WIFI_CAPTURE and
    //     XP_NEW_BLE_CAPTURE are paid only when this returns true, so patrol XP
    //     collapses to the food trickle and never recovers. With Sentinel at
    //     2400 XP that leaves no viable way to finish the game.
    //   * Taking the device somewhere genuinely new pays nothing. The pet is
    //     surrounded by networks it has never seen and cannot register one of
    //     them, which is the exact opposite of what the feature is for.
    //
    // A ring is the honest fix: the pet remembers the last 64 things it met.
    // Somewhere new always counts, and ground already covered only pays again
    // once 64 other identifiers have pushed it out of memory - far enough that
    // walking between two rooms cannot farm it.
    //
    // Shifting rather than carrying a head index, deliberately: a head index is
    // a new persisted field and therefore a save schema bump, for a memmove of
    // at most 2112 bytes that happens a handful of times per patrol. The array
    // stays in oldest-to-newest order, which is also what makes this readable
    // in a save dump.
    if (count >= maxCount) {
        if (maxCount <= 1) {
            return false;              // degenerate table; nothing to evict into
        }
        memmove(table, table + slotSize, (size_t)(maxCount - 1) * slotSize);
        count = (uint16_t)(maxCount - 1);
    }

    char* slot = table + (count * slotSize);
    strlcpy(slot, token, slotSize);
    count++;
    return true;
}

} // namespace

PetCore& PetCore::instance() {
    static PetCore core;
    return core;
}

void PetCore::init() {
    // If starting fresh (no save loaded), assign random traits
    if (!_state.hatched && _state.stats.xp == 0) {
        assignRandomTraits();
    }
    Serial.printf("[Pet] Init: %s stage=%d xp=%lu\n",
                  _state.name, _state.stage, _state.stats.xp);
}

void PetCore::assignRandomTraits() {
    _state.traits[0] = static_cast<Trait>(random(TRAIT_COUNT));
    do {
        _state.traits[1] = static_cast<Trait>(random(TRAIT_COUNT));
    } while (_state.traits[1] == _state.traits[0]);
}

int16_t PetCore::clampStat(int16_t val) {
    if (val < STAT_MIN) return STAT_MIN;
    if (val > STAT_MAX) return STAT_MAX;
    return val;
}

void PetCore::feedHunger(int16_t amount) {
    if (hasTrait(TRAIT_GREEDY)) amount = (int16_t)(amount * 0.7f);
    _foodAccum += amount;  // track raw gain before clamping
    _state.stats.hunger = clampStat(_state.stats.hunger + amount);
    _state.dirty = true;
}

void PetCore::changeMood(int16_t amount) {
    _state.stats.mood = clampStat(_state.stats.mood + amount);
    _state.dirty = true;
}

void PetCore::changeEnergy(int16_t amount) {
    if (hasTrait(TRAIT_SLEEPY) && amount < 0) amount = (int16_t)(amount * 0.6f);
    _state.stats.energy = clampStat(_state.stats.energy + amount);
    _state.dirty = true;
}

void PetCore::addXP(uint32_t amount) {
    if (hasTrait(TRAIT_CURIOUS)) amount = (uint32_t)(amount * 1.25f);
    _state.stats.xp += amount;
    _state.dirty = true;
    checkEvolution();
}

void PetCore::addMasteryXP(uint32_t amount) {
    if (_state.stage < STAGE_SENTINEL || amount == 0) {
        return;
    }

    _state.masteryXP += amount;
    while (_state.masteryXP >= masteryForNextRank()) {
        _state.masteryRank++;
    }
    refreshPerks();
    _state.dirty = true;
}

void PetCore::changeTrust(int16_t amount) {
    if (hasTrait(TRAIT_PROTECTIVE)) amount = (int16_t)(amount * 1.3f);
    _state.stats.trust = clampStat(_state.stats.trust + amount);
    _state.dirty = true;
}

void PetCore::changeMischief(int16_t amount) {
    if (hasTrait(TRAIT_CHAOTIC)) amount = (int16_t)(amount * 1.5f);
    _state.stats.mischief = clampStat(_state.stats.mischief + amount);
    _state.dirty = true;
}

void PetCore::changeHealth(int16_t amount) {
    _state.stats.health = clampStat(_state.stats.health + amount);
    _state.dirty = true;
}

void PetCore::decayTick() {
    // A hibernating pet is asleep, not starving. Nothing decays until someone
    // comes back, so walking away from a powered device can never be the thing
    // that hurts it.
    if (_state.hibernating) {
        return;
    }

    if (_state.idleTicks < 0xFFFF) {
        _state.idleTicks++;
    }
    if (_state.idleTicks >= HIBERNATE_AFTER_TICKS) {
        _state.hibernating = true;
        _state.dirty = true;
        Serial.println("[Pet] Hibernating - idle too long");
        return;
    }

    _state.stats.hunger = clampStat(_state.stats.hunger - HUNGER_DECAY);
    _state.stats.energy = clampStat(_state.stats.energy - ENERGY_DECAY);
    _state.stats.mood   = clampStat(_state.stats.mood - MOOD_DECAY);

    // Low hunger hurts health
    if (_state.stats.hunger < 10) {
        _state.stats.health = clampStat(_state.stats.health - 1);
    }
    _state.dirty = true;
}

int16_t PetCore::bond() const {
    // Trust is already the "how much does it rely on you" axis and, unlike
    // hunger or mood, it never decays. That is exactly the property a bond
    // should have: time apart does not undo a relationship.
    return clampStat(_state.stats.trust);
}

int16_t PetCore::spirit() const {
    return clampStat((int16_t)((_state.stats.mood + _state.stats.health) / 2));
}

int16_t PetCore::energy() const {
    return clampStat(_state.stats.energy);
}

void PetCore::beginSession() {
    // A power-on is the nearest thing this hardware has to "a new day", and it
    // is the pattern for a device that gets carried and recharged. The tick
    // counter below covers the opposite pattern, a device left powered.
    _state.questDay++;
    _state.dayTicks = 0;
    _state.dirty = true;
}

bool PetCore::tickQuestDay() {
    if (_state.dayTicks < TICKS_PER_QUEST_DAY) {
        _state.dayTicks++;
        return false;
    }
    _state.questDay++;
    _state.dayTicks = 0;
    _state.dirty = true;
    return true;
}

bool PetCore::rouse() {
    const bool wasAsleep = _state.hibernating;
    if (wasAsleep) {
        _state.hibernating = false;
        // Come back up glad to see them, not resentful. A pet that punishes
        // you for returning teaches you not to return.
        _state.stats.mood = clampStat(_state.stats.mood + 15);
        Serial.println("[Pet] Woke from hibernation");
    }
    _state.idleTicks = 0;
    if (wasAsleep) {
        _state.dirty = true;
    }
    return wasAsleep;
}

void PetCore::recordInteraction() {
    _state.interactions++;
    // Any interaction is proof someone is there, so it also resets the idle
    // clock. Routing every interaction through rouse() means a new interaction
    // type added later cannot forget to do this.
    rouse();
    _state.dirty = true;
}

void PetCore::recordWifiScan() {
    _state.wifiScans++;
    _state.dirty = true;
}

void PetCore::recordUsbConnect() {
    _state.usbConnects++;
    _state.dirty = true;
}

void PetCore::recordThreatOpen() {
    _state.threatOpenCount++;
    refreshPerks();
    _state.dirty = true;
}

void PetCore::recordThreatDuplicate() {
    _state.threatDuplicateCount++;
    refreshPerks();
    _state.dirty = true;
}

void PetCore::recordThreatTracker() {
    _state.threatTrackerCount++;
    refreshPerks();
    _state.dirty = true;
}

void PetCore::recordMissionComplete() {
    _state.missionsCompleted++;
    refreshPerks();
    _state.dirty = true;
}

bool PetCore::markWifiSeen(const char* ssid) {
    bool added = rememberCapture(
        ssid,
        &_state.seenWifi[0][0],
        _state.seenWifiCount,
        MAX_SEEN_WIFI_CAPTURES,
        sizeof(_state.seenWifi[0]));
    if (added) {
        refreshPerks();
        _state.dirty = true;
    }
    return added;
}

bool PetCore::markBleSeen(const char* addr) {
    bool added = rememberCapture(
        addr,
        &_state.seenBle[0][0],
        _state.seenBleCount,
        MAX_SEEN_BLE_CAPTURES,
        sizeof(_state.seenBle[0]));
    if (added) {
        refreshPerks();
        _state.dirty = true;
    }
    return added;
}

void PetCore::checkEvolution() {
    PetStage oldStage = _state.stage;

    if (!_state.hatched) {
        if (_state.interactions >= EGG_INTERACTIONS &&
            _state.wifiScans   >= EGG_WIFI_SCANS &&
            _state.usbConnects >= EGG_USB_CONNECTS) {
            _state.hatched = true;
            _state.stage = STAGE_PACKET_PUP;
            assignRandomTraits();
            EventBus::instance().publish(EVENT_PET_HATCHED);
            // Trigger evolution cutscene for hatching (same as stage transitions)
            int32_t payload = ((int32_t)STAGE_EGG << 8) | (int32_t)STAGE_PACKET_PUP;
            EventBus::instance().publish(EVENT_STAGE_EVOLVED, payload);
            Serial.println("[Pet] Hatched! Now Packet Pup");
        }
        return;
    }

    // XP-based evolution
    if (_state.stats.xp >= STAGE5_XP && _state.stage < STAGE_SENTINEL) {
        _state.stage = STAGE_SENTINEL;
    } else if (_state.stats.xp >= STAGE4_XP && _state.stage < STAGE_GREMLIN) {
        _state.stage = STAGE_GREMLIN;
    } else if (_state.stats.xp >= STAGE3_XP && _state.stage < STAGE_BEACON_BEAST) {
        _state.stage = STAGE_BEACON_BEAST;
    }

    if (_state.stage != oldStage) {
        // Pack both stages into payload: fromStage in high byte, toStage in low byte
        int32_t payload = ((int32_t)oldStage << 8) | (int32_t)_state.stage;
        EventBus::instance().publish(EVENT_STAGE_EVOLVED, payload);
        Serial.printf("[Pet] Evolved from stage %d to stage %d!\n", oldStage, _state.stage);
    }
}

bool PetCore::hasTrait(Trait t) const {
    return _state.traits[0] == t || _state.traits[1] == t;
}

float PetCore::traitMultiplier(Trait t) const {
    if (!hasTrait(t)) return 1.0f;
    switch (t) {
        case TRAIT_CURIOUS:    return 1.25f;
        case TRAIT_PROTECTIVE: return 1.30f;
        case TRAIT_CHAOTIC:    return 1.50f;
        case TRAIT_SLEEPY:     return 0.60f;
        case TRAIT_GREEDY:     return 0.70f;
        case TRAIT_BRAVE:      return 1.20f;
        default:               return 1.0f;
    }
}

const char* PetCore::stageName() const {
    switch (_state.stage) {
        case STAGE_EGG:          return "Egg";
        case STAGE_PACKET_PUP:   return "Packet Pup";
        case STAGE_BEACON_BEAST: return "Beacon Beast";
        case STAGE_GREMLIN:      return "Gremlin Mode";
        case STAGE_SENTINEL:     return "Sentinel";
        default:                 return "Unknown";
    }
}

uint32_t PetCore::xpForCurrentStage() const {
    switch (_state.stage) {
        case STAGE_EGG:
        case STAGE_PACKET_PUP:
            return STAGE2_XP;
        case STAGE_BEACON_BEAST:
            return STAGE3_XP;
        case STAGE_GREMLIN:
            return STAGE4_XP;
        case STAGE_SENTINEL:
            return STAGE5_XP;
        default:
            return 0;
    }
}

uint32_t PetCore::xpForNextStage() const {
    switch (_state.stage) {
        case STAGE_EGG:
        case STAGE_PACKET_PUP:
            return STAGE3_XP;
        case STAGE_BEACON_BEAST:
            return STAGE4_XP;
        case STAGE_GREMLIN:
            return STAGE5_XP;
        case STAGE_SENTINEL:
        default:
            return STAGE5_XP;
    }
}

uint32_t PetCore::xpToNextStage() const {
    if (_state.stage >= STAGE_SENTINEL) {
        return 0;
    }

    uint32_t nextXp = xpForNextStage();
    if (_state.stats.xp >= nextXp) {
        return 0;
    }
    return nextXp - _state.stats.xp;
}

uint32_t PetCore::masteryRequirementForRank(uint16_t rank) const {
    return 80U + (uint32_t)rank * 60U + (uint32_t)rank * (uint32_t)rank * 20U;
}

uint32_t PetCore::masteryForCurrentRank() const {
    uint32_t total = 0;
    for (uint16_t rank = 0; rank < _state.masteryRank; rank++) {
        total += masteryRequirementForRank(rank);
    }
    return total;
}

uint32_t PetCore::masteryForNextRank() const {
    return masteryForCurrentRank() + masteryRequirementForRank(_state.masteryRank);
}

uint32_t PetCore::masteryToNextRank() const {
    if (_state.stage < STAGE_SENTINEL) {
        return 0;
    }

    uint32_t next = masteryForNextRank();
    if (_state.masteryXP >= next) {
        return 0;
    }
    return next - _state.masteryXP;
}

const char* PetCore::nextStageName() const {
    switch (_state.stage) {
        case STAGE_EGG:
        case STAGE_PACKET_PUP:
            return STAGE_NAMES[STAGE_BEACON_BEAST - 1];
        case STAGE_BEACON_BEAST:
            return STAGE_NAMES[STAGE_GREMLIN - 1];
        case STAGE_GREMLIN:
            return STAGE_NAMES[STAGE_SENTINEL - 1];
        case STAGE_SENTINEL:
        default:
            return "MAX";
    }
}

const char* PetCore::masteryTitle() const {
    uint16_t idx = _state.masteryRank / 2;
    if (idx >= (sizeof(MASTERY_TITLES) / sizeof(MASTERY_TITLES[0]))) {
        idx = (sizeof(MASTERY_TITLES) / sizeof(MASTERY_TITLES[0])) - 1;
    }
    return MASTERY_TITLES[idx];
}

uint16_t PetCore::totalThreatFindings() const {
    return _state.threatOpenCount +
           _state.threatDuplicateCount +
           _state.threatTrackerCount +
           _state.missionsCompleted;
}

uint8_t PetCore::unlockedPerkCount() const {
    uint8_t count = 0;
    for (PetPerk perk : ALL_PERKS) {
        if (hasPerk(perk)) {
            count++;
        }
    }
    return count;
}

bool PetCore::hasPerk(PetPerk perk) const {
    return (_state.perkMask & perk) != 0;
}

void PetCore::refreshPerks() {
    uint16_t nextMask = 0;

    if (_state.seenWifiCount >= 25) {
        nextMask |= PERK_SIGNAL_CARTOGRAPHER;
    }
    if (_state.seenBleCount >= 20) {
        nextMask |= PERK_BEACON_HUNTER;
    }
    if ((_state.threatOpenCount + _state.threatDuplicateCount + _state.threatTrackerCount) >= 15) {
        nextMask |= PERK_ANOMALY_ARCHIVIST;
    }
    if (_state.missionsCompleted >= 10) {
        nextMask |= PERK_MISSION_OPERATOR;
    }
    if (_state.masteryRank >= 5) {
        nextMask |= PERK_FIELD_SENTINEL;
    }

    _state.perkMask = nextMask;
}

bool PetCore::loadFrom(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[Pet] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Saves written before versioning existed have no "schema" key. They are
    // version 1 by definition, so default to 1 rather than to the current
    // version - assuming "current" would silently skip every future migration
    // step for exactly the saves that need them most.
    const uint16_t loadedVersion = doc["schema"] | 1;

    strlcpy(_state.name, doc["name"] | PET_NAME_DEFAULT, sizeof(_state.name));

    // Range-check anything that indexes a table. A save is untrusted input:
    // it can be truncated by a power cut, hand-edited, or written by a newer
    // firmware. STAGE_NAMES/STAGE_ABILITY/STAGE_COLOR are indexed by stage-1
    // and the form tables by form, so an out-of-range value here is an
    // out-of-bounds read on the next draw, not a cosmetic oddity.
    {
        const int rawStage = doc["stage"] | (int)STAGE_EGG;
        _state.stage = (rawStage >= STAGE_EGG && rawStage <= STAGE_SENTINEL)
                     ? static_cast<PetStage>(rawStage)
                     : STAGE_EGG;
    }
    _state.stats.hunger = doc["hunger"]   | 50;
    _state.stats.mood   = doc["mood"]     | 50;
    _state.stats.energy = doc["energy"]   | 100;
    _state.stats.xp     = doc["xp"]       | 0;
    _state.stats.trust  = doc["trust"]    | 0;
    _state.stats.mischief = doc["mischief"] | 0;
    _state.stats.health = doc["health"]   | 100;
    _state.traits[0]    = static_cast<Trait>(doc["trait0"] | 0);
    _state.traits[1]    = static_cast<Trait>(doc["trait1"] | 5);
    _state.interactions = doc["interactions"] | 0;
    _state.wifiScans    = doc["wifiScans"]   | 0;
    _state.usbConnects  = doc["usbConnects"] | 0;
    _state.seenWifiCount = 0;
    JsonArray seenWifi = doc["seenWifi"].as<JsonArray>();
    for (JsonVariant v : seenWifi) {
        if (_state.seenWifiCount < MAX_SEEN_WIFI_CAPTURES) {
            strlcpy(_state.seenWifi[_state.seenWifiCount], v.as<const char*>(), sizeof(_state.seenWifi[0]));
            _state.seenWifiCount++;
        }
    }
    _state.seenBleCount = 0;
    JsonArray seenBle = doc["seenBle"].as<JsonArray>();
    for (JsonVariant v : seenBle) {
        if (_state.seenBleCount < MAX_SEEN_BLE_CAPTURES) {
            strlcpy(_state.seenBle[_state.seenBleCount], v.as<const char*>(), sizeof(_state.seenBle[0]));
            _state.seenBleCount++;
        }
    }
    _state.masteryXP           = doc["masteryXP"] | 0;
    _state.masteryRank         = 0;
    _state.threatOpenCount     = doc["threatOpenCount"] | 0;
    _state.threatDuplicateCount = doc["threatDuplicateCount"] | 0;
    _state.threatTrackerCount  = doc["threatTrackerCount"] | 0;
    _state.missionsCompleted   = doc["missionsCompleted"] | 0;
    _state.perkMask            = doc["perkMask"] | 0;
    while (_state.masteryXP >= masteryForNextRank()) {
        _state.masteryRank++;
    }
    _state.saveSeq      = doc["saveSeq"]     | 0;
    _state.hatched      = doc["hatched"]     | false;
    // New optional fields: an older save simply has neither, and the defaults
    // are correct for it (awake, freshly idle). This is the case the schema
    // header describes as NOT needing a version bump.
    _state.idleTicks    = doc["idleTicks"]   | 0;
    _state.hibernating  = doc["hibernating"] | false;
    _state.checkIns        = doc["checkIns"]        | 0;
    _state.questsCompleted = doc["questsCompleted"] | 0;
    _state.bestGameScore   = doc["bestGameScore"]   | 0;
    _state.questDay        = doc["questDay"]        | 0;
    _state.dayTicks        = doc["dayTicks"]        | 0;

    // Phase 1 state. Absent in a v1 save, in which case every default is
    // already correct and the migration step below simply confirms it.
    {
        // An unknown form means "we do not know what this pet is", which is
        // exactly FORM_UNSET. Clamping to UNSET rather than to the first real
        // form also means a save from a future firmware with more forms
        // degrades to undecided instead of silently claiming to be a
        // Pathfinder.
        const int rawForm = doc["form"] | (int)FORM_UNSET;
        _state.form = (rawForm > (int)FORM_UNSET && rawForm < (int)FORM_COUNT)
                    ? static_cast<PetForm>(rawForm)
                    : FORM_UNSET;
    }
    _state.roamPoints    = doc["roamPoints"]    | 0;
    _state.stepCount     = doc["stepCount"]     | 0;
    _state.roamSessions  = doc["roamSessions"]  | 0;
    _state.lastReportDay = doc["lastReportDay"] | 0;

    // Arrays are read by index rather than by push, so a pack or a save from a
    // build with a different array size can never overflow ours.
    JsonArray beh = doc["behaviour"].as<JsonArray>();
    for (uint8_t i = 0; i < FORM_BEHAVIOUR_COUNT; i++) {
        _state.behaviour[i] = (i < beh.size()) ? (uint16_t)(beh[i] | 0) : 0;
    }
    JsonArray inv = doc["items"].as<JsonArray>();
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) {
        _state.items[i] = (i < inv.size()) ? (uint16_t)(inv[i] | 0) : 0;
    }
    JsonArray den = doc["denSlots"].as<JsonArray>();
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) {
        const int rawSlot = (i < den.size()) ? (int)(den[i] | 0) : 0;
        // Anything that is not a real item id becomes empty. The den renders
        // each slot by looking the id up in the item table, so a bogus id
        // would index past it.
        _state.denSlots[i] = (rawSlot > 0 && rawSlot < ITEM_TYPE_COUNT)
                           ? (uint8_t)rawSlot : 0;
    }
    // Worn cosmetics. Absent in a v1..v3 save, in which case the pet is bare,
    // which is the correct answer: there was no way to wear anything.
    //
    // Validated exactly like denSlots[] above and for the same reason. This
    // array is looked up in the item table every time the pet is drawn, on
    // every screen, so a bogus id would index past the table in the renderer
    // rather than somewhere a load error would be obvious.
    JsonArray worn = doc["wornSlots"].as<JsonArray>();
    for (uint8_t i = 0; i < WEAR_SLOT_COUNT; i++) {
        const int rawWorn = (i < worn.size()) ? (int)(worn[i] | 0) : 0;
        _state.wornSlots[i] = (rawWorn > 0 && rawWorn < ITEM_TYPE_COUNT)
                            ? (uint8_t)rawWorn : 0;
    }
    // The flourish. Absent in a v1..v4 save, in which case the pet has none,
    // which is the correct answer: there was no way to choose one.
    //
    // Bounded exactly like wornSlots[] above, and for exactly the same reason:
    // this id is looked up in the item table on every frame the pet is drawn,
    // so an out-of-range byte would read past the table in the renderer rather
    // than anywhere a load error would be visible.
    {
        const int rawFlourish = doc["flourishSlot"] | 0;
        _state.flourishSlot = (rawFlourish > 0 && rawFlourish < ITEM_TYPE_COUNT)
                            ? (uint8_t)rawFlourish : 0;
    }

    // ── Phase 2: HexPass ──────────────────────────────────────────────────
    // Absent in a v1 or v2 save, in which case every default is correct: no
    // secret, feature OFF, no encounters. Read here rather than in the HexPass
    // engine so pet_core stays the single owner of serialisation, and so this
    // file does not need to link against the crypto unit.
    {
        HexPassState& hp = _state.hexpass;
        hp = HexPassState();

        JsonObjectConst hpDoc = doc["hexpass"].as<JsonObjectConst>();

        // The secret is all-or-nothing. A truncated or malformed hex string
        // means we do not have an identity, NOT that we have a partial one:
        // half a secret would still derive stable-looking EIDs, which is worse
        // than having none, because it looks like it works.
        const char* secretHex = hpDoc["secret"] | (const char*)nullptr;
        if (secretHex != nullptr &&
            HexPassHex::decode(secretHex, hp.secret, HEXPASS_SECRET_BYTES)) {
            uint8_t acc = 0;
            for (uint8_t i = 0; i < HEXPASS_SECRET_BYTES; i++) acc = (uint8_t)(acc | hp.secret[i]);
            hp.secretValid = (acc != 0);
        }
        if (!hp.secretValid) {
            memset(hp.secret, 0, HEXPASS_SECRET_BYTES);
        }

        // Consent defaults to OFF and is only ever turned on by an explicit
        // true in the save. `| false` matters here more than anywhere else in
        // this function.
        hp.enabled       = hpDoc["enabled"]     | false;
        hp.privateMode   = hpDoc["private"]     | false;
        {
            // Range-checked like every other loaded enum. An unknown value
            // falls back to FRIENDS, the more private of the two, so a
            // corrupt or future save can never silently make a device more
            // visible than its owner chose.
            const int rawVis = hpDoc["vis"] | (int)HEXPASS_VIS_FRIENDS;
            hp.visibility = (rawVis >= 0 && rawVis < (int)HEXPASS_VIS_COUNT)
                          ? (HexPassVisibility)rawVis
                          : HEXPASS_VIS_FRIENDS;
        }
        hp.epoch         = hpDoc["epoch"]       | 0;
        hp.seq           = hpDoc["seq"]         | 0;
        hp.day           = hpDoc["day"]         | 0;
        hp.recordedToday = hpDoc["today"]       | 0;

        // Both index a compiled-in string table on the card screen.
        const int rawBadge = hpDoc["badge"] | 0;
        hp.badge = (rawBadge >= 0 && rawBadge < HEXPASS_BADGE_COUNT)
                 ? (uint8_t)rawBadge : HEXPASS_BADGE_NONE;
        const int rawGreeting = hpDoc["greeting"] | 0;
        hp.greeting = (rawGreeting >= 0 && rawGreeting < HEXPASS_GREETING_COUNT)
                    ? (uint8_t)rawGreeting : 0;

        // The ring. Read by index against OUR array bound, never by push, so a
        // save written by a build with a bigger ring cannot overflow ours.
        JsonArrayConst enc = hpDoc["enc"].as<JsonArrayConst>();
        hp.encounterCount = 0;
        for (JsonObjectConst row : enc) {
            if (hp.encounterCount >= HEXPASS_MAX_ENCOUNTERS) {
                break;
            }
            HexPassEncounter e;
            const char* fidHex = row["f"] | (const char*)nullptr;
            if (fidHex == nullptr ||
                !HexPassHex::decode(fidHex, e.friendId, HEXPASS_FRIEND_BYTES)) {
                // Without a valid FriendID the record identifies nobody, so
                // there is nothing to keep. Skipped rather than zero-filled: a
                // zero FriendID would collide with every other broken record.
                continue;
            }
            e.lastEpoch = row["ep"] | 0;
            e.lastSeq   = row["sq"] | 0;
            e.meetCount = row["n"]  | 0;
            e.lastDay   = row["d"]  | 0;
            const int rawToday = row["t"] | 0;
            e.metToday = (rawToday >= 0 && rawToday <= 0xFF) ? (uint8_t)rawToday : 0;

            // Clamped, not dropped. Unlike a received card - which is rejected
            // outright when a field is out of range, because storing a clamped
            // value would record something the sender never sent - this record
            // already exists and its FriendID and meet count are the parts
            // that carry meaning. Losing a real friendship over one corrupt
            // display byte would be the worse trade. Every clamp target is a
            // valid index by construction.
            const int rawStage = row["s"] | (int)HEXPASS_STAGE_MIN;
            e.stage = (rawStage >= HEXPASS_STAGE_MIN && rawStage <= HEXPASS_STAGE_MAX)
                    ? (uint8_t)rawStage : (uint8_t)HEXPASS_STAGE_MIN;
            const int rawForm = row["fm"] | 0;
            e.form = (rawForm >= 0 && rawForm <= HEXPASS_FORM_MAX)
                   ? (uint8_t)rawForm : 0;
            const int rawEBadge = row["b"] | 0;
            e.badge = (rawEBadge >= 0 && rawEBadge < HEXPASS_BADGE_COUNT)
                    ? (uint8_t)rawEBadge : (uint8_t)HEXPASS_BADGE_NONE;
            const int rawEGreeting = row["g"] | 0;
            e.greeting = (rawEGreeting >= 0 && rawEGreeting < HEXPASS_GREETING_COUNT)
                       ? (uint8_t)rawEGreeting : 0;

            hp.encounters[hp.encounterCount] = e;
            hp.encounterCount++;
        }

        JsonArrayConst blk = hpDoc["blk"].as<JsonArrayConst>();
        hp.blockedCount = 0;
        for (JsonVariantConst v : blk) {
            if (hp.blockedCount >= HEXPASS_MAX_BLOCKED) {
                break;
            }
            const char* fidHex = v.as<const char*>();
            if (fidHex == nullptr ||
                !HexPassHex::decode(fidHex, hp.blocked[hp.blockedCount],
                                    HEXPASS_FRIEND_BYTES)) {
                // A block entry that will not decode must not be silently
                // dropped INTO the list as zeros: that would be a block on the
                // all-zero FriendID rather than on whoever the owner refused.
                memset(hp.blocked[hp.blockedCount], 0, HEXPASS_FRIEND_BYTES);
                continue;
            }
            hp.blockedCount++;
        }
    }

    _state.schemaVersion = loadedVersion;
    refreshPerks();

    const bool changed = migrate(loadedVersion);
    // A migrated save is only on disk in its old shape, so mark it dirty to get
    // the upgraded form written out. Otherwise the migration re-runs on every
    // boot and any one-way step would compound.
    _state.dirty        = changed;

    Serial.printf("[Pet] Loaded: %s stage=%d xp=%lu (schema %u)\n",
                  _state.name, _state.stage, _state.stats.xp,
                  (unsigned)loadedVersion);
    return true;
}

bool PetCore::migrate(uint16_t fromVersion) {
    if (fromVersion >= PET_SCHEMA_VERSION) {
        _state.schemaVersion = PET_SCHEMA_VERSION;
        return false;
    }

    Serial.printf("[Pet] Migrating save schema %u -> %u\n",
                  (unsigned)fromVersion, (unsigned)PET_SCHEMA_VERSION);

    // Steps are guarded by the version they upgrade FROM and fall through, so
    // a very old save walks every step in order.

    if (fromVersion < 2) {
        // v1 -> v2: the Phase 1 axes did not exist. Every new field already
        // holds its correct default from the struct initialiser (form unset,
        // empty inventory, empty den, zero roam), so there is nothing to
        // convert; what matters is what we must NOT do.
        //
        // Deliberately NOT seeding `behaviour` from existing counters. It is
        // tempting to backfill, say, wifiScans into the Pathfinder slot so a
        // long-standing pet arrives with an identity already. But those
        // counters were accumulated when patrolling was the only thing the pet
        // could do, so every existing pet would be declared a Pathfinder on
        // the strength of having been used at all. That is a fabricated
        // identity, and worse, it is unfalsifiable: the owner never chose it
        // and cannot see why. Existing pets start FORM_UNSET and earn a form
        // from what their owner does next, same as a new one.
        _state.form = FORM_UNSET;
        Serial.println("[Pet] v1 -> v2: form axis, inventory, den, roam added");
    }

    if (fromVersion < 3) {
        // v2 -> v3: HexPass did not exist. Every field of HexPassState already
        // holds its correct default from the struct initialiser, and the two
        // that matter are worth being explicit about.
        //
        // `enabled` STAYS FALSE. An existing pet must not wake up after a
        // firmware update quietly broadcasting an identifier in public. The
        // threat model is unambiguous about this and it is the one line in the
        // whole migration that would be a genuine harm to get wrong: "Off by
        // default. HexPass transmits nothing until the owner explicitly
        // enables it. Not opt-out, not on-with-a-notice." An upgrade is not an
        // owner enabling something.
        _state.hexpass.enabled     = false;
        _state.hexpass.privateMode = false;
        // Same argument one level down: even once an owner enables HexPass,
        // an upgrade must not decide they wanted to be readable by strangers.
        // FRIENDS is the private end of the scale and is where a migration
        // leaves them.
        _state.hexpass.visibility  = HEXPASS_VIS_FRIENDS;

        // No secret is minted here. HexPass::begin() generates one on first
        // run; doing it during a migration would mean an identity created by a
        // code path that has no access to the hardware RNG in every build that
        // shares this function (the simulator and the test runner do not have
        // esp_random()).
        _state.hexpass.secretValid = false;
        memset(_state.hexpass.secret, 0, HEXPASS_SECRET_BYTES);

        Serial.println("[Pet] v2 -> v3: HexPass added, disabled by default");
    }

    if (fromVersion < 4) {
        // v3 -> v4: worn cosmetics did not exist. There was no way to put
        // anything on the pet, so every existing save is correctly BARE and
        // the struct initialiser already says so.
        //
        // Explicit anyway, for the reason the den's slots are validated on
        // load: this array is indexed into the item table when the pet is
        // drawn, and "it must already be zero" is an assumption about a struct
        // that a future field reorder could quietly break.
        memset(_state.wornSlots, 0, sizeof(_state.wornSlots));
        Serial.println("[Pet] v3 -> v4: worn cosmetic slots added, pet is bare");
    }

    if (fromVersion < 5) {
        // v4 -> v5: the flourish slot did not exist. COSMETIC_FLOURISH items
        // were craftable in v2 onwards but nothing read them, so an existing
        // save may well own a PACKET FLURRY - and it still must arrive with
        // NONE selected. Owning something is not choosing it, and a migration
        // that switched on an effect the owner never picked would be the
        // firmware making a decision on their behalf. The same argument the
        // v3 step makes about HexPass, one scale down.
        //
        // Explicit rather than trusting the struct initialiser, for the reason
        // the v3 -> v4 step gives: this byte is looked up in the item table by
        // the renderer, and "it must already be zero" is an assumption about a
        // struct layout that a future field reorder could quietly break.
        _state.flourishSlot = 0;
        Serial.println("[Pet] v4 -> v5: flourish slot added, none selected");
    }

    _state.schemaVersion = PET_SCHEMA_VERSION;
    return true;
}

String PetCore::saveToJson() const {
    JsonDocument doc;
    doc["schema"]       = PET_SCHEMA_VERSION;
    doc["name"]         = _state.name;
    doc["stage"]        = _state.stage;
    doc["hunger"]       = _state.stats.hunger;
    doc["mood"]         = _state.stats.mood;
    doc["energy"]       = _state.stats.energy;
    doc["xp"]           = _state.stats.xp;
    doc["trust"]        = _state.stats.trust;
    doc["mischief"]     = _state.stats.mischief;
    doc["health"]       = _state.stats.health;
    doc["trait0"]        = _state.traits[0];
    doc["trait1"]        = _state.traits[1];
    doc["interactions"]  = _state.interactions;
    doc["wifiScans"]     = _state.wifiScans;
    doc["usbConnects"]   = _state.usbConnects;
    JsonArray seenWifi = doc["seenWifi"].to<JsonArray>();
    for (uint16_t i = 0; i < _state.seenWifiCount; i++) {
        seenWifi.add(_state.seenWifi[i]);
    }
    JsonArray seenBle = doc["seenBle"].to<JsonArray>();
    for (uint16_t i = 0; i < _state.seenBleCount; i++) {
        seenBle.add(_state.seenBle[i]);
    }
    doc["masteryXP"]        = _state.masteryXP;
    doc["masteryRank"]      = _state.masteryRank;
    doc["threatOpenCount"]  = _state.threatOpenCount;
    doc["threatDuplicateCount"] = _state.threatDuplicateCount;
    doc["threatTrackerCount"] = _state.threatTrackerCount;
    doc["missionsCompleted"] = _state.missionsCompleted;
    doc["perkMask"]         = _state.perkMask;
    doc["saveSeq"]       = _state.saveSeq;
    doc["hatched"]       = _state.hatched;
    doc["idleTicks"]     = _state.idleTicks;
    doc["hibernating"]   = _state.hibernating;
    doc["checkIns"]        = _state.checkIns;
    doc["questsCompleted"] = _state.questsCompleted;
    doc["bestGameScore"]   = _state.bestGameScore;
    doc["questDay"]        = _state.questDay;
    doc["dayTicks"]        = _state.dayTicks;

    doc["form"]          = (int)_state.form;
    doc["roamPoints"]    = _state.roamPoints;
    doc["stepCount"]     = _state.stepCount;
    doc["roamSessions"]  = _state.roamSessions;
    doc["lastReportDay"] = _state.lastReportDay;

    JsonArray beh = doc["behaviour"].to<JsonArray>();
    for (uint8_t i = 0; i < FORM_BEHAVIOUR_COUNT; i++) beh.add(_state.behaviour[i]);
    JsonArray inv = doc["items"].to<JsonArray>();
    for (uint8_t i = 0; i < ITEM_TYPE_COUNT; i++) inv.add(_state.items[i]);
    JsonArray den = doc["denSlots"].to<JsonArray>();
    for (uint8_t i = 0; i < DEN_SLOT_COUNT; i++) den.add(_state.denSlots[i]);
    JsonArray worn = doc["wornSlots"].to<JsonArray>();
    for (uint8_t i = 0; i < WEAR_SLOT_COUNT; i++) worn.add(_state.wornSlots[i]);
    doc["flourishSlot"] = _state.flourishSlot;

    // ── Phase 2: HexPass ──────────────────────────────────────────────────
    // Short keys throughout the encounter rows. There can be 16 of them in a
    // save that already runs to a few kilobytes on a board with no PSRAM, and
    // "friendId"/"meetCount" spelled out 16 times buys nothing a comment here
    // does not.
    {
        const HexPassState& hp = _state.hexpass;
        JsonObject hpDoc = doc["hexpass"].to<JsonObject>();

        // The secret is written in the clear, because there is nowhere on this
        // hardware to put it that is meaningfully better: there is no secure
        // element and no flash encryption key the firmware could hold back
        // from itself. That is a real limitation, recorded in
        // docs/p2w1-wiring.md rather than papered over. Anyone holding the SD
        // card can derive this device's past and future EIDs.
        char hex[HEXPASS_SECRET_BYTES * 2 + 1];
        if (hp.secretValid) {
            HexPassHex::encode(hp.secret, HEXPASS_SECRET_BYTES, hex);
            hpDoc["secret"] = hex;
        }
        hpDoc["enabled"]  = hp.enabled;
        hpDoc["private"]  = hp.privateMode;
        hpDoc["vis"]      = (int)hp.visibility;
        hpDoc["epoch"]    = hp.epoch;
        hpDoc["seq"]      = hp.seq;
        hpDoc["day"]      = hp.day;
        hpDoc["today"]    = hp.recordedToday;
        hpDoc["badge"]    = hp.badge;
        hpDoc["greeting"] = hp.greeting;

        JsonArray enc = hpDoc["enc"].to<JsonArray>();
        for (uint8_t i = 0; i < hp.encounterCount && i < HEXPASS_MAX_ENCOUNTERS; i++) {
            const HexPassEncounter& e = hp.encounters[i];
            char fid[HEXPASS_FRIEND_BYTES * 2 + 1];
            HexPassHex::encode(e.friendId, HEXPASS_FRIEND_BYTES, fid);
            JsonObject row = enc.add<JsonObject>();
            row["f"]  = fid;
            row["ep"] = e.lastEpoch;
            row["sq"] = e.lastSeq;
            row["n"]  = e.meetCount;
            row["d"]  = e.lastDay;
            row["t"]  = e.metToday;
            row["s"]  = e.stage;
            row["fm"] = e.form;
            row["b"]  = e.badge;
            row["g"]  = e.greeting;
        }

        JsonArray blk = hpDoc["blk"].to<JsonArray>();
        for (uint8_t i = 0; i < hp.blockedCount && i < HEXPASS_MAX_BLOCKED; i++) {
            char fid[HEXPASS_FRIEND_BYTES * 2 + 1];
            HexPassHex::encode(hp.blocked[i], HEXPASS_FRIEND_BYTES, fid);
            blk.add(fid);
        }
    }

    String output;
    serializeJsonPretty(doc, output);
    return output;
}
