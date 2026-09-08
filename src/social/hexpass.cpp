#include "hexpass.h"
#include "../pet/pet_core.h"

#include <string.h>

#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
#include <esp_system.h>
#include <esp_random.h>
#else
#include <stdlib.h>
#endif

// ── HexHound - HexPass Implementation ────────────────────────────
//
// No radio. See the banner in hexpass.h. If you are reading this file looking
// for where advertising happens, it does not exist yet and that is deliberate.

const char* const HEXPASS_GREETINGS[] = {
    "...",
    "hey",
    "sniff sniff",
    "good hunting",
    "seen anything?",
    "stay sharp",
    "nice antenna",
    "bark"
};

const char* const HEXPASS_BADGES[] = {
    "none",
    "first patrol",
    "night owl",
    "packet hound",
    "beacon finder",
    "quest runner",
    "long haul",
    "good dog"
};

namespace {

// ── Domain separation labels ──────────────────────────────────────────────
// Distinct labels are what keep three different derivations from the same
// secret from being the same function. Written as explicit byte arrays with no
// terminator so the message length is exactly the label length; a stray NUL in
// one of them and not another would be an invisible protocol change.
const uint8_t LBL_EID[]    = { 'h','e','x','p','a','s','s','-','e','i','d' };
const uint8_t LBL_FRIEND[] = { 'h','e','x','p','a','s','s','-','f','r','i','e','n','d' };
const uint8_t LBL_TAG[]    = { 'h','e','x','p','a','s','s','-','t','a','g' };

void (*s_randomOverride)(uint8_t* out, size_t len) = nullptr;

void fillRandom(uint8_t* out, size_t len) {
    if (s_randomOverride != nullptr) {
        s_randomOverride(out, len);
        return;
    }
#if !defined(SIMULATOR_BUILD) && !defined(UNIT_TEST)
    // The hardware RNG. This is the only entropy that ever matters: the
    // simulator and the tests have no radio, so the identity they generate is
    // never seen by anybody.
    size_t i = 0;
    while (i < len) {
        const uint32_t r = esp_random();
        const size_t take = (len - i < 4) ? (len - i) : 4;
        memcpy(out + i, &r, take);
        i += take;
    }
#else
    // NOT cryptographic, and it does not need to be. Off-target builds do not
    // transmit, so this identity is a fixture. It must never become the
    // hardware path; the guard above is what prevents that.
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(rand() & 0xFF);
    }
#endif
}

// Epoch encoding for the EID message. Little-endian, fixed here so the radio
// layer cannot pick a different one later. See the note in hexpass.h.
void putEpochLE(uint8_t* out, uint32_t epoch) {
    out[0] = (uint8_t)(epoch & 0xFF);
    out[1] = (uint8_t)((epoch >> 8) & 0xFF);
    out[2] = (uint8_t)((epoch >> 16) & 0xFF);
    out[3] = (uint8_t)((epoch >> 24) & 0xFF);
}

// tag = HMAC-SHA256(HMAC-SHA256(eid, "hexpass-tag"), payload[0..24])[0..7]
void computeTag(const uint8_t* signedBytes, const uint8_t eid[HEXPASS_EID_BYTES],
                uint8_t out[HEXPASS_TAG_BYTES]) {
    uint8_t tagKey[HEXPASS_SHA256_BYTES];
    HexPassCrypto::hmacSha256(eid, HEXPASS_EID_BYTES, LBL_TAG, sizeof(LBL_TAG), tagKey);

    uint8_t full[HEXPASS_SHA256_BYTES];
    HexPassCrypto::hmacSha256(tagKey, sizeof(tagKey), signedBytes, HEXPASS_SIGNED_BYTES, full);
    memcpy(out, full, HEXPASS_TAG_BYTES);

    memset(tagKey, 0, sizeof(tagKey));
}

bool isAllZero(const uint8_t* p, size_t len) {
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (uint8_t)(acc | p[i]);
    }
    return acc == 0;
}

