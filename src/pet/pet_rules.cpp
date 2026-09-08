#include "pet_rules.h"
#include <string.h>
#include "pet_inventory.h"
#include "../content/material_yield.h"
#include "pet_core.h"
#include "../events/event_bus.h"
#include "../events/event_types.h"
#include "../config.h"
#include "../modules/wifi_module.h"
#include "../modules/ble_module.h"
#include "../content/quest_engine.h"
#if defined(UNIT_TEST)
// Test stubs provide Serial
#elif defined(SIMULATOR_BUILD)
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif

// ── HexHound - Rules Engine Implementation ───────────────────────

namespace {

uint32_t xpFromFood(int16_t foodGain) {
    if (foodGain <= 0) {
        return 0;
    }
    return (uint32_t)foodGain / XP_FOOD_PER_POINT;
}

uint32_t masteryFromDiscoveries(int newCaptures, int totalSeen, uint32_t perNew, uint32_t repeatBonusDivisor) {
    uint32_t mastery = (uint32_t)newCaptures * perNew;
    if (totalSeen > 0 && repeatBonusDivisor > 0) {
        mastery += (uint32_t)totalSeen / repeatBonusDivisor;
    }
    return mastery;
}

} // namespace

PetRules& PetRules::instance() {
    static PetRules rules;
    return rules;
}

void PetRules::init() {
    auto& bus = EventBus::instance();

    bus.subscribe(EVENT_WIFI_SCAN_DONE, [this](const Event& e) {
        onWifiScanDone(e.data);
    });
    bus.subscribe(EVENT_WIFI_DUPLICATE_SSID, [this](const Event& e) {
        onWifiDuplicate(e.data);
    });
    bus.subscribe(EVENT_WIFI_OPEN_NETWORK, [this](const Event& e) {
        onWifiOpenNetwork(e.data);
    });
    bus.subscribe(EVENT_BLE_DEVICE_FOUND, [this](const Event& e) {
        onBleDeviceFound(e.data);
    });
    bus.subscribe(EVENT_BLE_TRACKER_ALERT, [this](const Event& e) {
        onBleTrackerAlert(e.data);
    });
    bus.subscribe(EVENT_USB_CONNECTED, [this](const Event& e) {
        onUsbConnected();
    });
    bus.subscribe(EVENT_USB_DISCONNECTED, [this](const Event& e) {
        onUsbDisconnected();
    });
    bus.subscribe(EVENT_MISSION_COMPLETE, [this](const Event& e) {
        onMissionComplete(e.data);
    });
    bus.subscribe(EVENT_TIMER_TICK, [this](const Event& e) {
        onTimerTick(e.data);
    });
}

void PetRules::beginPatrolTally() {
    _tallyN  = 0;
    _newWifi = 0;
    _newBle  = 0;
}

void PetRules::addToTally(const MaterialGrant* g, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        if (!g[i].item || g[i].qty == 0) continue;
        bool merged = false;
        for (uint8_t j = 0; j < _tallyN; j++) {
            if (strcmp(_tally[j].item, g[i].item) == 0) {
                const uint32_t sum = (uint32_t)_tally[j].qty + g[i].qty;
                _tally[j].qty = (uint16_t)(sum > 0xFFFF ? 0xFFFF : sum);
                merged = true;
                break;
            }
        }
        if (merged || _tallyN >= MATERIAL_GRANT_MAX) continue;
        _tally[_tallyN++] = g[i];
    }
}

void PetRules::onWifiScanDone(int32_t networkCount) {
    auto& pet = PetCore::instance();
    auto& wifi = WiFiModule::instance();

    int16_t foodGain = (int16_t)(networkCount * WIFI_FOOD_PER_NETWORK) + 2;
    uint32_t xpGain  = xpFromFood(foodGain);
    uint32_t masteryGain = 0;
    int newCaptures  = 0;

    for (int i = 0; i < wifi.resultCount(); i++) {
        const WiFiResult& result = wifi.results()[i];
        const char* token = strlen(result.bssid) > 0 ? result.bssid : result.ssid;
        if (pet.markWifiSeen(token)) {
            xpGain += XP_NEW_WIFI_CAPTURE;
            masteryGain += 4;
            newCaptures++;
        }
    }

    _newWifi += (uint16_t)newCaptures;

    pet.feedHunger(foodGain);
    pet.addXP(xpGain);
    pet.addMasteryXP(masteryGain + masteryFromDiscoveries(newCaptures, networkCount, 0, 6));
    pet.changeMood(4);
    pet.recordWifiScan();
    pet.recordInteraction();

    // The everyday drip. A patrol is the action someone actually repeats, so
    // it is what keeps the kit topped up; the rarer materials come from a roam.
    MaterialGrant g[MATERIAL_GRANT_MAX];
    const uint8_t gn = matyield::patrolYield((uint16_t)networkCount,
                                             (uint16_t)newCaptures, 0, g);
    Inventory::instance().applyGrants(g, gn);
    addToTally(g, gn);

    Serial.printf("[Rules] WiFi scan: %d nets, %d new -> food+%d xp+%lu mastery+%lu\n",
                  networkCount, newCaptures, foodGain, xpGain, masteryGain);
}

