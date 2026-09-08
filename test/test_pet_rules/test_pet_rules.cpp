// ── HexHound - PetRules Unit Tests ───────────────────────────────

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/modules/wifi_module.h"
#include "../../src/modules/ble_module.h"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"
#include "../../src/pet/pet_rules.h"
// pet_rules grants craft materials on a patrol now, so the inventory it calls
// into has to be linked here as well - and the inventory carries the recipe
// pack loader with it. Same set, in the same order, as test_inventory uses.
// See src/content/material_yield.h.
#include "../../src/pet/pet_inventory.h"
#include "../../src/content/content_crypto.cpp"
#include "../../src/content/content_cbor.cpp"
#include "../../src/content/content_pack.cpp"
#include "../../src/pet/pet_inventory.cpp"
// pet_rules.cpp pays quest rewards through QuestEngine, so the engine has to
// be linked in or the suite does not build. It was added when reward payout
// landed and went unnoticed while the native linker was broken.
#include "../../src/content/content_store.cpp"
#include "../../src/content/quest_engine.cpp"
#include "../../src/pet/pet_rules.cpp"

#include <ArduinoJson.h>

#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))

// ── Local module fakes ─────────────────────────────────────────────────────

static const char* TEST_WIFI_SSIDS[] = {
    "HomeNetwork",
    "CoffeeShop_Free",
    "HomeNetwork",
    "OpenWiFi"
};

static const char* TEST_BLE_ADDRS[] = {
    "AA:BB:CC:DD:EE:01",
    "AA:BB:CC:DD:EE:02",
    "AA:BB:CC:DD:EE:03"
};

WiFiModule& WiFiModule::instance() {
    static WiFiModule mod;
    return mod;
}

void WiFiModule::init() {}

bool WiFiModule::startScan() {
    if (_scanning) return false;
    _scanning = true;
    _resultCount = 0;
    _openCount = 0;
    _dupeCount = 0;
    memset(_results, 0, sizeof(_results));
    return true;
}

bool WiFiModule::pollScan() {
    if (!_scanning) return false;
    _scanning = false;
    _resultCount = 4;

    for (int i = 0; i < _resultCount; i++) {
        strlcpy(_results[i].ssid, TEST_WIFI_SSIDS[i], sizeof(_results[i].ssid));
        snprintf(_results[i].bssid, sizeof(_results[i].bssid), "AA:BB:CC:DD:EE:%02d", i + 1);
        _results[i].channel = (uint8_t)(i + 1);
        _results[i].isOpen = (strcmp(TEST_WIFI_SSIDS[i], "OpenWiFi") == 0);
        _results[i].isDuplicate = false;
    }

    analyzeResults();

    auto& bus = EventBus::instance();
    bus.publish(EVENT_WIFI_SCAN_DONE, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isDuplicate) bus.publish(EVENT_WIFI_DUPLICATE_SSID, i);
        if (_results[i].isOpen) bus.publish(EVENT_WIFI_OPEN_NETWORK, i);
    }
    return true;
}

void WiFiModule::analyzeResults() {
    _openCount = 0;
    _dupeCount = 0;
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isOpen) _openCount++;
        if (isDuplicateSSID(_results[i].ssid, i)) {
            _results[i].isDuplicate = true;
            _dupeCount++;
        }
    }
}

bool WiFiModule::isDuplicateSSID(const char* ssid, int currentIndex) {
    for (int i = 0; i < currentIndex; i++) {
        if (strcmp(_results[i].ssid, ssid) == 0) return true;
    }
    return false;
}

BLEModule& BLEModule::instance() {
    static BLEModule mod;
    return mod;
}

void BLEModule::init() {}

void BLEModule::startScan() {
    _scanning = true;
    _resultCount = 0;
    _trackerCount = 0;
    memset(_results, 0, sizeof(_results));

    for (int i = 0; i < 3; i++) {
        strlcpy(_results[i].addr, TEST_BLE_ADDRS[i], sizeof(_results[i].addr));
        _results[i].rssi = -40;
        _results[i].companyId = (i == 0) ? TRACKER_COMPANY_APPLE : 0;
        _results[i].isTracker = (_results[i].companyId == TRACKER_COMPANY_APPLE);
        if (_results[i].isTracker) _trackerCount++;
        _resultCount++;
    }

    _scanning = false;

    auto& bus = EventBus::instance();
    bus.publish(EVENT_BLE_DEVICE_FOUND, _resultCount);
    for (int i = 0; i < _resultCount; i++) {
        if (_results[i].isTracker) {
            bus.publish(EVENT_BLE_TRACKER_ALERT, _results[i].rssi);
        }
    }
}