// Serialises a card into the fixed 33-byte layout with an explicit counter, so
// the transmit path and the "show me what you would send" path share one
// encoder and cannot disagree about what is being broadcast.
void encodeCard(uint8_t out[HEXPASS_PAYLOAD_BYTES],
                const uint8_t eid[HEXPASS_EID_BYTES],
                uint8_t stage, uint8_t form, uint8_t badge, uint8_t greeting,
                uint32_t counter, bool discoverable = false) {
    memset(out, 0, HEXPASS_PAYLOAD_BYTES);
    out[HEXPASS_OFF_VERSION] = HEXPASS_PROTOCOL_VERSION;
    memcpy(out + HEXPASS_OFF_EID, eid, HEXPASS_EID_BYTES);
    // Stage is 1..5, so bits 3..7 of this byte are spare and the discoverable
    // flag rides in the top one. Putting it here rather than adding a byte is
    // what keeps the payload exactly 33 bytes in BOTH modes: a card whose
    // length changed with the setting would announce the setting to anyone
    // counting bytes, before they decoded anything at all.
    out[HEXPASS_OFF_STAGE]    = (uint8_t)(stage | (discoverable ? HEXPASS_STAGE_DISCOVERABLE_BIT : 0));
    out[HEXPASS_OFF_FORM]     = form;
    out[HEXPASS_OFF_BADGE]    = badge;
    out[HEXPASS_OFF_GREETING] = greeting;
    putEpochLE(out + HEXPASS_OFF_COUNTER, counter);
    computeTag(out, eid, out + HEXPASS_OFF_TAG);
}

}  // namespace

HexPass& HexPass::instance() {
    static HexPass hp;
    return hp;
}

HexPassState& HexPass::st() {
    return PetCore::instance().state().hexpass;
}

const HexPassState& HexPass::st() const {
    return PetCore::instance().state().hexpass;
}

void HexPass::setRandomSource(void (*fn)(uint8_t* out, size_t len)) {
    s_randomOverride = fn;
}

void HexPass::generateSecret() {
    HexPassState& s = st();
    fillRandom(s.secret, HEXPASS_SECRET_BYTES);
    // An all-zero secret would make every device on earth derive the same
    // identifiers. Astronomically unlikely from a real RNG and catastrophic if
    // it happened, so it is checked rather than assumed.
    if (isAllZero(s.secret, HEXPASS_SECRET_BYTES)) {
        s.secretValid = false;
        return;
    }
    s.secretValid = true;
    PetCore::instance().state().dirty = true;
}

void HexPass::resetVolatile() {
    for (uint8_t i = 0; i < REPLAY_SLOTS; i++) {
        _replay[i] = ReplaySlot();
    }
    _replayNext = 0;
    _counter    = 0;
}

void HexPass::begin() {
    HexPassState& s = st();

    if (!s.secretValid || isAllZero(s.secret, HEXPASS_SECRET_BYTES)) {
        generateSecret();
    }

    // Advance the epoch on every boot. Without this a device that lost power
    // would resume at the epoch it was already using and re-emit identifiers a
    // sniffer may already hold, which is precisely the linkage rotation exists
    // to prevent.
    rotate();

    Serial.printf("[HexPass] begin: secret=%s crypto=%s enabled=%d epoch=%lu\n",
                  s.secretValid ? "ok" : "MISSING",
                  HexPassCrypto::backendName(),
                  (int)s.enabled,
                  (unsigned long)s.epoch);
}

void HexPass::afterLoad() {
    // A load replaces the whole persisted block, so anything derived from it
    // is stale. Deliberately does NOT rotate: begin() owns rotation, and doing
    // it in both would burn two epochs per boot for no benefit.
    resetVolatile();
}

void HexPass::rotate() {
    HexPassState& s = st();
    s.epoch++;
    // The counter is monotonic WITHIN an epoch, so a new epoch restarts it.
    // The replay table is keyed by EID, and every EID it holds belonged to the
    // epoch that just ended, so it is cleared with it.
    resetVolatile();
    PetCore::instance().state().dirty = true;

    // The BLE address change goes HERE and nowhere else. One trigger, not two
    // timers. See the rotation comment in hexpass.h.
    if (_rotationHook != nullptr) {
        _rotationHook(_rotationUser);
    }
}

void HexPass::setRotationHook(void (*hook)(void* user), void* user) {
    _rotationHook = hook;
    _rotationUser = user;
}

bool HexPass::hasSecret() const {
    const HexPassState& s = st();
    return s.secretValid && !isAllZero(s.secret, HEXPASS_SECRET_BYTES);
}

