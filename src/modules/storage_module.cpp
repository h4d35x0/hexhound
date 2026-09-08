#include "storage_module.h"
#include "../pet/pet_core.h"
#include "../config.h"
#include "../board/board_support.h"

#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <ArduinoJson.h>
#ifndef SIMULATOR_BUILD
#include <esp_log.h>
#endif

// ── HexHound - Storage Module Implementation ────────────────────

// Dedicated SPI bus for SD card (HSPI / SPI3).
// TFT_eSPI uses the default FSPI (SPI2) - sharing the global SPI object
// would clobber TFT pin routing and kill the display.
#ifndef SIMULATOR_BUILD
static SPIClass sdSPI(HSPI);
#endif

// FLASH_PET_STATE is the LEGACY single-file save: still read so an existing
// unit keeps its pet across a firmware update, never written to again.
static constexpr const char* FLASH_PET_STATE = "/pet_state.json";
static constexpr const char* FLASH_PET_STATE_A = "/pet_state_a.json";
static constexpr const char* FLASH_PET_STATE_B = "/pet_state_b.json";
static constexpr const char* FLASH_CONFIG = "/config.json";
static constexpr const char* FLASH_JOURNAL = "/journal.log";

// Slot identifiers for the alternating pet save. Legacy single-file saves are
// readable candidates but never a write target.
static constexpr int8_t SAVE_SLOT_LEGACY = -1;
static constexpr int8_t SAVE_SLOT_A = 0;
static constexpr int8_t SAVE_SLOT_B = 1;