bool BLEModule::startAsyncScan() { return false; }
int BLEModule::pollNewDevices(BLEResult*, int) { return 0; }
void BLEModule::onAsyncDeviceFound(void*) {}
void BLEModule::onAsyncScanComplete() {}

bool BLEModule::isLikelyTracker(uint16_t companyId, int8_t, const char*) {
    return companyId == TRACKER_COMPANY_APPLE ||
           companyId == TRACKER_COMPANY_SAMSUNG ||
           companyId == TRACKER_COMPANY_TILE;
}

// ── Helper ─────────────────────────────────────────────────────────────────

static void resetPet() {
    s_fakeMillis = 0;
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_BRAVE;
    pet.state().traits[1] = TRAIT_SLEEPY;
    EventBus::instance().reset();
    PetRules::instance().init();
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(test_wifi_found_rule) {
    resetPet();
    auto& pet = PetCore::instance();
    int16_t hungerBefore = pet.state().stats.hunger;
    uint32_t xpBefore = pet.state().stats.xp;

    auto& wifi = WiFiModule::instance();
    wifi.startScan();
    s_fakeMillis = 4000;
    wifi.pollScan();
    EventBus::instance().processAll();

    ASSERT_GT(pet.state().stats.hunger, hungerBefore);
    ASSERT_GT(pet.state().stats.xp, xpBefore);
}

TEST(test_repeat_wifi_scan_has_low_repeat_xp) {
    resetPet();
    auto& pet = PetCore::instance();
    auto& wifi = WiFiModule::instance();

    wifi.startScan();
    s_fakeMillis = 4000;
    wifi.pollScan();
    EventBus::instance().processAll();
    uint32_t firstGain = pet.state().stats.xp;

    wifi.startScan();
    s_fakeMillis = 8000;
    wifi.pollScan();
    EventBus::instance().processAll();
    uint32_t secondGain = pet.state().stats.xp - firstGain;

    ASSERT_GT(firstGain, 0);
    ASSERT_TRUE(secondGain < firstGain);
}

TEST(test_ble_tracker_rule) {
    resetPet();
    auto& pet = PetCore::instance();
    uint32_t xpBefore = pet.state().stats.xp;
    auto& ble = BLEModule::instance();

    ble.startScan();
    EventBus::instance().processAll();

    ASSERT_GT(pet.state().stats.xp, xpBefore);
}

TEST(test_usb_connect_rule) {
    resetPet();
    auto& pet = PetCore::instance();
    int16_t trustBefore = pet.state().stats.trust;
    uint32_t xpBefore = pet.state().stats.xp;

    EventBus::instance().publish(EVENT_USB_CONNECTED);
    EventBus::instance().processAll();

    ASSERT_GT(pet.state().stats.trust, trustBefore);
    ASSERT_EQ(pet.state().stats.xp, xpBefore);
}

TEST(test_mission_complete_rule) {
    resetPet();
    auto& pet = PetCore::instance();
    int16_t mischiefBefore = pet.state().stats.mischief;

    EventBus::instance().publish(EVENT_MISSION_COMPLETE, 0);
    EventBus::instance().processAll();

    ASSERT_GT(pet.state().stats.mischief, mischiefBefore);
    ASSERT_EQ(pet.state().missionsCompleted, 1);
}

TEST(test_threat_codex_counts) {
    resetPet();
    auto& pet = PetCore::instance();

    auto& wifi = WiFiModule::instance();
    wifi.startScan();
    s_fakeMillis = 4000;
    wifi.pollScan();
    EventBus::instance().processAll();

    auto& ble = BLEModule::instance();
    ble.startScan();
    EventBus::instance().processAll();

    ASSERT_EQ(pet.state().threatOpenCount, 1);
    ASSERT_EQ(pet.state().threatDuplicateCount, 1);
    ASSERT_EQ(pet.state().threatTrackerCount, 1);
}

TEST(test_trait_curious_bonus) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_CURIOUS;
    pet.state().traits[1] = TRAIT_BRAVE;

    uint32_t xpBefore = pet.state().stats.xp;
    pet.addXP(100);
    ASSERT_EQ(pet.state().stats.xp, xpBefore + 125);
}

