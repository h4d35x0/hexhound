// ── HexHound - Storage Unit Tests ────────────────────────────────

#include "../test_stubs.h"

#include "../../src/events/event_bus.h"
#include "../../src/events/event_bus.cpp"
#include "../../src/pet/pet_core.h"
#include "../../src/pet/pet_core.cpp"

#include <ArduinoJson.h>

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        s_testsFailed++; return; \
    } \
} while(0)

// ── Journal CSV parsing (extracted from UIJournal) ─────────────────────────

struct JournalEntry {
    char timestamp[6];
    char title[20];
    char detail1[28];
    char detail2[28];
};

static void parseLine(const char* line, JournalEntry& entry) {
    memset(&entry, 0, sizeof(entry));

    const char* p1 = strchr(line, '|');
    if (p1) {
        long ms = atol(line);
        unsigned long sec = ms / 1000;
        snprintf(entry.timestamp, sizeof(entry.timestamp), "%02lu:%02lu",
                 (sec / 3600) % 24, (sec / 60) % 60);

        const char* typeStart = p1 + 1;
        const char* p2 = strchr(typeStart, '|');
        if (p2) {
            int typeLen = (int)(p2 - typeStart);
            if (typeLen >= (int)sizeof(entry.title)) typeLen = sizeof(entry.title) - 1;
            strncpy(entry.title, typeStart, typeLen);
            entry.title[typeLen] = '\0';

            const char* d1Start = p2 + 1;
            const char* p3 = strchr(d1Start, '|');
            if (p3) {
                int d1Len = (int)(p3 - d1Start);
                if (d1Len >= (int)sizeof(entry.detail1)) d1Len = sizeof(entry.detail1) - 1;
                strncpy(entry.detail1, d1Start, d1Len);
                entry.detail1[d1Len] = '\0';
                strlcpy(entry.detail2, p3 + 1, sizeof(entry.detail2));
            } else {
                strlcpy(entry.detail1, d1Start, sizeof(entry.detail1));
            }
        }
    } else if (line[0] == '[') {
        strncpy(entry.timestamp, line + 1, 5);
        entry.timestamp[5] = '\0';
        const char* textStart = strchr(line, ']');
        if (textStart) {
            textStart++;
            while (*textStart == ' ') textStart++;
            strlcpy(entry.title, textStart, sizeof(entry.title));
        }
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(test_journal_csv_parse) {
    JournalEntry entry;
    parseLine("60000|PATROL|WiFi:5 BLE:3|Open:1 Dupe:0 +15XP", entry);
    ASSERT_STREQ(entry.timestamp, "00:01");
    ASSERT_STREQ(entry.title, "PATROL");
    ASSERT_STREQ(entry.detail1, "WiFi:5 BLE:3");
    ASSERT_STREQ(entry.detail2, "Open:1 Dupe:0 +15XP");
}

TEST(test_journal_legacy_parse) {
    JournalEntry entry;
    parseLine("[12:30:45] Boot complete", entry);
    ASSERT_STREQ(entry.timestamp, "12:30");
    ASSERT_STREQ(entry.title, "Boot complete");
}

TEST(test_config_roundtrip) {
    JsonDocument doc;
    JsonArray ssids = doc["trusted_ssids"].to<JsonArray>();
    ssids.add("HomeNet");
    ssids.add("WorkNet");

    JsonArray hosts = doc["trusted_hosts"].to<JsonArray>();
    hosts.add("Laptop-1");
    doc["display_flipped"] = true;

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));

    JsonDocument doc2;
    DeserializationError err = deserializeJson(doc2, buf);
    ASSERT_TRUE(!err);

    JsonArray ssids2 = doc2["trusted_ssids"].as<JsonArray>();
    ASSERT_EQ((int)ssids2.size(), 2);
    ASSERT_STREQ(ssids2[0].as<const char*>(), "HomeNet");
    ASSERT_STREQ(ssids2[1].as<const char*>(), "WorkNet");

    JsonArray hosts2 = doc2["trusted_hosts"].as<JsonArray>();
    ASSERT_EQ((int)hosts2.size(), 1);
    ASSERT_STREQ(hosts2[0].as<const char*>(), "Laptop-1");
    ASSERT_TRUE(doc2["display_flipped"].as<bool>());
}

TEST(test_trusted_ssid) {
    char trustedSSIDs[16][33];
    int ssidCount = 0;

    auto isSSIDTrusted = [&](const char* ssid) -> bool {
        for (int i = 0; i < ssidCount; i++) {
            if (strcmp(trustedSSIDs[i], ssid) == 0) return true;
        }
        return false;
    };

    auto addTrustedSSID = [&](const char* ssid) {
        if (ssidCount >= 16 || isSSIDTrusted(ssid)) return;
        strlcpy(trustedSSIDs[ssidCount], ssid, 33);
        ssidCount++;
    };

    auto removeTrustedSSID = [&](int index) {
        if (index < 0 || index >= ssidCount) return;
        for (int i = index; i < ssidCount - 1; i++) {
            strlcpy(trustedSSIDs[i], trustedSSIDs[i + 1], 33);
        }
        ssidCount--;
    };

    ASSERT_FALSE(isSSIDTrusted("TestNet"));
    addTrustedSSID("TestNet");
    ASSERT_TRUE(isSSIDTrusted("TestNet"));
    ASSERT_EQ(ssidCount, 1);

    addTrustedSSID("TestNet");
    ASSERT_EQ(ssidCount, 1);

    addTrustedSSID("OtherNet");
    ASSERT_EQ(ssidCount, 2);

    removeTrustedSSID(0);
    ASSERT_FALSE(isSSIDTrusted("TestNet"));
    ASSERT_TRUE(isSSIDTrusted("OtherNet"));
    ASSERT_EQ(ssidCount, 1);
}

