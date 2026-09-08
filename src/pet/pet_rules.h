#pragma once
#include <stdint.h>

// ── HexHound - Rules Engine ──────────────────────────────────────

#include <stdint.h>
#include "../content/material_yield.h"

class PetRules {
public:
    static PetRules& instance();

    // Initialize - subscribes to events on the EventBus
    void init();

    // ── What this patrol put in the kit ───────────────────────────────────
    //
    // The materials are granted here, as each scan lands, because this is the
    // only place that knows how many identifiers were NEW. The result screen is
    // drawn later and elsewhere, so the run has to be tallied as it happens.
    //
    // Call beginPatrolTally() when a patrol starts; anything granted after that
    // accumulates until the next call. A patrol grants at most three distinct
    // materials, so the table is small and repeats are merged - two "SCRAP"
    // rows on a report would read as a bug rather than as two scans.
    void    beginPatrolTally();
    uint8_t patrolTallyCount() const { return _tallyN; }

    // ── How many identifiers this patrol had never met before ─────────────
    //
    // COUNTED as they happen, not derived from how much the seen-table grew.
    // The derived version (seenWifiCount - prePatrolSeenWifiCount) was correct
    // only while that table could grow forever. It is a ring now - full means
    // evict-and-replace, so the count is pinned at its maximum and the
    // difference is permanently zero. A patrol would award XP for new captures
    // while reporting "New WiFi: 0", which is how this was caught: the XP line
    // and the New lines disagreed on a real report.
    uint16_t patrolNewWifi() const { return _newWifi; }
    uint16_t patrolNewBle()  const { return _newBle; }
    const MaterialGrant* patrolTally() const { return _tally; }

    // ── Pay for every quest that finished since the last call ─────────────
    //
    // Safe and cheap to call unconditionally; it does nothing when nothing has
    // completed. Driven from the main loop rather than from each
    // reportProgress() call site, because there are four of those today and a
    // fifth one added later would silently stop paying.
    //
    // The reward VALUES are read here, in the rules layer, and clamped. That is
    // the rule already stated at the patrol call site in main.cpp: the engine
    // only tracks progress, so a content pack - which is untrusted data that can
    // arrive over OTA - cannot name its own XP number and have it believed.
    void payCompletedQuests();


private:
    PetRules() = default;

    // Event handlers
    void onWifiScanDone(int32_t networkCount);
    void onWifiDuplicate(int32_t index);
    void onWifiOpenNetwork(int32_t index);
    void onBleDeviceFound(int32_t deviceCount);
    void onBleTrackerAlert(int32_t data);
    void onUsbConnected();
    void onUsbDisconnected();
    void onMissionComplete(int32_t missionId);

    void onTimerTick(int32_t tickCount);

    // Merge a yield table into the running patrol tally.
    void addToTally(const MaterialGrant* g, uint8_t n);

    MaterialGrant _tally[MATERIAL_GRANT_MAX];
    uint8_t       _tallyN = 0;
    uint16_t      _newWifi = 0;   // reset by beginPatrolTally()
    uint16_t      _newBle  = 0;
};