uint32_t HexPass::epoch() const {
    return st().epoch;
}

bool HexPass::deriveEid(uint32_t epochValue, uint8_t out[HEXPASS_EID_BYTES]) const {
    memset(out, 0, HEXPASS_EID_BYTES);
    if (!hasSecret()) {
        return false;
    }

    uint8_t msg[sizeof(LBL_EID) + 4];
    memcpy(msg, LBL_EID, sizeof(LBL_EID));
    putEpochLE(msg + sizeof(LBL_EID), epochValue);

    uint8_t full[HEXPASS_SHA256_BYTES];
    HexPassCrypto::hmacSha256(st().secret, HEXPASS_SECRET_BYTES,
                              msg, sizeof(msg), full);
    memcpy(out, full, HEXPASS_EID_BYTES);
    return true;
}

bool HexPass::currentEid(uint8_t out[HEXPASS_EID_BYTES]) const {
    return deriveEid(st().epoch, out);
}

void HexPass::friendId(const uint8_t a[HEXPASS_EID_BYTES],
                       const uint8_t b[HEXPASS_EID_BYTES],
                       uint8_t out[HEXPASS_FRIEND_BYTES]) {
    // Sorted, so both sides key the HMAC with the same 32 bytes and therefore
    // land on the same FriendID no matter which of them is "a".
    const bool aFirst = memcmp(a, b, HEXPASS_EID_BYTES) <= 0;
    uint8_t key[HEXPASS_EID_BYTES * 2];
    memcpy(key, aFirst ? a : b, HEXPASS_EID_BYTES);
    memcpy(key + HEXPASS_EID_BYTES, aFirst ? b : a, HEXPASS_EID_BYTES);

    uint8_t full[HEXPASS_SHA256_BYTES];
    HexPassCrypto::hmacSha256(key, sizeof(key), LBL_FRIEND, sizeof(LBL_FRIEND), full);
    memcpy(out, full, HEXPASS_FRIEND_BYTES);
}

bool HexPass::buildPayload(uint8_t out[HEXPASS_PAYLOAD_BYTES]) {
    memset(out, 0, HEXPASS_PAYLOAD_BYTES);
    if (!broadcasting() || !hasSecret()) {
        return false;
    }

    uint8_t eid[HEXPASS_EID_BYTES];
    if (!currentEid(eid)) {
        return false;
    }

    const PetState& p = PetCore::instance().state();
    const HexPassState& s = st();

    // The pet's own stage and form are bounded here as well as on parse. They
    // come from our save, which is untrusted input by the same argument that
    // makes a received card untrusted.
    uint8_t stage = (uint8_t)p.stage;
    if (stage < HEXPASS_STAGE_MIN || stage > HEXPASS_STAGE_MAX) {
        stage = HEXPASS_STAGE_MIN;
    }
    uint8_t form = (uint8_t)p.form;
    if (form > HEXPASS_FORM_MAX) {
        form = 0;
    }

    // Visibility gate. In FRIENDS mode the personality fields carry no
    // information about this pet.
    //
    // They are filled with fresh random bytes rather than zeroed, and the
    // payload stays exactly the same length either way. Both details matter:
    // a shorter card in one mode would announce the mode by its length alone,
    // and an all-zero personality block would announce it just as loudly. Only
    // the single `discoverable` bit distinguishes the modes, and that bit is
    // something the owner consciously chose to broadcast.
    //
    // Friends are unaffected: they hold the seed, recognise the EID, and read
    // the pet from their own stored record rather than from the card.
    uint8_t outForm = form;
    uint8_t outBadge = s.badge;
    uint8_t outGreeting = s.greeting;
    const bool discoverable = (s.visibility == HEXPASS_VIS_DISCOVERABLE);

    if (!discoverable) {
        uint8_t noise[3];
        fillRandom(noise, sizeof(noise));
        outForm     = (uint8_t)(noise[0] % (HEXPASS_FORM_MAX + 1));
        outBadge    = (uint8_t)(noise[1] % HEXPASS_BADGE_COUNT);
        outGreeting = (uint8_t)(noise[2] % HEXPASS_GREETING_COUNT);
    }

    encodeCard(out, eid, stage, outForm, outBadge, outGreeting, _counter,
               discoverable);
    _counter++;
    return true;
}

