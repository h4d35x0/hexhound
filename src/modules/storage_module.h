#pragma once
#ifdef SIMULATOR_BUILD
#include "../hal/tft_compat.h"
#else
#include <Arduino.h>
#endif
#include "../board/board_profile.h"   // HEXHOUND_PANEL_ROUND

// ── HexHound - SD Card Storage Module ───────────────────────────

#define MAX_TRUSTED_SSIDS 16
#define MAX_TRUSTED_HOSTS 8

class StorageModule {
public:
    static StorageModule& instance();

    // Initialize SD card
    bool init();

    // Pet state persistence
    bool   loadPetState();
    bool   savePetState();

    // ── Journal logging ───────────────────────────────────────────────────
    // Structured entry: writes "millis|TYPE|detail1|detail2\n"
    bool   appendJournal(const char* type, const char* detail1,
                         const char* detail2 = "");
    // Read last N lines from journal.log as newline-separated String
    String readLastJournalEntries(int count);

    // ── Config (trusted SSIDs + hosts) ────────────────────────────────────
    bool   loadConfig();
    bool   saveConfig();

    // Trusted SSIDs
    bool   isSSIDTrusted(const char* ssid);
    void   addTrustedSSID(const char* ssid);
    void   removeTrustedSSID(int index);
    int    getTrustedSSIDCount() const { return _ssidCount; }
    const char* getTrustedSSID(int idx) const;

    // Trusted Hosts
    bool   isHostTrusted(const char* host);
    void   addTrustedHost(const char* host);
    void   removeTrustedHost(int index);
    int    getTrustedHostCount() const { return _hostCount; }
    const char* getTrustedHost(int idx) const;
    bool   isDisplayFlipped() const { return _displayFlipped; }
#if HEXHOUND_PANEL_ROUND
    // A round panel is square, so "landscape" and "portrait" are the same
    // orientation and both must use rotation 0 - rotation 1 would turn the
    // whole round layout 90 degrees and put the USB-C port at 3 o'clock.
    // Rotation 0 puts the port at 6 o'clock, confirmed on this board.
    // Flipping is a 180 turn (rotation 2), which keeps the port at 12 o'clock
    // for wearing it the other way up.
    uint8_t getLandscapeRotation() const { return _displayFlipped ? 2 : 0; }
    uint8_t getPortraitRotation() const { return _displayFlipped ? 2 : 0; }
#else
    uint8_t getLandscapeRotation() const { return _displayFlipped ? 3 : 1; }
    uint8_t getPortraitRotation() const { return _displayFlipped ? 2 : 0; }
#endif
    void   toggleDisplayFlip();

    bool isReady() const { return _ready; }
    bool isSDReady() const { return _sdReady; }
    bool isFlashReady() const { return _flashReady; }

private:
    StorageModule() = default;
    bool ensureDir(const char* path);

    bool _ready = false;
    bool _sdReady = false;
    bool _flashReady = false;
    // Which of the two alternating pet-save slots the next save writes to.
    // Set from the freshest copy found at load so we never overwrite it.
    int8_t _saveSlot = 0;   // 0 = A, 1 = B

    // Trusted data (loaded from config.json)
    char _trustedSSIDs[MAX_TRUSTED_SSIDS][33];
    int  _ssidCount = 0;

    char _trustedHosts[MAX_TRUSTED_HOSTS][32];
    int  _hostCount = 0;
    bool _displayFlipped = false;
};