#ifndef SIMULATOR_BUILD
static bool beginSdCardQuietly() {
    // Optional SD boards should fall back to SPIFFS cleanly when no card is
    // inserted, without spamming low-level driver errors during the probe.
    esp_log_level_t sdDiskioLevel = esp_log_level_get("sd_diskio");
    esp_log_level_t sdmmcCmdLevel = esp_log_level_get("sdmmc_cmd");
    esp_log_level_set("sd_diskio", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    bool ok = SD.begin(PIN_SD_CS, sdSPI);
    esp_log_level_set("sd_diskio", sdDiskioLevel);
    esp_log_level_set("sdmmc_cmd", sdmmcCmdLevel);
    return ok;
}
#endif

// NOTE: this deletes before recreating, so the target file does not exist for
// a short window. That is only safe because pet saves alternate between two
// slots - the slot NOT being written still holds a complete copy. Never point
// this at the sole copy of anything that must survive a power cut.
static bool openOverwriteFile(fs::FS& fs, const char* path, File& outFile) {
    if (fs.exists(path) && !fs.remove(path)) {
        Serial.printf("[Storage] Failed to replace %s\n", path);
        return false;
    }

    outFile = fs.open(path, FILE_WRITE);
    if (!outFile) {
        Serial.printf("[Storage] Failed to open %s for write\n", path);
        return false;
    }
    return true;
}

static int findLastJsonObjectStart(const String& text) {
    for (int i = text.length() - 1; i >= 0; --i) {
        if (text[i] == '{') {
            return i;
        }
    }
    return -1;
}

struct PetSaveCandidate {
    bool valid = false;
    bool recovered = false;
    String json;
    const char* label = "";
    int8_t slot = SAVE_SLOT_LEGACY;
    uint32_t saveSeq = 0;
    uint32_t xp = 0;
    uint8_t stage = 0;
    bool hatched = false;
};

static bool readRecoverableJson(fs::FS& fs, const char* path, String& outJson, bool& recovered) {
    File f = fs.open(path, FILE_READ);
    if (!f) {
        return false;
    }

    String json = f.readString();
    f.close();

    JsonDocument doc;
    if (!deserializeJson(doc, json)) {
        outJson = json;
        recovered = false;
        return true;
    }

    int recoveredStart = findLastJsonObjectStart(json);
    if (recoveredStart > 0) {
        String candidate = json.substring(recoveredStart);
        if (!deserializeJson(doc, candidate)) {
            outJson = candidate;
            recovered = true;
            return true;
        }
    }

    return false;
}

static bool loadPetCandidate(fs::FS& fs, const char* path, const char* label,
                             int8_t slot, PetSaveCandidate& out) {
    bool recovered = false;
    String json;
    if (!readRecoverableJson(fs, path, json, recovered)) {
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        return false;
    }

    out.valid = true;
    out.recovered = recovered;
    out.json = json;
    out.label = label;
    out.slot = slot;
    out.saveSeq = doc["saveSeq"] | 0;
    out.xp = doc["xp"] | 0;
    out.stage = doc["stage"] | 0;
    out.hatched = doc["hatched"] | false;
    return true;
}

static bool isBetterPetSave(const PetSaveCandidate& candidate, const PetSaveCandidate& currentBest) {
    if (!currentBest.valid) {
        return true;
    }
    if (candidate.saveSeq != currentBest.saveSeq) {
        return candidate.saveSeq > currentBest.saveSeq;
    }
    if (candidate.stage != currentBest.stage) {
        return candidate.stage > currentBest.stage;
    }
    if (candidate.hatched != currentBest.hatched) {
        return candidate.hatched && !currentBest.hatched;
    }
    if (candidate.xp != currentBest.xp) {
        return candidate.xp > currentBest.xp;
    }
    return false;
}

StorageModule& StorageModule::instance() {
    static StorageModule mod;
    return mod;
}

bool StorageModule::init() {
    _sdReady = false;
    _flashReady = false;

#ifndef SIMULATOR_BUILD
    if (SPIFFS.begin(false)) {
        _flashReady = true;
        Serial.println("[Storage] SPIFFS ready");
    } else {
        Serial.println("[Storage] SPIFFS init FAILED, attempting format...");
        if (SPIFFS.begin(true)) {
            _flashReady = true;
            Serial.println("[Storage] SPIFFS formatted and mounted");
        } else {
            Serial.println("[Storage] SPIFFS format+mount FAILED");
        }
    }
#endif

#ifndef SIMULATOR_BUILD
    // Init SD on a SEPARATE SPI bus (HSPI/SPI3) so it cannot interfere
    // with TFT_eSPI which owns FSPI/SPI2.
    if (hexhoundHasSpiSdCard()) {
        sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    }

    if (!hexhoundHasSpiSdCard() || !beginSdCardQuietly()) {
#else
    if (false) {  // Simulator has no SD
#endif
        if (hexhoundHasSpiSdCard()) {
            Serial.println("[Storage] No SD card detected - using SPIFFS only");
        } else {
            Serial.println("[Storage] SPI SD not present on this board");
        }
    } else {
        Serial.printf("[Storage] SD card ready. Size: %lluMB\n",
                      SD.cardSize() / (1024 * 1024));
        _sdReady = true;
        ensureDir("/sd");
    }

    _ready = _sdReady || _flashReady;
    if (!_ready) {
        Serial.println("[Storage] No persistence backend available");
        return false;
    }

    return true;
}

bool StorageModule::ensureDir(const char* path) {
    if (!_sdReady) return false;
    if (!SD.exists(path)) {
        return SD.mkdir(path);
    }
    return true;
}

// ── Pet State ─────────────────────────────────────────────────────────────

bool StorageModule::loadPetState() {
    if (!_ready) return false;

    // Every readable copy is a candidate: both alternating slots plus the
    // legacy single-file save from firmware that predates the A/B scheme.
    // isBetterPetSave() picks the newest by saveSeq, so a stale legacy file
    // can never beat a live slot.
    struct Candidate {
        fs::FS* fs;
        const char* path;
        const char* label;
        int8_t slot;
    } sources[] = {
        { _sdReady ? &SD : nullptr,        PATH_PET_STATE,    "SD (legacy)",     SAVE_SLOT_LEGACY },
        { _sdReady ? &SD : nullptr,        PATH_PET_STATE_A,  "SD:A",            SAVE_SLOT_A },
        { _sdReady ? &SD : nullptr,        PATH_PET_STATE_B,  "SD:B",            SAVE_SLOT_B },
        { _flashReady ? &SPIFFS : nullptr, FLASH_PET_STATE,   "SPIFFS (legacy)", SAVE_SLOT_LEGACY },
        { _flashReady ? &SPIFFS : nullptr, FLASH_PET_STATE_A, "SPIFFS:A",        SAVE_SLOT_A },
        { _flashReady ? &SPIFFS : nullptr, FLASH_PET_STATE_B, "SPIFFS:B",        SAVE_SLOT_B }
    };

    PetSaveCandidate best;

    for (const auto& source : sources) {
        if (!source.fs) continue;

        PetSaveCandidate candidate;
        if (!loadPetCandidate(*source.fs, source.path, source.label,
                              source.slot, candidate)) {
            continue;
        }

        Serial.printf("[Storage] Found %s pet save: seq=%lu stage=%u xp=%lu%s\n",
                      source.label,
                      (unsigned long)candidate.saveSeq,
                      candidate.stage,
                      (unsigned long)candidate.xp,
                      candidate.recovered ? " (recovered)" : "");

        if (isBetterPetSave(candidate, best)) {
            best = candidate;
        }
    }

    // Write to the slot that is NOT the freshest one, so the copy we just
    // loaded stays intact until the new one is fully closed. Anything else
    // (legacy save, or no save at all) starts the cycle at slot A.
    _saveSlot = (best.valid && best.slot == SAVE_SLOT_A) ? SAVE_SLOT_B : SAVE_SLOT_A;

    if (best.valid && PetCore::instance().loadFrom(best.json.c_str())) {
        Serial.printf("[Storage] Pet state loaded from %s\n", best.label);
        return true;
    }

    Serial.println("[Storage] No readable pet save found - starting fresh");
    return false;
}

bool StorageModule::savePetState() {
#ifdef HEXHOUND_NO_PERSIST
    Serial.println("[Storage] savePetState skipped (HEXHOUND_NO_PERSIST)");
    PetCore::instance().state().dirty = false;
    return false;
#endif

    if (!_ready) return false;

    PetCore::instance().state().saveSeq++;
    String json = PetCore::instance().saveToJson();
    bool saved = false;

    // Alternate slots. The slot we are NOT touching still holds the previous
    // complete save with a lower saveSeq, so losing power at any point during
    // this write leaves that copy as the winner on the next boot. The pet can
    // lose at most the last save interval, never its whole history.
    const bool useSlotA = (_saveSlot == SAVE_SLOT_A);
    const char* sdPath = useSlotA ? PATH_PET_STATE_A : PATH_PET_STATE_B;
    const char* flashPath = useSlotA ? FLASH_PET_STATE_A : FLASH_PET_STATE_B;
    const char* slotName = useSlotA ? "A" : "B";

    if (_sdReady) {
        File f;
        if (openOverwriteFile(SD, sdPath, f)) {
            f.print(json);
            f.flush();
            f.close();
            saved = true;
            Serial.printf("[Storage] Pet state saved to SD:%s\n", slotName);
        }
    }

    if (_flashReady) {
        File f;
        if (openOverwriteFile(SPIFFS, flashPath, f)) {
            f.print(json);
            f.flush();
            f.close();
            saved = true;
            Serial.printf("[Storage] Pet state saved to SPIFFS:%s\n", slotName);
        }
    }

    if (!saved) return false;

    // Only flip once a copy is safely on disk. If both writes failed we retry
    // the same slot next time rather than walking over the good one.
    _saveSlot = useSlotA ? SAVE_SLOT_B : SAVE_SLOT_A;

    PetCore::instance().state().dirty = false;
    return true;
}

// ── Journal ───────────────────────────────────────────────────────────────

bool StorageModule::appendJournal(const char* type, const char* detail1,
                                   const char* detail2) {
    if (!_ready) return false;
    bool written = false;

    if (_sdReady) {
        File f = SD.open(PATH_JOURNAL, FILE_APPEND);
        if (f) {
            f.printf("%lu|%s|%s|%s\n", millis(), type, detail1, detail2);
            f.close();
            written = true;
        }
    }

    if (_flashReady) {
        File f = SPIFFS.open(FLASH_JOURNAL, FILE_APPEND);
        if (f) {
            f.printf("%lu|%s|%s|%s\n", millis(), type, detail1, detail2);
            f.close();
            written = true;
        }
    }

    return written;
}

String StorageModule::readLastJournalEntries(int count) {
    if (!_ready) return "No storage";

    File f;
    if (_sdReady) {
        f = SD.open(PATH_JOURNAL, FILE_READ);
    }
    if (!f && _flashReady) {
        f = SPIFFS.open(FLASH_JOURNAL, FILE_READ);
    }
    if (!f) return "";

    // Read all lines, keep last 'count' in circular buffer
    // Use dynamic Strings - ESP32 has plenty of RAM for this
    static const int MAX_BUF = 50;
    int bufSize = min(count, MAX_BUF);
    String* lines = new String[bufSize];
    int totalLines = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            lines[totalLines % bufSize] = line;
            totalLines++;
        }
    }
    f.close();

    String result;
    int start = (totalLines > bufSize) ? totalLines - bufSize : 0;
    int available = min(totalLines, bufSize);

    // Output in chronological order (oldest first)
    for (int i = 0; i < available; i++) {
        int idx = (start + i) % bufSize;
        result += lines[idx] + "\n";
    }

    delete[] lines;
    return result;
}