HexPassParse HexPass::parsePayload(const uint8_t* buf, size_t len, HexPassCard& out) {
    // Fixed size, no optional fields. A short buffer is not a truncated card to
    // be salvaged; it is not a card.
    if (buf == nullptr || len != HEXPASS_PAYLOAD_BYTES) {
        return HEXPASS_PARSE_BAD_LENGTH;
    }
    if (buf[HEXPASS_OFF_VERSION] != HEXPASS_PROTOCOL_VERSION) {
        // Reject rather than guess. A future version may reuse these offsets
        // for different fields, and reading them anyway would record garbage.
        return HEXPASS_PARSE_BAD_VERSION;
    }

    // Verify before believing any field. The tag covers bytes 0..24, so a
    // single flipped bit anywhere in the card fails here.
    uint8_t expect[HEXPASS_TAG_BYTES];
    computeTag(buf, buf + HEXPASS_OFF_EID, expect);
    if (!HexPassCrypto::equalCT(expect, buf + HEXPASS_OFF_TAG, HEXPASS_TAG_BYTES)) {
        return HEXPASS_PARSE_BAD_TAG;
    }

    const uint8_t stageByte = buf[HEXPASS_OFF_STAGE];
    const bool senderDiscoverable =
        (stageByte & HEXPASS_STAGE_DISCOVERABLE_BIT) != 0;
    const uint8_t stage    = (uint8_t)(stageByte & HEXPASS_STAGE_MASK);
    const uint8_t form     = buf[HEXPASS_OFF_FORM];
    const uint8_t badge    = buf[HEXPASS_OFF_BADGE];
    const uint8_t greeting = buf[HEXPASS_OFF_GREETING];

    // Every one of these indexes a table on the encounter screen. A tag that
    // verifies proves the card was not edited in flight; it does NOT prove the
    // sender was honest, because anyone can mint a card. So the ranges are
    // checked, and an out-of-range card is dropped rather than clamped: a
    // clamp would store a value the sender never sent.
    if (stage < HEXPASS_STAGE_MIN || stage > HEXPASS_STAGE_MAX ||
        form > HEXPASS_FORM_MAX ||
        badge >= HEXPASS_BADGE_COUNT ||
        greeting >= HEXPASS_GREETING_COUNT) {
        return HEXPASS_PARSE_BAD_FIELD;
    }

    // An all-zero EID is what deriveEid() emits when a device has no secret.
    // It is not an identity and must not become one.
    if (isAllZero(buf + HEXPASS_OFF_EID, HEXPASS_EID_BYTES)) {
        return HEXPASS_PARSE_BAD_FIELD;
    }

    out.version = buf[HEXPASS_OFF_VERSION];
    memcpy(out.eid, buf + HEXPASS_OFF_EID, HEXPASS_EID_BYTES);
    out.stage    = stage;
    out.discoverable = senderDiscoverable;
    // A sender who is not discoverable filled these with noise. Carrying that
    // noise forward would have the UI cheerfully display a form and badge the
    // sender never chose, which is a fabricated statement about another
    // person's pet. Blank them here so the only way to show a stranger's
    // personality is for the stranger to have opted into showing it.
    //
    // Friends are unaffected: recognition goes through the stored friendship
    // record, not through these fields.
    out.form     = senderDiscoverable ? form     : 0;
    out.badge    = senderDiscoverable ? badge    : HEXPASS_BADGE_NONE;
    out.greeting = senderDiscoverable ? greeting : 0;
    out.counter  = (uint32_t)buf[HEXPASS_OFF_COUNTER]
                 | ((uint32_t)buf[HEXPASS_OFF_COUNTER + 1] << 8)
                 | ((uint32_t)buf[HEXPASS_OFF_COUNTER + 2] << 16)
                 | ((uint32_t)buf[HEXPASS_OFF_COUNTER + 3] << 24);
    return HEXPASS_PARSE_OK;
}

