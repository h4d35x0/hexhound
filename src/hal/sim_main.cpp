#ifdef SIMULATOR_BUILD

// ── HexHound - Simulator Entry Point ─────────────────────────────
// Provides main() for the desktop-sim build, wiring SDL2 HAL to the
// firmware's setup()/loop() functions.

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <filesystem>
#include <cstring>

#include "hal.h"
#include "tft_compat.h"
#include "../config.h"
#include "../pet/pet_core.h"
#include "../modules/storage_module.h"
#include "../ui/ui_evolve.h"
#include "../ui/ui_home.h"
#include "../ui/ui_journal.h"
#include "../ui/ui_menu.h"
#include "../ui/ui_patrol.h"

// The firmware defines these
extern void setup();
extern void loop();

// Serial proxy instance
SimSerialProxy Serial;

namespace {

void runFrames(uint32_t durationMs) {
    uint32_t end = halTime().millis() + durationMs;
    while (!halShouldQuit() && halTime().millis() < end) {
        loop();
        SDL_Delay(16);
    }
}

bool saveShot(const std::filesystem::path& outDir, const char* filename) {
    std::filesystem::create_directories(outDir);
    auto fullPath = outDir / filename;
    return halSaveScreenshot(fullPath.string().c_str());
}

void seedPetShowcaseState() {
    auto& state = PetCore::instance().state();
    state = PetState{};
    strlcpy(state.name, "HexHound", sizeof(state.name));
    state.stage = STAGE_SENTINEL;
    state.hatched = true;
    state.stats.hunger = 92;
    state.stats.mood = 78;
    state.stats.energy = 88;
    state.stats.xp = STAGE5_XP;
    state.stats.trust = 86;
    state.stats.mischief = 38;
    state.stats.health = 97;
    state.traits[0] = TRAIT_CURIOUS;
    state.traits[1] = TRAIT_BRAVE;
    state.interactions = 42;
    state.wifiScans = 18;
    state.usbConnects = 7;
    state.seenWifiCount = 27;
    state.seenBleCount = 21;
    state.masteryRank = 4;
    state.masteryXP = 1180;
    state.threatOpenCount = 6;
    state.threatDuplicateCount = 4;
    state.threatTrackerCount = 5;
    state.missionsCompleted = 12;
    state.perkMask = PERK_SIGNAL_CARTOGRAPHER |
                     PERK_BEACON_HUNTER |
                     PERK_ANOMALY_ARCHIVIST |
                     PERK_MISSION_OPERATOR;
    state.dirty = false;
}

void seedJournalEntries() {
    auto& storage = StorageModule::instance();
    storage.appendJournal("BOOT", "SENTINEL ONLINE", "systems nominal");
    storage.appendJournal("PATROL", "WiFi:8 BLE:5", "New:3/2 +18XP");
    storage.appendJournal("ALERT", "Duplicate SSID", "CoffeeShop_Free");
    storage.appendJournal("MISSION", "WiFi Safety Tip", "completed");
    storage.appendJournal("EVOLVED", "SENTINEL", "Mastery unlocked");
}

int runDocCapture(const char* outDirArg) {
    std::filesystem::path outDir = outDirArg && outDirArg[0]
        ? std::filesystem::path(outDirArg)
        : std::filesystem::path("docs") / "screenshots";

    seedPetShowcaseState();
    seedJournalEntries();

    UIHome::instance().draw(true);
    runFrames(120);
    if (!saveShot(outDir, "01_home_sentinel.bmp")) return 1;

    UIMenu::instance().drawStats();
    if (!saveShot(outDir, "02_stats.bmp")) return 1;

    PatrolSummary patrolSummary = {};
    patrolSummary.wifiCount = 8;
    patrolSummary.newWifiCount = 3;
    patrolSummary.bleCount = 5;
    patrolSummary.newBleCount = 2;
    patrolSummary.openAPCount = 1;
    patrolSummary.dupeCount = 1;
    patrolSummary.trackerCount = 1;
    patrolSummary.foodGained = 19;
    patrolSummary.xpGained = 18;
    UIPatrol::instance().draw(patrolSummary);
    if (!saveShot(outDir, "03_patrol_results.bmp")) return 1;

    UIMenu::instance().open();
    if (!saveShot(outDir, "04_menu.bmp")) return 1;

    UIJournal::instance().loadEntries();
    UIJournal::instance().draw();
    if (!saveShot(outDir, "05_journal.bmp")) return 1;

    EvolveScene::instance().begin(STAGE_GREMLIN, STAGE_SENTINEL);
    while (!halShouldQuit() &&
           !EvolveScene::instance().isComplete() &&
           EvolveScene::instance().phase() != EVOLVE_HOLD) {
        EvolveScene::instance().update();
        SDL_Delay(16);
    }
    EvolveScene::instance().update();
    if (!saveShot(outDir, "06_evolution.bmp")) return 1;

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    halInit();
    setup();

    if (argc > 1 && strcmp(argv[1], "--capture-docs") == 0) {
        int rc = runDocCapture(argc > 2 ? argv[2] : nullptr);
        SDL_Quit();
        return rc;
    }

    while (!halShouldQuit()) {
        loop();

        // Frame pacing (~30fps). Use the HAL delay so SDL events are pumped and
        // the framebuffer is presented every frame, even when the firmware path
        // (e.g. the evolution cutscene) never polls the button itself.
        halTime().delay(16);
    }

    SDL_Quit();
    return 0;
}

#endif // SIMULATOR_BUILD