TEST(test_trait_greedy_penalty) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_PACKET_PUP;
    pet.state().traits[0] = TRAIT_GREEDY;
    pet.state().traits[1] = TRAIT_BRAVE;
    pet.state().stats.hunger = 50;

    pet.feedHunger(100);
    // Greedy gives 0.7x, so 100 * 0.7 = 70, 50+70=120 capped to 100
    ASSERT_EQ(pet.state().stats.hunger, 100);
}

TEST(test_a_patrol_actually_puts_materials_in_the_kit) {
    // test_material_yield covers the yield TABLE. This covers the WIRING: that
    // a completed scan really reaches the Inventory. Until this feature landed
    // Inventory::add() had no caller in the firmware at all, so a table that
    // computes the right answer and is never consulted is exactly the failure
    // worth a test of its own.
    resetPet();
    Inventory& inv = Inventory::instance();
    inv.clearAll();

    const uint8_t signal = inv.idFor("mat.signal");
    ASSERT_TRUE(signal != ITEM_ID_NONE);
    ASSERT_EQ((int)inv.count(signal), 0);

    // The fake scan returns four networks, all unseen on a fresh pet. Four new
    // identifiers over PATROL_NEW_PER_SIGNAL, which is 3, is exactly one frag -
    // and integer division is why it is one and not two.
    auto& wifi = WiFiModule::instance();
    wifi.startScan();
    s_fakeMillis = 4000;
    wifi.pollScan();
    EventBus::instance().processAll();

    ASSERT_EQ((int)inv.count(signal), 1);
}

// ── Quest reward payout ────────────────────────────────────────────────────
//
// This path shipped with no test behind it. PetRules::payCompletedQuests() is
// the ONLY consumer of QuestDef::rewardXP / rewardBond, and rewardXP is a
// uint16_t arriving over OTA in a content pack, so the clamp is a trust
// boundary and not a tidiness measure.
//
// resetPet() gives the pet BRAVE and SLEEPY deliberately: TRAIT_CURIOUS scales
// XP by 1.25 and TRAIT_PROTECTIVE scales bond by 1.3, and either would turn
// these exact-delta assertions into approximations.

// Installs a one-quest pack and rolls it into slot 0. One definition means one
// slot, so there is no ambiguity about which quest the payout under test
// belongs to. Void because ASSERT_TRUE expands to a bare `return`.
static const char* QUEST_PROBE_ID = "pay.one";

static void installSingleQuest(uint16_t xp, uint16_t bond) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"quests\":[{\"id\":\"pay.one\",\"text\":\"Payout probe.\","
             "\"kind\":\"care\",\"target\":2,\"xp\":%u,\"bond\":%u}]}",
             (unsigned)xp, (unsigned)bond);

    ContentStore::instance().resetToBaseline();
    ASSERT_TRUE(ContentStore::instance().applyQuestJson(json));
    ASSERT_TRUE(QuestEngine::isOfferable(*ContentStore::instance()
                                              .questDefById("pay.one")));

    QuestEngine::instance().rollDaily(0xC0FFEE);
    // If this ever rolls empty the payout assertions below would pass
    // vacuously, so the fixture proves its own setup.
    ASSERT_EQ(QuestEngine::instance().activeCount(), 1);
}

TEST(test_quest_reward_is_paid_on_completion) {
    resetPet();
    auto& pet = PetCore::instance();
    installSingleQuest(20, 4);

    const uint32_t xpBefore    = pet.state().stats.xp;
    const int16_t  trustBefore = pet.state().stats.trust;

    // target is 2, so one report must NOT complete it and must pay nothing.
    ASSERT_FALSE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 1));
    PetRules::instance().payCompletedQuests();
    ASSERT_EQ(pet.state().stats.xp, xpBefore);
    ASSERT_EQ(pet.state().stats.trust, trustBefore);

    ASSERT_TRUE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 1));
    PetRules::instance().payCompletedQuests();

    ASSERT_EQ(pet.state().stats.xp, xpBefore + 20);
    ASSERT_EQ(pet.state().stats.trust, (int16_t)(trustBefore + 4));
}