bool HexPass::seenBefore(const uint8_t eid[HEXPASS_EID_BYTES], uint32_t counter) {
    for (uint8_t i = 0; i < REPLAY_SLOTS; i++) {
        if (!_replay[i].used) {
            continue;
        }
        if (memcmp(_replay[i].eid, eid, HEXPASS_EID_BYTES) != 0) {
            continue;
        }
        if (counter <= _replay[i].lastCounter) {
            return true;   // replay: counter did not advance
        }
        _replay[i].lastCounter = counter;
        return false;
    }

    // Unknown EID. Round-robin insert, so the table is bounded and an attacker
    // spraying fresh EIDs evicts only other attacker entries in the long run.
    _replay[_replayNext].used = true;
    memcpy(_replay[_replayNext].eid, eid, HEXPASS_EID_BYTES);
    _replay[_replayNext].lastCounter = counter;
    _replayNext = (uint8_t)((_replayNext + 1) % REPLAY_SLOTS);
    return false;
}

void HexPass::refreshDay() {
    HexPassState& s = st();
    const uint16_t today = PetCore::instance().state().questDay;
    if (s.day == today) {
        return;
    }
    s.day = today;
    s.recordedToday = 0;
    for (uint8_t i = 0; i < s.encounterCount; i++) {
        s.encounters[i].metToday = 0;
        s.encounters[i].lastDay  = today;
    }
}

uint8_t HexPass::evictionVictim() const {
    const HexPassState& s = st();
    // Oldest first, where oldest means least recently seen. A pure
    // first-in-first-out would throw away a friend you meet every day simply
    // because you met them first, which is the opposite of what the store is
    // for. Under a flood of unfamiliar cards the two behave identically,
    // because flood entries are always the most recent.
    uint8_t victim = 0;
    uint32_t oldest = s.encounters[0].lastSeq;
    for (uint8_t i = 1; i < s.encounterCount; i++) {
        if (s.encounters[i].lastSeq < oldest) {
            oldest = s.encounters[i].lastSeq;
            victim = i;
        }
    }
    return victim;
}

int HexPass::findFriend(const uint8_t fid[HEXPASS_FRIEND_BYTES]) const {
    const HexPassState& s = st();
    for (uint8_t i = 0; i < s.encounterCount; i++) {
        if (memcmp(s.encounters[i].friendId, fid, HEXPASS_FRIEND_BYTES) == 0) {
            return (int)i;
        }
    }
    return -1;
}

HexPassRecord HexPass::record(const HexPassCard& card) {
    HexPassState& s = st();

    if (!hasSecret()) {
        return HEXPASS_RECORD_NO_SECRET;
    }
    // Private mode suppresses RECORDING as well as advertising. The threat
    // model words it as "stops advertising immediately"; a device that quietly
    // keeps building a social graph while the owner believes it is private
    // would be the worse of the two readings, so this takes the stricter one.
    if (!broadcasting()) {
        return HEXPASS_RECORD_DISABLED;
    }

    uint8_t mine[HEXPASS_EID_BYTES];
    if (!currentEid(mine)) {
        return HEXPASS_RECORD_NO_SECRET;
    }

    // Our own card coming back at us, either reflected or replayed by someone
    // who captured it. Checked across the epoch boundary too, so a card built
    // moments before a rotation is still recognised as ours.
    if (memcmp(mine, card.eid, HEXPASS_EID_BYTES) == 0) {
        return HEXPASS_RECORD_SELF;
    }
    if (s.epoch > 0) {
        uint8_t prev[HEXPASS_EID_BYTES];
        if (deriveEid(s.epoch - 1, prev) &&
            memcmp(prev, card.eid, HEXPASS_EID_BYTES) == 0) {
            return HEXPASS_RECORD_SELF;
        }
    }

    if (seenBefore(card.eid, card.counter)) {
        return HEXPASS_RECORD_REPLAY;
    }

    uint8_t fid[HEXPASS_FRIEND_BYTES];
    friendId(mine, card.eid, fid);

    if (isBlocked(fid)) {
        return HEXPASS_RECORD_BLOCKED;
    }

    refreshDay();

    const int existing = findFriend(fid);
    if (existing >= 0) {
        HexPassEncounter& e = s.encounters[existing];
        if (e.lastEpoch == s.epoch) {
            return HEXPASS_RECORD_RATE_EPOCH;
        }
        if (e.metToday >= HEXPASS_MAX_PER_FRIEND_PER_DAY) {
            return HEXPASS_RECORD_RATE_FRIEND;
        }
    }

    // The global cap is checked only once the per-friend rules have agreed
    // this would actually be stored, so a rate-limited repeat does not eat the
    // day's budget.
    if (s.recordedToday >= HEXPASS_MAX_PER_DAY) {
        return HEXPASS_RECORD_RATE_DAY;
    }

    s.seq++;
    s.recordedToday++;
    PetCore::instance().state().dirty = true;

    if (existing >= 0) {
        HexPassEncounter& e = s.encounters[existing];
        if (e.meetCount < 0xFFFF) {
            e.meetCount++;
        }
        if (e.metToday < 0xFF) {
            e.metToday++;
        }
        e.lastEpoch = s.epoch;
        e.lastSeq   = s.seq;
        e.lastDay   = s.day;
        e.stage     = card.stage;
        e.form      = card.form;
        e.badge     = card.badge;
        e.greeting  = card.greeting;
        return HEXPASS_RECORD_REPEAT;
    }

    uint8_t slot;
    if (s.encounterCount < HEXPASS_MAX_ENCOUNTERS) {
        slot = s.encounterCount;
        s.encounterCount++;
    } else {
        slot = evictionVictim();
    }

    HexPassEncounter& e = s.encounters[slot];
    e = HexPassEncounter();
    memcpy(e.friendId, fid, HEXPASS_FRIEND_BYTES);
    e.lastEpoch = s.epoch;
    e.lastSeq   = s.seq;
    e.lastDay   = s.day;
    e.meetCount = 1;
    e.metToday  = 1;
    e.stage     = card.stage;
    e.form      = card.form;
    e.badge     = card.badge;
    e.greeting  = card.greeting;
    return HEXPASS_RECORD_NEW;
}