TEST(test_pet_state_roundtrip) {
    auto& pet = PetCore::instance();
    pet.state() = PetState();
    pet.state().hatched = true;
    pet.state().stage = STAGE_SENTINEL;
    pet.state().stats.xp = 1500;
    pet.state().stats.hunger = 75;
    pet.state().stats.mood = 60;
    pet.state().stats.energy = 90;
    pet.state().stats.trust = 45;
    pet.state().stats.mischief = 30;
    pet.state().stats.health = 95;
    pet.state().masteryXP = 520;
    pet.state().masteryRank = 3;
    pet.state().threatOpenCount = 6;
    pet.state().threatDuplicateCount = 2;
    pet.state().threatTrackerCount = 7;
    pet.state().missionsCompleted = 4;
    pet.state().perkMask = PERK_ANOMALY_ARCHIVIST;
    pet.state().traits[0] = TRAIT_CURIOUS;
    pet.state().traits[1] = TRAIT_CHAOTIC;
    pet.state().interactions = 150;
    pet.state().wifiScans = 42;
    pet.state().usbConnects = 10;
    pet.state().seenWifiCount = 1;
    strlcpy(pet.state().seenWifi[0], "AA:BB:CC:DD:EE:FF", sizeof(pet.state().seenWifi[0]));
    pet.state().seenBleCount = 1;
    strlcpy(pet.state().seenBle[0], "11:22:33:44:55:66", sizeof(pet.state().seenBle[0]));
    strlcpy(pet.state().name, "RoundTrip", sizeof(pet.state().name));

    String json = pet.saveToJson();

    PetState backup = pet.state();
    pet.state() = PetState();
    ASSERT_TRUE(pet.loadFrom(json.c_str()));

    ASSERT_EQ(pet.state().stage, backup.stage);
    ASSERT_EQ(pet.state().stats.xp, backup.stats.xp);
    ASSERT_EQ(pet.state().stats.hunger, backup.stats.hunger);
    ASSERT_EQ(pet.state().stats.mood, backup.stats.mood);
    ASSERT_EQ(pet.state().stats.energy, backup.stats.energy);
    ASSERT_EQ(pet.state().stats.trust, backup.stats.trust);
    ASSERT_EQ(pet.state().stats.mischief, backup.stats.mischief);
    ASSERT_EQ(pet.state().stats.health, backup.stats.health);
    ASSERT_EQ(pet.state().masteryXP, backup.masteryXP);
    ASSERT_EQ(pet.state().masteryRank, backup.masteryRank);
    ASSERT_EQ(pet.state().threatOpenCount, backup.threatOpenCount);
    ASSERT_EQ(pet.state().threatDuplicateCount, backup.threatDuplicateCount);
    ASSERT_EQ(pet.state().threatTrackerCount, backup.threatTrackerCount);
    ASSERT_EQ(pet.state().missionsCompleted, backup.missionsCompleted);
    ASSERT_EQ(pet.state().perkMask, backup.perkMask);
    ASSERT_EQ(pet.state().traits[0], backup.traits[0]);
    ASSERT_EQ(pet.state().traits[1], backup.traits[1]);
    ASSERT_EQ(pet.state().interactions, backup.interactions);
    ASSERT_EQ(pet.state().wifiScans, backup.wifiScans);
    ASSERT_EQ(pet.state().usbConnects, backup.usbConnects);
    ASSERT_EQ(pet.state().seenWifiCount, backup.seenWifiCount);
    ASSERT_EQ(pet.state().seenBleCount, backup.seenBleCount);
    ASSERT_STREQ(pet.state().seenWifi[0], backup.seenWifi[0]);
    ASSERT_STREQ(pet.state().seenBle[0], backup.seenBle[0]);
    ASSERT_TRUE(pet.state().hatched);
    ASSERT_STREQ(pet.state().name, "RoundTrip");
}

int main() {
    printf("\n=== Storage Tests ===\n");

    RUN_TEST(test_journal_csv_parse);
    RUN_TEST(test_journal_legacy_parse);
    RUN_TEST(test_config_roundtrip);
    RUN_TEST(test_trusted_ssid);
    RUN_TEST(test_pet_state_roundtrip);

    printf("\n%d passed, %d failed\n\n", s_testsPassed, s_testsFailed);
    return s_testsFailed > 0 ? 1 : 0;
}