void PetRules::onWifiDuplicate(int32_t index) {
    auto& pet = PetCore::instance();
    pet.feedHunger(6);
    pet.changeTrust(4);
    pet.recordThreatDuplicate();
    pet.addMasteryXP(2);

    Serial.printf("[Rules] Duplicate SSID at index %d - CRITICAL\n", index);
}

void PetRules::onWifiOpenNetwork(int32_t index) {
    auto& pet = PetCore::instance();
    pet.feedHunger(4);
    pet.changeMood(1);
    pet.recordThreatOpen();
    pet.addMasteryXP(2);

    Serial.printf("[Rules] Open network at index %d - WARNING\n", index);
}

void PetRules::onBleDeviceFound(int32_t deviceCount) {
    auto& pet = PetCore::instance();
    auto& ble = BLEModule::instance();
    int16_t foodGain = (int16_t)(deviceCount * BLE_FOOD_PER_DEVICE) + 2;
    uint32_t xpGain  = xpFromFood(foodGain);
    uint32_t masteryGain = 0;
    int newCaptures  = 0;

    for (int i = 0; i < ble.resultCount(); i++) {
        const char* addr = ble.results()[i].addr;
        if (pet.markBleSeen(addr)) {
            xpGain += XP_NEW_BLE_CAPTURE;
            masteryGain += 5;
            newCaptures++;
        }
    }

    _newBle += (uint16_t)newCaptures;

    pet.feedHunger(foodGain);
    pet.addXP(xpGain);
    pet.addMasteryXP(masteryGain + masteryFromDiscoveries(newCaptures, deviceCount, 0, 4));
    pet.changeMood(2);
    pet.recordInteraction();

    // Only the BLE column: the WiFi half of a patrol already paid out in
    // onWifiScanDone, and counting the same discovery twice would double it.
    MaterialGrant g[MATERIAL_GRANT_MAX];
    const uint8_t gn = matyield::patrolYield(0, 0, (uint16_t)newCaptures, g);
    Inventory::instance().applyGrants(g, gn);
    addToTally(g, gn);

    Serial.printf("[Rules] BLE scan: %d devs, %d new -> food+%d xp+%lu mastery+%lu\n",
                  deviceCount, newCaptures, foodGain, xpGain, masteryGain);
}

void PetRules::onBleTrackerAlert(int32_t data) {
    auto& pet = PetCore::instance();
    pet.feedHunger(6);
    pet.changeTrust(2);
    pet.recordThreatTracker();
    pet.addMasteryXP(3);

    Serial.println("[Rules] BLE tracker candidate - WARNING");
}

void PetRules::onUsbConnected() {
    auto& pet = PetCore::instance();
    pet.feedHunger(5);
    pet.changeTrust(2);
    pet.recordUsbConnect();
    pet.recordInteraction();

    Serial.println("[Rules] USB connected");
}

void PetRules::onUsbDisconnected() {
    auto& pet = PetCore::instance();
    pet.changeMood(-5);
    Serial.println("[Rules] USB disconnected");
}

// The ceiling on what one quest may pay, whatever its definition claims.
//
// Not defensive decoration. A QuestDef arrives from ContentStore, which parses
// a pack that can be delivered over OTA, and rewardXP is a uint16_t - so an
// unclamped payout lets a malformed or hostile pack hand out 65535 XP and jump
// a pet straight to Sentinel. The baseline quests pay 10 to 30, so 50 is
// generous headroom for authored content and still refuses the absurd.
//
// Clamping rather than rejecting: a pack with one over-eager number should pay
// the maximum and keep working, not brick the day's quests.
static const uint16_t QUEST_REWARD_XP_MAX   = 50;
static const uint16_t QUEST_REWARD_BOND_MAX = 10;

void PetRules::payCompletedQuests() {
    uint8_t slots[QUEST_MAX_ACTIVE];
    const uint8_t n = QuestEngine::instance().takeJustCompleted(slots, QUEST_MAX_ACTIVE);
    if (n == 0) return;

    auto& pet = PetCore::instance();
    for (uint8_t i = 0; i < n; i++) {
        const QuestDef* def = QuestEngine::instance().definitionFor(slots[i]);
        if (!def) {
            // The slot completed but the pack no longer defines it - a content
            // update between the completion and this call. Nothing to pay, and
            // nothing broken; the quest still shows as done.
            continue;
        }

        uint16_t xp   = def->rewardXP   > QUEST_REWARD_XP_MAX
                            ? QUEST_REWARD_XP_MAX   : def->rewardXP;
        uint16_t bond = def->rewardBond > QUEST_REWARD_BOND_MAX
                            ? QUEST_REWARD_BOND_MAX : def->rewardBond;

        if (xp)   pet.addXP(xp);
        if (bond) pet.changeTrust((int16_t)bond);

        Serial.printf("[Rules] Quest paid: %s +%uXP +%uBond\n",
                      def->id, (unsigned)xp, (unsigned)bond);
    }
    pet.recordInteraction();
}

void PetRules::onMissionComplete(int32_t missionId) {
    auto& pet = PetCore::instance();
    pet.feedHunger(15);
    pet.addXP(8);
    pet.addMasteryXP(12);
    pet.changeMischief(5);
    pet.changeMood(10);
    pet.recordInteraction();
    pet.recordMissionComplete();

    Serial.printf("[Rules] Mission %d complete\n", missionId);
}

void PetRules::onTimerTick(int32_t tickCount) {
    PetCore::instance().decayTick();
}