uint8_t HexPass::encounterCount() const {
    return st().encounterCount;
}

const HexPassEncounter* HexPass::encounter(uint8_t index) const {
    const HexPassState& s = st();
    if (index >= s.encounterCount) {
        return nullptr;
    }
    return &s.encounters[index];
}

bool HexPass::block(const uint8_t fid[HEXPASS_FRIEND_BYTES]) {
    HexPassState& s = st();
    if (isBlocked(fid)) {
        return true;
    }
    if (s.blockedCount >= HEXPASS_MAX_BLOCKED) {
        return false;
    }
    memcpy(s.blocked[s.blockedCount], fid, HEXPASS_FRIEND_BYTES);
    s.blockedCount++;

    // Blocking is retroactive. Leaving the existing encounter in place would
    // mean the screen still shows a friendship the owner just refused.
    const int idx = findFriend(fid);
    if (idx >= 0) {
        for (uint8_t i = (uint8_t)idx; i + 1 < s.encounterCount; i++) {
            s.encounters[i] = s.encounters[i + 1];
        }
        s.encounterCount--;
        s.encounters[s.encounterCount] = HexPassEncounter();
    }

    PetCore::instance().state().dirty = true;
    return true;
}

bool HexPass::isBlocked(const uint8_t fid[HEXPASS_FRIEND_BYTES]) const {
    const HexPassState& s = st();
    for (uint8_t i = 0; i < s.blockedCount; i++) {
        if (memcmp(s.blocked[i], fid, HEXPASS_FRIEND_BYTES) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t HexPass::blockedCount() const {
    return st().blockedCount;
}

bool HexPass::enabled() const {
    return st().enabled;
}

void HexPass::setEnabled(bool on) {
    HexPassState& s = st();
    if (s.enabled == on) {
        return;
    }
    s.enabled = on;
    PetCore::instance().state().dirty = true;
}

bool HexPass::privateMode() const {
    return st().privateMode;
}

HexPassVisibility HexPass::visibility() const {
    return st().visibility;
}

bool HexPass::setVisibility(HexPassVisibility v) {
    if (v >= HEXPASS_VIS_COUNT) {
        return false;
    }
    HexPassState& s = st();
    if (s.visibility == v) {
        return true;
    }
    s.visibility = v;
    PetCore::instance().state().dirty = true;
    return true;
}

void HexPass::setPrivateMode(bool on) {
    HexPassState& s = st();
    if (s.privateMode == on) {
        return;
    }
    s.privateMode = on;
    PetCore::instance().state().dirty = true;
}

bool HexPass::broadcasting() const {
    const HexPassState& s = st();
    return s.enabled && !s.privateMode;
}

uint8_t HexPass::badge() const {
    return st().badge;
}

bool HexPass::setBadge(uint8_t badgeId) {
    if (badgeId >= HEXPASS_BADGE_COUNT) {
        return false;
    }
    HexPassState& s = st();
    s.badge = badgeId;
    PetCore::instance().state().dirty = true;
    return true;
}

uint8_t HexPass::greeting() const {
    return st().greeting;
}

bool HexPass::setGreeting(uint8_t greetingId) {
    if (greetingId >= HEXPASS_GREETING_COUNT) {
        return false;
    }
    HexPassState& s = st();
    s.greeting = greetingId;
    PetCore::instance().state().dirty = true;
    return true;
}

void HexPass::wipe() {
    HexPassState& s = st();

    // Order matters. The secret goes first so that a power loss part-way
    // through leaves a device with no identity rather than one with its old
    // identity and no encounters to explain it.
    memset(s.secret, 0, HEXPASS_SECRET_BYTES);
    s.secretValid = false;

    for (uint8_t i = 0; i < HEXPASS_MAX_ENCOUNTERS; i++) {
        s.encounters[i] = HexPassEncounter();
    }
    memset(s.blocked, 0, sizeof(s.blocked));
    s.encounterCount = 0;
    s.blockedCount   = 0;
    s.recordedToday  = 0;
    s.seq            = 0;

    // A fresh identity, and an epoch that keeps moving forward. Rolling the
    // epoch back to zero would let the new secret produce EIDs at epoch values
    // an observer already has recorded sightings for, which is a small but
    // free correlation to give away.
    generateSecret();
    s.epoch++;
    resetVolatile();

    // enabled/privateMode are deliberately left alone. A wipe is a reset of
    // identity, not a change of consent, and silently switching the feature
    // off would be a different action from the one the owner asked for.
    PetCore::instance().state().dirty = true;
    Serial.println("[HexPass] wiped: secret, encounters and friendships erased");
}

bool HexPass::describePayloadHex(char* out, size_t outLen) const {
    if (out == nullptr || outLen < (size_t)(HEXPASS_PAYLOAD_BYTES * 2 + 1)) {
        return false;
    }
    out[0] = '\0';
    if (!broadcasting() || !hasSecret()) {
        return false;
    }

    uint8_t eid[HEXPASS_EID_BYTES];
    if (!currentEid(eid)) {
        return false;
    }

    const PetState& p = PetCore::instance().state();
    const HexPassState& s = st();
    uint8_t stage = (uint8_t)p.stage;
    if (stage < HEXPASS_STAGE_MIN || stage > HEXPASS_STAGE_MAX) {
        stage = HEXPASS_STAGE_MIN;
    }
    uint8_t form = (uint8_t)p.form;
    if (form > HEXPASS_FORM_MAX) {
        form = 0;
    }

    uint8_t buf[HEXPASS_PAYLOAD_BYTES];
    // The counter is NOT advanced. Looking at what you would send must not be
    // the same act as sending it.
    //
    // This has to be TRUTHFUL, because it is the answer to "what am I telling
    // strangers?" and consent means nothing if the answer is flattering. So it
    // respects visibility: in FRIENDS mode the personality bytes show as zero,
    // matching the fact that they carry nothing about this pet. The bytes
    // actually broadcast are random rather than zero (see buildPayload) so
    // that a stranger cannot tell the modes apart by looking for a zero block;
    // the hex here is therefore representative of MEANING, not byte-identical
    // to any one advertisement. Anything else would be showing the owner a
    // card that claims a form they are not in fact broadcasting.
    const bool discoverable = (s.visibility == HEXPASS_VIS_DISCOVERABLE);
    encodeCard(buf, eid, stage,
               discoverable ? form       : 0,
               discoverable ? s.badge    : HEXPASS_BADGE_NONE,
               discoverable ? s.greeting : 0,
               _counter, discoverable);
    HexPassHex::encode(buf, HEXPASS_PAYLOAD_BYTES, out);
    return true;
}