// ── Config ────────────────────────────────────────────────────────────────

bool StorageModule::loadConfig() {
    if (!_ready) return false;
    struct Candidate {
        fs::FS* fs;
        const char* path;
        const char* label;
    } sources[] = {
        { _sdReady ? &SD : nullptr, PATH_CONFIG, "SD" },
        { _flashReady ? &SPIFFS : nullptr, FLASH_CONFIG, "SPIFFS" }
    };

    JsonDocument doc;
    bool loaded = false;

    for (const auto& source : sources) {
        if (!source.fs) continue;

        File f = source.fs->open(source.path, FILE_READ);
        if (!f) continue;

        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            File recover = source.fs->open(source.path, FILE_READ);
            if (!recover) continue;

            String json = recover.readString();
            recover.close();

            int recoveredStart = findLastJsonObjectStart(json);
            if (recoveredStart < 0) continue;

            err = deserializeJson(doc, json.c_str() + recoveredStart);
            if (err) continue;

            Serial.printf("[Storage] Recovered config from %s save\n", source.label);
        }

        Serial.printf("[Storage] Config loaded from %s\n", source.label);
        loaded = true;
        break;
    }

    if (!loaded) return false;

    // Load trusted SSIDs
    _ssidCount = 0;
    JsonArray ssids = doc["trusted_ssids"].as<JsonArray>();
    for (JsonVariant v : ssids) {
        if (_ssidCount < MAX_TRUSTED_SSIDS) {
            strlcpy(_trustedSSIDs[_ssidCount], v.as<const char*>(), 33);
            _ssidCount++;
        }
    }

    // Load trusted hosts
    _hostCount = 0;
    JsonArray hosts = doc["trusted_hosts"].as<JsonArray>();
    for (JsonVariant v : hosts) {
        if (_hostCount < MAX_TRUSTED_HOSTS) {
            strlcpy(_trustedHosts[_hostCount], v.as<const char*>(), 32);
            _hostCount++;
        }
    }

    _displayFlipped = doc["display_flipped"] | false;

    Serial.printf("[Storage] Config ready: %d SSIDs, %d hosts, displayFlipped=%d\n",
                  _ssidCount, _hostCount, _displayFlipped ? 1 : 0);
    return true;
}