TEST(test_quest_reward_is_paid_only_once) {
    resetPet();
    auto& pet = PetCore::instance();
    installSingleQuest(20, 4);

    ASSERT_TRUE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 2));
    PetRules::instance().payCompletedQuests();

    const uint32_t xpAfterFirst    = pet.state().stats.xp;
    const int16_t  trustAfterFirst = pet.state().stats.trust;

    // The completion bitmask is an EVENT and takeJustCompleted() clears it.
    // Paying twice is the failure this guards: it is what a persisted
    // _justCompleted would cause across a reboot.
    PetRules::instance().payCompletedQuests();
    PetRules::instance().payCompletedQuests();

    ASSERT_EQ(pet.state().stats.xp, xpAfterFirst);
    ASSERT_EQ(pet.state().stats.trust, trustAfterFirst);

    // A further report on a finished quest does not re-arm it either.
    ASSERT_FALSE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 5));
    PetRules::instance().payCompletedQuests();
    ASSERT_EQ(pet.state().stats.xp, xpAfterFirst);
}

TEST(test_quest_reward_xp_is_clamped_at_50) {
    resetPet();
    auto& pet = PetCore::instance();
    // Driven from a pack, not a literal, because a pack is how a hostile or
    // simply wrong value actually reaches this code.
    installSingleQuest(60000, 0);

    const uint32_t xpBefore = pet.state().stats.xp;
    ASSERT_TRUE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 2));
    PetRules::instance().payCompletedQuests();

    ASSERT_EQ(pet.state().stats.xp, xpBefore + 50);
}

TEST(test_quest_reward_bond_is_clamped_at_10) {
    resetPet();
    auto& pet = PetCore::instance();
    installSingleQuest(0, 5000);

    const int16_t trustBefore = pet.state().stats.trust;
    ASSERT_TRUE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 2));
    PetRules::instance().payCompletedQuests();

    ASSERT_EQ(pet.state().stats.trust, (int16_t)(trustBefore + 10));
}

TEST(test_quest_paid_nothing_when_pack_drops_the_definition) {
    resetPet();
    auto& pet = PetCore::instance();
    installSingleQuest(20, 4);

    ASSERT_TRUE(QuestEngine::instance().reportProgressById(QUEST_PROBE_ID, 2));

    // A content update lands between the completion and the payout. The slot
    // is still complete; the definition behind it is gone, so there is no
    // reward value to read. The boundary case payCompletedQuests() explicitly
    // handles, and the one that would dereference null if it did not.
    ASSERT_TRUE(ContentStore::instance().applyQuestJson(
        "{\"quests\":[{\"id\":\"other.one\",\"text\":\"Different.\","
        "\"kind\":\"care\",\"target\":1,\"xp\":7,\"bond\":7}]}"));

    const uint32_t xpBefore    = pet.state().stats.xp;
    const int16_t  trustBefore = pet.state().stats.trust;

    PetRules::instance().payCompletedQuests();

    ASSERT_EQ(pet.state().stats.xp, xpBefore);
    ASSERT_EQ(pet.state().stats.trust, trustBefore);
}

int main() {
    printf("\n=== PetRules Tests ===\n");

    RUN_TEST(test_wifi_found_rule);
    RUN_TEST(test_repeat_wifi_scan_has_low_repeat_xp);
    RUN_TEST(test_ble_tracker_rule);
    RUN_TEST(test_usb_connect_rule);
    RUN_TEST(test_mission_complete_rule);
    RUN_TEST(test_threat_codex_counts);
    RUN_TEST(test_trait_curious_bonus);
    RUN_TEST(test_trait_greedy_penalty);
    RUN_TEST(test_a_patrol_actually_puts_materials_in_the_kit);
    RUN_TEST(test_quest_reward_is_paid_on_completion);
    RUN_TEST(test_quest_reward_is_paid_only_once);
    RUN_TEST(test_quest_reward_xp_is_clamped_at_50);
    RUN_TEST(test_quest_reward_bond_is_clamped_at_10);
    RUN_TEST(test_quest_paid_nothing_when_pack_drops_the_definition);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