bool StorageModule::saveConfig() {
    if (!_ready) return false;

    JsonDocument doc;

    JsonArray ssids = doc["trusted_ssids"].to<JsonArray>();
    for (int i = 0; i < _ssidCount; i++) {
        ssids.add(_trustedSSIDs[i]);
    }

    JsonArray hosts = doc["trusted_hosts"].to<JsonArray>();
    for (int i = 0; i < _hostCount; i++) {
        hosts.add(_trustedHosts[i]);
    }

    doc["display_flipped"] = _displayFlipped;

    bool saved = false;

    if (_sdReady) {
        File f;
        if (openOverwriteFile(SD, PATH_CONFIG, f)) {
            serializeJsonPretty(doc, f);
            f.flush();
            f.close();
            saved = true;
        }
    }

    if (_flashReady) {
        File f;
        if (openOverwriteFile(SPIFFS, FLASH_CONFIG, f)) {
            serializeJsonPretty(doc, f);
            f.flush();
            f.close();
            saved = true;
        }
    }

    return saved;
}

// ── Trusted SSIDs ─────────────────────────────────────────────────────────

bool StorageModule::isSSIDTrusted(const char* ssid) {
    for (int i = 0; i < _ssidCount; i++) {
        if (strcmp(_trustedSSIDs[i], ssid) == 0) return true;
    }
    return false;
}

void StorageModule::addTrustedSSID(const char* ssid) {
    if (_ssidCount >= MAX_TRUSTED_SSIDS) return;
    if (isSSIDTrusted(ssid)) return;
    strlcpy(_trustedSSIDs[_ssidCount], ssid, 33);
    _ssidCount++;
    saveConfig();
}

void StorageModule::removeTrustedSSID(int index) {
    if (index < 0 || index >= _ssidCount) return;
    // Shift remaining entries down
    for (int i = index; i < _ssidCount - 1; i++) {
        strlcpy(_trustedSSIDs[i], _trustedSSIDs[i + 1], 33);
    }
    _ssidCount--;
    saveConfig();
}

const char* StorageModule::getTrustedSSID(int idx) const {
    if (idx < 0 || idx >= _ssidCount) return "";
    return _trustedSSIDs[idx];
}

// ── Trusted Hosts ─────────────────────────────────────────────────────────

bool StorageModule::isHostTrusted(const char* host) {
    for (int i = 0; i < _hostCount; i++) {
        if (strcmp(_trustedHosts[i], host) == 0) return true;
    }
    return false;
}

void StorageModule::addTrustedHost(const char* host) {
    if (_hostCount >= MAX_TRUSTED_HOSTS) return;
    if (isHostTrusted(host)) return;
    strlcpy(_trustedHosts[_hostCount], host, 32);
    _hostCount++;
    saveConfig();
}

void StorageModule::removeTrustedHost(int index) {
    if (index < 0 || index >= _hostCount) return;
    for (int i = index; i < _hostCount - 1; i++) {
        strlcpy(_trustedHosts[i], _trustedHosts[i + 1], 32);
    }
    _hostCount--;
    saveConfig();
}

const char* StorageModule::getTrustedHost(int idx) const {
    if (idx < 0 || idx >= _hostCount) return "";
    return _trustedHosts[idx];
}

void StorageModule::toggleDisplayFlip() {
    _displayFlipped = !_displayFlipped;
    saveConfig();
}
