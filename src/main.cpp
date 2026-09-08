// ── HexHound - Main Firmware ─────────────────────────────────────
// Gamified Cybersecurity Companion for LilyGo T-Dongle S3
// ────────────────────────────────────────────────────────────────────────────

#include "hal/tft_compat.h"
#include "hal/backlight.h"
#include "board/board_support.h"
#include "board/capabilities.h"
#include "content/content_store.h"
#include "content/dialogue_engine.h"
#include "content/quest_engine.h"
#include "pet/pet_memory.h"
#include "pet/pet_forms.h"
#include "pet/pet_inventory.h"
#include "content/material_yield.h"
#include "games/game_registry.h"
#include "ui/ui_quests.h"
#include "ui/ui_games.h"
#include "ui/ui_inventory.h"
#include "ui/ui_report.h"
#include "ui/ui_roam.h"
#include "ui/ui_den.h"
#include "ui/ui_closet.h"
#include "pet/pet_wear.h"
#include "pet/pet_flourish.h"
#include "modules/roam_module.h"
#include "social/hexpass.h"
#include "diagnostics/step_capture.h"
#ifndef SIMULATOR_BUILD
#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>    // gpio_reset_pin() - needed for GPIO 3 strapping pin fix
#if HEXHOUND_HAS_BUTTON_B && !defined(SIMULATOR_BUILD)
#include <esp_sleep.h>      // ext0 wakeup + deep sleep for both-buttons power off
#endif
// rtc_gpio_* is needed by the panel power rail's hold RELEASE as well as by the
// sleep path, and those are gated on different macros. Include it for either,
// rather than leaving the rail's release depending on the board also happening
// to have a second button.
#if (HEXHOUND_HAS_BUTTON_B || (defined(PIN_TFT_POWER) && PIN_TFT_POWER >= 0)) && \
    !defined(SIMULATOR_BUILD)
#include <driver/rtc_io.h>  // rtc_gpio_hold_dis / rtc_gpio_deinit / pullups
#endif
#ifndef HEXHOUND_DISABLE_LED
#include <FastLED.h>
#endif
#endif
#include "config.h"

// Core systems
#include "events/event_bus.h"
#include "events/event_types.h"
#include "pet/pet_core.h"
#include "pet/pet_rules.h"

// Modules
#include "modules/wifi_module.h"
#include "modules/ble_module.h"
#include "modules/usb_module.h"
#include "modules/imu_module.h"
#include "modules/storage_module.h"
#include "modules/notif_module.h"
#include "modules/touch_module.h"
#include "modules/battery_module.h"

// UI
#include "ui/ui_home.h"
#include "ui/ui_alert.h"
#include "ui/ui_patrol.h"
#include "ui/ui_journal.h"
#include "ui/ui_patrol_hud.h"
#include "ui/ui_evolve.h"
#include "ui/ui_menu.h"
#include "ui/ui_config.h"
#include "ui/ui_missions.h"
#include "ui/animator.h"
#include "ui/ui_update.h"
#if HEXHOUND_HAS_TOUCH
// Only the two boards with a touch panel pull these in, so nothing about the
// other six changes by a byte.
#include "ui/touch_nav.h"
#include "ui/ui_utils.h"
#include "ui/ui_round.h"
#endif
#include "diagnostics/hw_validator.h"
#include "ota/ota_health.h"
#include "ota/ota_serial.h"
#include "content/content_crypto.h"

// ── Globals ────────────────────────────────────────────────────────────────

static TFT_eSPI* tft = nullptr;  // Heap-allocated in setup() to avoid static constructor crash

// Single shared LED array - used by heartbeat AND NotifModule.
// Only ONE FastLED controller is registered (in setup()) to avoid
// duplicate LED bus writers fighting over the same physical device.
#ifndef SIMULATOR_BUILD
static bool heartbeatInit = false;

// ── Backlight Keep-Alive ─────────────────────────────────────────────────
// Some S3 boards share backlight and LED GPIO banks; we defensively
// re-assert backlight after LED updates and major init steps.
bool g_backlightEnabled = false;  // also accessed by NotifModule

static inline void backlightAssert() {
    if (g_backlightEnabled) {
        hexhoundSetBacklight(true);
    }
}

#ifndef HEXHOUND_DISABLE_LED
CRGB g_leds[NUM_LEDS];       // Global - NotifModule writes to this too

static inline void showLED() {
    FastLED.show();
    backlightAssert();  // re-assert backlight after every LED update
}
#endif
#endif

// Timing
static uint32_t lastTickTime   = 0;
static uint32_t lastUIRefresh  = 0;
static uint32_t lastSaveTime   = 0;
static uint32_t animOverrideEnd = 0;

// Button state
static bool     buttonDown     = false;
static uint32_t buttonDownTime = 0;
static bool     buttonHandled  = false;
#if HEXHOUND_HAS_TOUCH
// Touch gesture state. Declared up here with the other input state because
// setup() configures it; the gesture-to-action mapping lives with
// handleButton(), which is where the rest of the input routing is.
static touchnav::GestureRecognizer g_touchNav;

// Thresholds as a fraction of the panel rather than a pixel count. The two
// boards with a touch panel are 320x172 and 480x480, so a 12 px slop that is a
// fingertip on one is a rounding error on the other.
static touchnav::GestureConfig touchNavConfig() {
    touchnav::GestureConfig cfg;
    const int minDim = SCREEN_W < SCREEN_H ? SCREEN_W : SCREEN_H;
    cfg.tapSlop  = minDim / 16;            // 10 px at 172, 30 at 480
    cfg.swipeMin = minDim / 6;             // 28 px at 172, 80 at 480
    cfg.holdMs   = BUTTON_LONG_PRESS_MS;   // one hold duration, whichever input
    return cfg;
}
#endif
#if HEXHOUND_HAS_BUTTON_B
static bool     bDown          = false;
static uint32_t bDownTime      = 0;
static bool     bHandled       = false;
static uint32_t bothDownTime   = 0;   // 0 = both are not currently held
static bool     bothHandled    = false;
#endif

// Current screen
static Screen   currentScreen  = SCREEN_HOME;

// The minigame currently holding SCREEN_GAME_PLAY, or nullptr. Owned by the
// registry, not by this pointer; endActiveGame() is the single place that
// clears it, so every exit path (finished, abandoned, alert takeover,
// evolution cutscene) frees the round exactly once.
static Minigame* g_activeGame = nullptr;
static uint32_t screenTimeout  = 0;

// Where a screen that times out lands. HOME is the historical behaviour and
// stays the default; a screen reached FROM the menu (the expedition report) goes
// back to the menu instead, because dumping someone on the home screen after
// they asked for a menu action loses their place for no reason.
//
// ONLY SCREEN_HOME AND SCREEN_MENU ARE SUPPORTED, because those are the only two
// the timeout handler knows how to paint - every other screen needs its own
// open()/draw() call that switchScreen() does not make. Anything else passed
// here lands on HOME. If a third destination is ever wanted, teach the handler
// to paint it rather than adding a case that silently does nothing.
static Screen   screenTimeoutTo = SCREEN_HOME;

// Button-down edges seen during the current expedition. Reset when one starts,
// reported in its journal entry. Diagnostic for accidental roam endings: the
// firmware can only ever observe "the pin was low", so the count is how it says
// whether that happened once or constantly.
static uint16_t roamButtonEdges = 0;

// ── Patrol State Machine ─────────────────────────────────────────────────
enum PatrolPhase {
    PATROL_IDLE,
    PATROL_HUD_WIFI,
    PATROL_HUD_BLE,
    PATROL_HUD_COMPLETE,
    PATROL_RESULTS,
    PATROL_DONE
};
static PatrolPhase patrolPhase = PATROL_IDLE;
static PatrolSummary patrolSummary;
static uint32_t patrolResultsEnd = 0;

// Pre-patrol XP snapshot for calculating reward delta
static uint32_t prePatrolXP = 0;

// Mission index for gremlin mode
static uint8_t currentMission = 0;

// Evolution cutscene state
static bool evolveActive = false;
static PetStage evolveFromStage = STAGE_EGG;
static PetStage evolveToStage   = STAGE_PACKET_PUP;

// Temp buffer for BLE polling
static BLEResult blePollBuf[4];

// Track if setup completed for heartbeat color
static bool setupComplete = false;

// Why the last boot happened, captured before anything can overwrite it and
// journalled once storage is up, so a failure that occurred with no host
// attached can still be read back afterwards.
static const char* g_resetReasonName = "?";

// ── Forward declarations ───────────────────────────────────────────────────

void handleButton();
void onShortPress();
void onLongPress();
// Back is reached by button B where there is one, and by a tap on the footer
// strip where there is a touch panel. Same routing either way: a second way in
// to the same action, not a second definition of what Back means.
#if HEXHOUND_HAS_BUTTON_B || HEXHOUND_HAS_TOUCH
void onBackPress();
#endif
#if HEXHOUND_HAS_BUTTON_B
static void softPowerOff();
#endif
static void endActiveGame();
static void endRoam(RoamEnd reason);
void startPatrol();
void updatePatrol();
void finishPatrol();
static void abortPatrol();
void startEvolveCutscene(PetStage from, PetStage to);
void updateEvolveCutscene();
void updateEvolveLED();
void showAlert(NotifLevel level, const char* msg, const char* src,
               Screen returnTo = SCREEN_HOME);
void switchScreen(Screen scr, uint32_t timeoutMs = 0,
                  Screen timeoutTo = SCREEN_HOME);
void refreshUI();
void triggerAnim(AnimID anim, uint32_t durationMs = 3000);
void updateAnimState();
void applyLandscapeRotation();
#ifdef HEXHOUND_SOFT_TFT_DEBUG_HOLD
void drawSoftTftDebugHold(uint32_t now);
#endif

// ── Setup ──────────────────────────────────────────────────────────────────

void setup() {
    // ── Panel power rail, FIRST, before anything else ─────────────────────
    //
    // This is deliberately the very first thing setup() does, ahead of even
    // Serial, and it must stay there.
    //
    // On the T-Display S3, GPIO15 gates the LDO feeding the LCD. While the board
    // is on USB that rail is already up from VBUS before any code runs, so the
    // panel is stable by the time it is initialised and everything works. On
    // BATTERY the rail only exists once this pin is driven, so asserting it a few
    // lines before tft->init() left the LDO and the panel controller still coming
    // up while the init sequence was being clocked at them. The result was a lit
    // backlight with a dead panel: the backlight is not gated by GPIO15, so it
    // comes on regardless and the device looks powered but blank.
    //
    // Doing it here gives the rail the whole of Serial and capability setup to
    // settle - hundreds of milliseconds - so no arbitrary delay has to be guessed
    // at. The short delay below is belt and braces for a cold LDO.
    //
    // The hold release matters just as much. softPowerOff() parks this pin LOW
    // and HOLDS it so deep sleep is actually dark, and an RTC pad hold SURVIVES a
    // reset - it is only cleared by a power-on reset or by releasing it. So after
    // a single sleep, every later boot found the rail held off and showed the
    // same lit-blank panel, which is why the symptom appeared after "sleep OR
    // reset". Release through both APIs and return the pad to digital GPIO before
    // driving it: gpio_hold_* and rtc_gpio_* are different registers and this pin
    // is RTC-capable, so only clearing one of them leaves it stuck.
#if defined(PIN_TFT_POWER) && (PIN_TFT_POWER >= 0) && !defined(SIMULATOR_BUILD)
    gpio_deep_sleep_hold_dis();
    rtc_gpio_hold_dis((gpio_num_t)PIN_TFT_POWER);
    gpio_hold_dis((gpio_num_t)PIN_TFT_POWER);
    rtc_gpio_deinit((gpio_num_t)PIN_TFT_POWER);
    pinMode(PIN_TFT_POWER, OUTPUT);
    digitalWrite(PIN_TFT_POWER, HIGH);
    delay(50);
#endif

    // ── Serial receive buffer, sized for an OTA frame ─────────────────────
    //
    // MUST come before Serial.begin(): the buffer is allocated there and a
    // later call is ignored.
    //
    // The default is 256 bytes, which is a quarter of one OTA data frame
    // (HEXHOUND_OTA_CHUNK_MAX 4096, plus 12 bytes of framing). The host writes
    // a whole frame at USB speed, roughly a megabyte a second, while this loop
    // is also repainting a progress screen and ticking the pet. Anything that
    // arrives between two poll() calls beyond 256 bytes is silently dropped by
    // the driver, and if the dropped run happens to include the 4-byte magic,
    // the reader never even starts a frame. The failure is total silence: the
    // device sits healthy in RECEIVING with zero bytes written, the host waits,
    // and the session eventually dies on its own stall timeout with nothing
    // anywhere naming the cause.
    //
    // That is exactly what happened on the 1.47B, and no native test could have
    // caught it: MemoryOtaTarget is handed complete frames by the test harness,
    // so the wire never has a buffer to overflow.
    //
    // 8192 is two full frames. The host is strictly request-response and only
    // ever has one frame in flight, so one would do; the second is headroom for
    // a slow repaint and for HEXHOUND_OTA_CHUNK_MAX growing later.
#if !defined(SIMULATOR_BUILD)
    Serial.setRxBufferSize(8192);
#endif
    Serial.begin(115200);

    // ── Serial must never be able to stall the boot ───────────────────────
    //
    // Symptom this cost days of: "just a backlit screen, never boots". The
    // firmware was booting the WHOLE TIME. One observed boot took 443696 ms, over
    // seven minutes, so the owner sat looking at a lit splash and entirely
    // reasonably concluded the device was dead.
    //
    // The culprit was a single `Serial.flush()` before tft->init(), now removed -
    // see the comment there. On a CDC-on-boot board flush() WAITS for the host to
    // drain the ring buffer, and a host that has the port open but is not reading
    // it never does. VBUS present is enough for the chip to believe a host is
    // there, so it triggered whenever the board was powered over USB by anything
    // that does not read the port:
    //
    //   * a laptop with no serial monitor open (i.e. the normal case)
    //   * a USB POWER BANK or a charger, which is why it also looked like a
    //     battery-only fault and sent the hunt to GPIO15 first
    //
    // Measured, same firmware, only the observer changing:
    //
    //     monitor attached from boot      3962 ms
    //     port enumerated, nobody reading 34 s / 44 s / 78 s / 443 s
    //
    // and the long numbers tracked how long nobody looked, which is the signature
    // of waiting on the host rather than of slow work.
    //
    // Serial.setTxTimeoutMs(0) was tried here as well and is deliberately NOT
    // used: with the flush gone, boot is 3962 ms at the default timeout, and the
    // default keeps the boot log COMPLETE. A zero timeout drops lines, which
    // costs exactly the diagnostics that solved this.
    //
    // NOT the same failure as the unplugged case: with no VBUS at all,
    // isCDC_Connected() is false and writes already take a non-blocking path. The
    // panel rail (GPIO15) was a separate and equally real bug.
    //
    // MEASURE THIS THE RIGHT WAY. `Total boot time` is on-device millis(), so it
    // is valid whenever you read it. Per-line stamps captured after an RTS pulse
    // are NOT: RTS does not reset an ESP32-S3 on native USB CDC, so that capture
    // is the PREVIOUS boot's log flushing out of the ring buffer. Believing those
    // stamps is what made this look like a display fault for three attempts.

    // Do not spend three seconds waiting for a host that is not coming. On
    // battery `!Serial` is true for the whole wait, so this was three seconds
    // added to EVERY unplugged boot purely so a monitor attaching by hand would
    // not miss the first lines. 400 ms still catches a monitor that is already
    // open, which is the case that matters.
    { unsigned long ws = millis(); while (!Serial && (millis()-ws < 400)) delay(20); }
    Serial.println("\n[BOOT] HexHound starting...");

    // ── Why did we boot? ──────────────────────────────────────────────────
    //
    // The chip reports the PREVIOUS boot's reset reason, which is the only way
    // to learn anything about a boot that happened with no host attached: run it
    // on battery, let it fail, then plug in and read this line.
    //
    // It exists because three firmware fixes in a row failed to cure a
    // "lit but blank panel on battery", and a brownout reset loop - a battery or
    // power bank that cannot take the current step when the panel and radios come
    // up - produces exactly that symptom and is NOT a firmware bug. Guessing
    // could not tell the two apart; this line can.
#ifndef SIMULATOR_BUILD
    {
        const esp_reset_reason_t rr = esp_reset_reason();
        const char* name = "?";
        switch (rr) {
        case ESP_RST_POWERON:  name = "POWERON";               break;
        case ESP_RST_SW:       name = "SW (ESP.restart)";      break;
        case ESP_RST_PANIC:    name = "PANIC (crash)";         break;
        case ESP_RST_INT_WDT:  name = "INT_WDT";               break;
        case ESP_RST_TASK_WDT: name = "TASK_WDT";              break;
        case ESP_RST_WDT:      name = "WDT (other)";           break;
        case ESP_RST_BROWNOUT: name = "BROWNOUT (power!)";     break;
        case ESP_RST_DEEPSLEEP:name = "DEEPSLEEP wake";        break;
        case ESP_RST_EXT:      name = "EXT (reset pin)";       break;
        case ESP_RST_SDIO:     name = "SDIO";                  break;
        default:               name = "UNKNOWN";               break;
        }
        Serial.printf("[BOOT] Reset reason: %s (%d)\n", name, (int)rr);
        g_resetReasonName = name;
    }
#endif
    // Print the resolved capability set once at boot. A wrong-board flash is
    // the most common support case and it renders as a dead panel, so having
    // the board's own idea of what it is in the log makes that a one-line
    // diagnosis instead of a guess.
    Serial.printf("[BOOT] Board: %s (tier=%s caps=0x%02X)\n",
                  Caps::boardName(), Caps::tierName(), (unsigned)Caps::mask());

    // ─── OTA trial window ───────────────────────────────────────────────
    //
    // First thing after the banner, and deliberately before any of the long
    // init below. Two reasons, both load-bearing:
    //
    //   1. The trial clock starts at true boot, so the health deadline measures
    //      how long this image took to become useful, not how long it took
    //      after some arbitrary later point.
    //   2. The trial watchdog is armed before anything that could hang. A
    //      pending image that wedges inside setup() never reaches loop(), so
    //      the deadline check could never fire; the watchdog is the only thing
    //      that turns that hang into the reset the bootloader needs to see in
    //      order to roll back.
    //
    // On a device that has only ever been USB-flashed this is a no-op: there is
    // no pending image, so nothing is armed and nothing is timed.
    OtaHealth::begin();
    if (OtaHealth::bootedAfterRollback()) {
        // Otherwise a failed update looks to the owner exactly like nothing
        // happened, which is the most confusing possible outcome.
        Serial.println("[OTA] A previous update was rejected and rolled back. "
                       "This is the firmware that was running before it.");
    }

#ifdef MINIMAL_BOOT
    // ══════════════════════════════════════════════════════════════════
    // MINIMAL_BOOT: Raw SPI display test - bypasses TFT_eSPI entirely.
    // Uses the EXACT same init sequence as the working tft-test env.
    // If this works on POR -> TFT_eSPI's SPI init is the problem.
    // ══════════════════════════════════════════════════════════════════
#if !defined(SIMULATOR_BUILD) && defined(HEXHOUND_BOARD_TDONGLE_S3)
    Serial.println("[RAW] gpio_reset_pin on all TFT pins...");
    hexhoundResetDisplayPins();

    // Configure control pins (matches tft-test exactly)
    pinMode(PIN_TFT_CS, OUTPUT);
    pinMode(PIN_TFT_DC, OUTPUT);
    pinMode(PIN_TFT_RST, OUTPUT);
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    digitalWrite(PIN_TFT_BL, LOW);

    // Hardware reset with generous timing (matches tft-test)
    Serial.println("[RAW] Hardware reset...");
    digitalWrite(PIN_TFT_RST, HIGH); delay(50);
    digitalWrite(PIN_TFT_RST, LOW);  delay(50);
    digitalWrite(PIN_TFT_RST, HIGH); delay(150);
    Serial.println("[RAW] Hardware reset done");

    // Explicit SPI bus init (matches tft-test: SPI.begin(SCLK, MISO, MOSI, SS))
    Serial.println("[RAW] SPI.begin(5, -1, 3, -1)...");
    SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, -1);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    Serial.println("[RAW] SPI ready at 4MHz");

    // ─── Raw ST7735 init (exact copy from working tft-test) ──────────
    // Helper lambdas for sending commands/data via HW SPI
    auto rawCmd = [](uint8_t cmd) {
        digitalWrite(PIN_TFT_DC, LOW);
        digitalWrite(PIN_TFT_CS, LOW);
        SPI.transfer(cmd);
        digitalWrite(PIN_TFT_CS, HIGH);
    };
    auto rawData = [](uint8_t data) {
        digitalWrite(PIN_TFT_DC, HIGH);
        digitalWrite(PIN_TFT_CS, LOW);
        SPI.transfer(data);
        digitalWrite(PIN_TFT_CS, HIGH);
    };

    Serial.println("[RAW] ST7735 init...");
    rawCmd(0x01);  // SWRESET
    delay(150);
    Serial.println("[RAW]   SWRESET + 150ms");

    rawCmd(0x11);  // SLPOUT
    delay(500);    // extra long for display readiness
    Serial.println("[RAW]   SLPOUT + 500ms");

    rawCmd(0x3A); rawData(0x05);  // COLMOD = 16-bit color
    Serial.println("[RAW]   COLMOD=0x05");

    rawCmd(0x36); rawData(0xC8);  // MADCTL
    Serial.println("[RAW]   MADCTL=0xC8");

    rawCmd(0x21);  // INVON
    rawCmd(0x13);  // NORON
    delay(10);
    rawCmd(0x29);  // DISPON
    delay(100);
    Serial.println("[RAW]   DISPON - display controller ready");

    // Set window to full screen (matches tft-test column/row offsets)
    // CASET: col 26..105 (80 pixels)
    rawCmd(0x2A);
    rawData(0x00); rawData(26); rawData(0x00); rawData(105);
    // RASET: row 1..160
    rawCmd(0x2B);
    rawData(0x00); rawData(1); rawData(0x00); rawData(160);

    // Fill entire screen with CYAN (0x07FF) - bright, unmissable
    rawCmd(0x2C);  // RAMWR
    digitalWrite(PIN_TFT_DC, HIGH);
    digitalWrite(PIN_TFT_CS, LOW);
    for (uint32_t i = 0; i < 80UL * 160; i++) {
        SPI.transfer(0x07);
        SPI.transfer(0xFF);
    }
    digitalWrite(PIN_TFT_CS, HIGH);
    Serial.println("[RAW] Screen filled CYAN");

    // Backlight ON
    digitalWrite(PIN_TFT_BL, HIGH);
    Serial.println("[RAW] Backlight ON - CYAN screen should be visible NOW");

    Serial.println();
    Serial.println("========================================");
    Serial.println("  [MINIMAL_BOOT] Raw SPI display test");
    Serial.println("  Screen should show solid CYAN");
    Serial.println("  If dark -> hardware or power issue");
    Serial.println("========================================");
    Serial.println();
#elif !defined(SIMULATOR_BUILD)
    Serial.println("[MINIMAL_BOOT] Only implemented for T-Dongle ST7735 bring-up.");
#endif // SIMULATOR_BUILD

#else
    // ══════════════════════════════════════════════════════════════════
    // NORMAL BOOT: Full firmware with TFT_eSPI
    // ══════════════════════════════════════════════════════════════════

    // ─── BACKLIGHT OFF IMMEDIATELY - prevent floating pin glow ───────
#ifndef SIMULATOR_BUILD
    hexhoundInitBoardPower();
    hexhoundInitBacklightHardware();
    hexhoundSetBacklight(false);
#endif

    // ─── Button ──────────────────────────────────────────────────────
    pinMode(PIN_BUTTON, INPUT_PULLUP);
#if HEXHOUND_HAS_BUTTON_B
    // Button B (Back). Only the T-Display S3 has a second button today; every
    // other board is single-button and compiles none of this.
    pinMode(PIN_BUTTON_2, INPUT_PULLUP);
#endif

    // ─── Panel power rail ───────────────────────────────────────────
    //
    // MUST be asserted before the panel is touched, and it is the difference
    // between a device that works and one that looks dead.
    //
    // On the T-Display S3, GPIO15 gates the LDO feeding the LCD. On USB the
    // panel gets its rail from VBUS regardless, so the board works perfectly
    // while it is plugged into a computer and shows a LIT BUT BLANK screen the
    // moment it runs on battery. PIN_TFT_POWER was defined when the board was
    // added and then never driven anywhere in the tree, so every battery-powered
    // T-Display S3 has always been broken; nobody noticed because the board had
    // only ever been run tethered.
    //
    // Held through deep sleep as well, see softPowerOff(): the backlight pin
    // itself (GPIO38) is NOT RTC-capable and cannot be held, so cutting this
    // rail is the only way to make sleep actually dark rather than a lit blank
    // panel with a sleeping CPU.
#if defined(PIN_TFT_POWER) && (PIN_TFT_POWER >= 0) && !defined(SIMULATOR_BUILD)
    // Already asserted at the very top of setup() - see the long comment there
    // for why it cannot wait until here. Reported now that Serial exists, and
    // read back rather than assumed, so a rail that failed to come up is visible
    // in the log instead of being inferred from a blank screen.
    Serial.printf("[BOOT] Panel power ON (GPIO%d reads %d)\n",
                  (int)PIN_TFT_POWER, (int)digitalRead(PIN_TFT_POWER));
#endif

    // ─── TFT display ────────────────────────────────────────────────
#ifndef SIMULATOR_BUILD
    Serial.println("[BOOT] Resetting ALL TFT GPIO pins...");
    hexhoundResetDisplayPins();
    Serial.println("[BOOT] TFT pins reset OK");
#endif

    Serial.println("[BOOT] Allocating TFT_eSPI...");
    tft = new TFT_eSPI();

    Serial.println("[BOOT] tft->init()...");
    // NO Serial.flush() here. It was added so this line was guaranteed out
    // before a panel init that might hang, but on a CDC-on-boot board flush()
    // WAITS for the host to drain the ring buffer, and a host that has the port
    // open but is not reading it never does. That turned a ~4 s boot into one
    // that finished only when a serial monitor was finally opened - measured at
    // 34 s, 44 s, 78 s and once 443 s, always tracking how long nobody looked.
    // The device sat on a lit splash the whole time and read as bricked.
    //
    // The line is still emitted; it just is not waited on. Losing it in the rare
    // case where init() really does hang is a far better trade than a device that
    // appears dead whenever it is plugged into a charger or an unmonitored port.
    tft->init();
#ifndef SIMULATOR_BUILD
    hexhoundApplyBoardDisplayInit(tft);
#endif
    Serial.println("[BOOT] TFT init OK");

    tft->setRotation(StorageModule::instance().getLandscapeRotation());

    // Draw boot splash BEFORE turning backlight on (clean visual).
    // Artwork above a centered title. The pet's own stage is not known yet
    // (StorageModule::loadPetState() runs further down), so the splash always
    // shows the Sentinel - the final form, and the closest thing HexHound has
    // to a brand mark.
    tft->fillScreen(TFT_BLACK);
    {
        const char* title = "HexHound";
        const char* boot  = "Booting...";
        const bool  big   = (SCREEN_H > 100);
        // Title drops from size 6 to 4 on the big panel to make room for the
        // art: 64 + 8 + 32 + 8 + 16 = 128px of a 172px panel.
        int titleSize = big ? 4 : 2;   // GLCD glyph = 6*size x 8*size
        int bootSize  = big ? 2 : 1;
        int gap       = big ? 8 : 4;
        int artSize   = HD_HOME_PX;
        int titleW = (int)strlen(title) * 6 * titleSize;
        int titleH = 8 * titleSize;
        int bootW  = (int)strlen(boot)  * 6 * bootSize;
        int bootH  = 8 * bootSize;
        int blockH = artSize + gap + titleH + gap + bootH;
        int artY   = (SCREEN_H - blockH) / 2;
        int titleY = artY + artSize + gap;
        int bootY  = titleY + titleH + gap;

        drawSprite(*tft, getStageHomeHD(STAGE_SENTINEL),
                   (SCREEN_W - artSize) / 2, artY, artSize, artSize);

        tft->setTextColor(TFT_CYAN, TFT_BLACK);
        tft->setTextSize(titleSize);
        tft->setCursor((SCREEN_W - titleW) / 2, titleY);
        tft->print(title);

        tft->setTextColor(TFT_WHITE, TFT_BLACK);
        tft->setTextSize(bootSize);
        tft->setCursor((SCREEN_W - bootW) / 2, bootY);
        tft->print(boot);

        tft->setTextSize(1);  // reset for later drawing
    }
    delay(50);

    // Manual backlight assertion (redundant - TFT_eSPI already set it via TFT_BL)
#ifndef SIMULATOR_BUILD
    hexhoundInitBacklightHardware();
    hexhoundSetBacklight(true);
    g_backlightEnabled = true;
#endif
    Serial.println("[BOOT] Backlight ON - boot splash visible");

    // Reported here rather than straight after tft->init(), because init()
    // returning success is not the same claim as pixels being lit. This build
    // has already shipped firmware that ran perfectly with a dead panel: the
    // HID image sat on the TFT_eSPI backend that black-screens the T-Dongle S3,
    // and with no serial console in HID mode nobody could see it. The splash is
    // drawn and the backlight is on by this line, which is the closest thing to
    // "the owner can see something" that setup() can honestly assert.
    OtaHealth::report(OTA_HEALTH_DISPLAY);

    // ─── LED - visual debug beacon ───────────────────────────────────
    // FastLED initialized AFTER TFT to prevent GPIO_OUT1 register
    // interference between pins 38 (BL), 39 (LED CLK), 40 (LED DATA).
#if !defined(SIMULATOR_BUILD) && !defined(HEXHOUND_DISABLE_LED)
    hexhoundInitSharedLedController(g_leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS);
    g_leds[0] = CRGB::Green;  // GREEN = TFT + backlight ready
    showLED();
    heartbeatInit = true;
    Serial.println("[BOOT] FastLED initialized, GREEN beacon on");
#elif !defined(SIMULATOR_BUILD)
    Serial.println("[BOOT] LED disabled for safe S3 bring-up");
#endif

    // ─── STEP 6: NotifModule ────────────────────────────────────────
    Serial.println("[BOOT] Init NotifModule...");
    NotifModule::instance().init();
    Serial.println("[BOOT] NotifModule OK");

    // ─── STEP 7: StorageModule ──────────────────────────────────────
    Serial.println("[BOOT] Init StorageModule...");
    StorageModule::instance().init();
    Serial.println("[BOOT] StorageModule OK");
    OtaHealth::report(OTA_HEALTH_STORAGE);

    // ─── STEP 8: Load saved state ───────────────────────────────────
    Serial.println("[BOOT] Loading pet state...");
    StorageModule::instance().loadPetState();
    Serial.println("[BOOT] Pet state loaded");

    // The pet surviving the update is the whole point of the save-safety work,
    // so it is a required milestone rather than an observation. A legitimately
    // absent save (a brand new device) still counts: loadPetState() succeeding
    // is the claim, not the pet being non-empty.
    OtaHealth::report(OTA_HEALTH_PET);

#ifdef HEXHOUND_FORCE_RUNTIME_STAGE
    {
        auto& forcedState = PetCore::instance().state();
        forcedState.stage = static_cast<PetStage>(HEXHOUND_FORCE_RUNTIME_STAGE);
        if (forcedState.stage >= STAGE_PACKET_PUP) {
            forcedState.hatched = true;
        }
        forcedState.dirty = false;
        Serial.printf("[BOOT] Forced runtime stage: %s (%d)\n",
                      STAGE_NAMES[forcedState.stage - 1], forcedState.stage);
    }
#endif

    Serial.println("[BOOT] Loading config...");
    StorageModule::instance().loadConfig();
    Serial.println("[BOOT] Config loaded");
    applyLandscapeRotation();

    Serial.println("[BOOT] Init TouchModule...");
    TouchModule::instance().init();
    TouchModule::instance().setDisplayRotation(
        StorageModule::instance().getLandscapeRotation(),
        tft->width(),
        tft->height());
#if HEXHOUND_HAS_TOUCH
    // Gesture thresholds are derived from the panel, so they are set once here
    // rather than recomputed on every poll.
    g_touchNav.configure(touchNavConfig());
#endif
    Serial.println("[BOOT] TouchModule OK");

    Serial.println("[BOOT] Init BatteryModule...");
    BatteryModule::instance().init();

#if HEXHOUND_HAS_IMU
    Serial.println("[BOOT] Init IMU...");
    IMUModule::instance().init();
#endif
    Serial.println("[BOOT] BatteryModule OK");

    // ─── STEP 9: PetCore ────────────────────────────────────────────
    Serial.println("[BOOT] Init PetCore...");
    PetCore::instance().init();
    // A power-on rolls the quest day forward. Must run after loadPetState()
    // so it advances the SAVED day rather than restarting from zero, which
    // would hand out the same first-day quests forever.
    PetCore::instance().beginSession();
    Serial.println("[BOOT] PetCore OK");

    // ─── Content engines ────────────────────────────────────────────
    // ContentStore reads SPIFFS, so it must come after StorageModule::init()
    // mounted it. Running earlier finds no pack and silently uses the
    // baseline, which is correct but throws away any shipped content update.
    Serial.println("[BOOT] Init content engines...");
    ContentStore::instance().begin();
    // The recipe table. Without this Inventory::_recipeCount stays 0 for the
    // whole run, so the Kit lists no recipes at all and craft() answers
    // CRAFT_NO_SUCH_RECIPE however full the kit is - which is exactly what was
    // happening: begin() is the only thing that reads BASELINE_RECIPES, it was
    // never called, and its [Items] line was absent from every boot log ever
    // captured. Same shape as the materials that had no source: a complete
    // implementation with tests, never wired to anything.
    //
    // After ContentStore and after SPIFFS, because it also looks for a shipped
    // recipe pack and falls back to the baseline when there is none.
    Inventory::instance().begin();
    DialogueEngine::instance().begin((uint32_t)millis());
    DialogueEngine::instance().setMemoryAccessor(PetMemory::accessor, nullptr);
    // Slots this board cannot compute, so a line asking for one is never
    // rendered with filler. See PetMemory::unavailableSlotMask().
    DialogueEngine::instance().setUnavailableSlots(PetMemory::unavailableSlotMask());
    QuestEngine::instance().begin(PetCore::instance().questDaySeed());
    // Powering on and watching it boot is itself a check-in.
    PetMemory::recordCheckIn();
    // Resolve the behavioural form from the counters the save just restored,
    // so the first frame already shows the right title and accent rather than
    // settling into it a tick later.
    PetForms::refresh(PetCore::instance().state());
    {
        // formName() returns "" for FORM_UNSET, which is right for the UI (a
        // pet with no form yet should show its stage name, not the word
        // "unset") but reads as a broken log line here.
        const char* fn = PetForms::formName(PetCore::instance().state().form);
        Serial.printf("[BOOT] Form: %s\n", (fn && *fn) ? fn : "none yet");
    }
    Serial.printf("[BOOT] Content OK - %u dialogue, %u quest defs, %u quests today\n",
                  (unsigned)ContentStore::instance().dialogueCount(),
                  (unsigned)ContentStore::instance().questDefCount(),
                  (unsigned)QuestEngine::instance().activeCount());

    // ─── STEP 10: PetRules ──────────────────────────────────────────
    Serial.println("[BOOT] Init PetRules...");
    PetRules::instance().init();
    Serial.println("[BOOT] PetRules OK");

    // ─── STEP 11: WiFiModule ────────────────────────────────────────
    Serial.println("[BOOT] Init WiFiModule...");
    WiFiModule::instance().init();
    Serial.println("[BOOT] WiFiModule OK");

    // ─── STEP 12: BLEModule ─────────────────────────────────────────
    Serial.println("[BOOT] Init BLEModule...");
    BLEModule::instance().init();
    Serial.println("[BOOT] BLEModule OK");

    // ─── STEP 13: USBModule ─────────────────────────────────────────
    Serial.println("[BOOT] Init USBModule...");
    USBModule::instance().init();
    Serial.println("[BOOT] USBModule OK");

    // Test hook: HEXHOUND_SIM_STAGE=1..5 forces the pet's stage.
    //
    // APPLIED HERE, before STEP 14, and that position is the whole point. It
    // used to sit with the other sim hooks at the end of setup(), after
    // UIHome::init() had already recorded the stage it booted with. The home
    // screen resets its animator to ANIM_IDLE on any stage it has not seen, so
    // the first draw after a late override threw away whatever animation a
    // later hook had selected - which is exactly how HEXHOUND_SIM_HUNGER below
    // came out pixel-identical to a fed pet and looked like a no-op.
    //
    // Re-initialising UIHome afterwards "fixed" that and broke the header
    // instead: syncing _lastStage suppressed the forceRedraw that repaints the
    // stage name, so the art was a Packet Pup under a title that still said
    // Egg. Setting the stage before anything initialises from it is the only
    // version with no second artifact.
#ifdef SIMULATOR_BUILD
    if (const char* stg = getenv("HEXHOUND_SIM_STAGE")) {
        const int v = atoi(stg);
        if (v >= STAGE_EGG && v <= STAGE_SENTINEL) {
            PetCore::instance().state().stage = (PetStage)v;
        }
    }
#endif

#ifdef HEXHOUND_DEMO_STAGE
    // The hardware equivalent of the hook above, for FILMING a stage-gated
    // screen. getenv() has no meaning on an ESP32, so the sim hook cannot serve.
    //
    // Only [env:lilygo-t-rgb-hid-demo] defines this, and that env also defines
    // HEXHOUND_NO_PERSIST, so savePetState() returns early and clears the dirty
    // flag: the forced stage lives in RAM for one boot and the stored pet is
    // never written. Pairing the two is not optional. Without NO_PERSIST the
    // mission-success path saves, and a demo would permanently promote somebody
    // real pet.
    //
    // It exists because MISSIONS gates on STAGE_GREMLIN and a bench T-RGB sat at
    // Packet Pup with 241 XP against a 900 XP threshold. 659 XP of patrols is
    // not a filming schedule.
    //
    // Never lowers a stage, so a board that has legitimately gone further is
    // filmed as it really is.
    if (PetCore::instance().state().stage < (PetStage)HEXHOUND_DEMO_STAGE) {
        PetCore::instance().state().stage = (PetStage)HEXHOUND_DEMO_STAGE;
        Serial.printf("[DEMO] stage forced to %d in RAM (NO_PERSIST)\n",
                      (int)HEXHOUND_DEMO_STAGE);
    }
#endif

    // ─── STEP 14: UI screens ───────────────────────────────────────
    Serial.println("[BOOT] Init UIHome...");
    UIHome::instance().init(tft);
    Serial.println("[BOOT] UIHome OK");

    Serial.println("[BOOT] Init UIAlert...");
    UIAlert::instance().init(tft);
    Serial.println("[BOOT] UIAlert OK");

    Serial.println("[BOOT] Init UIPatrol...");
    UIPatrol::instance().init(tft);
    Serial.println("[BOOT] UIPatrol OK");

    Serial.println("[BOOT] Init UIJournal...");
    UIJournal::instance().init(tft);
    Serial.println("[BOOT] UIJournal OK");

    Serial.println("[BOOT] Init UIPatrolHUD...");
    UIPatrolHUD::instance().init(tft);
    Serial.println("[BOOT] UIPatrolHUD OK");

    Serial.println("[BOOT] Init EvolveScene...");
    EvolveScene::instance().init(tft);
    Serial.println("[BOOT] EvolveScene OK");

    Serial.println("[BOOT] Init UIMenu...");
    UIMenu::instance().init(tft);
    Serial.println("[BOOT] UIMenu OK");

    Serial.println("[BOOT] Init UIConfig...");
    UIConfig::instance().init(tft);
    Serial.println("[BOOT] UIConfig OK");

    Serial.println("[BOOT] Init UIMissions...");
    UIMissions::instance().init(tft);
    Serial.println("[BOOT] UIMissions OK");

    Serial.println("[BOOT] Init UIQuests...");
    UIQuests::instance().init(tft);
    Serial.println("[BOOT] UIQuests OK");

    Serial.println("[BOOT] Init UIGames...");
    UIGames::instance().init(tft);
    Serial.println("[BOOT] UIGames OK");

    Serial.println("[BOOT] Init UIInventory...");
    UIInventory::instance().init(tft);
    Serial.println("[BOOT] UIInventory OK");

    Serial.println("[BOOT] Init UIReport...");
    UIReport::instance().init(tft);
    Serial.println("[BOOT] UIReport OK");

    Serial.println("[BOOT] Init UIDen...");
    UIDen::instance().init(tft);
    UICloset::instance().init(tft);
    Serial.println("[BOOT] UIDen OK");

    // ─── HexPass ────────────────────────────────────────────────────────
    // After the save has loaded: begin() reads the secret out of PetState, so
    // running it earlier would mint a second identity and orphan the first.
    //
    // Nothing is transmitted by this. HexPass is opt-in and defaults to off,
    // there is no radio code in the build at all yet, and the visibility tier
    // defaults to FRIENDS. This call generates an identity and nothing more.
    Serial.println("[BOOT] Init HexPass...");
    const bool hexpassCryptoOk = HexPassCrypto::selfTest();
    if (!hexpassCryptoOk) {
        // The crypto backend failed its known-answer test, so every identifier
        // this build would derive is untrustworthy. Refuse rather than
        // broadcast something weak later.
        Serial.println("[BOOT] HexPass DISABLED - crypto self-test FAILED");
        HexPass::instance().setEnabled(false);
    } else {
        HexPass::instance().begin();
        Serial.printf("[BOOT] HexPass OK (enabled=%s, visibility=%s)\n",
                      HexPass::instance().enabled() ? "yes" : "no",
                      HexPass::instance().visibility() == HEXPASS_VIS_DISCOVERABLE
                          ? "discoverable" : "friends");
    }

    // ─── OTA health: crypto milestone ───────────────────────────────────
    //
    // Reported from the known-answer tests this firmware actually depends on,
    // not from the fact that the build linked. HexPassCrypto backs identity
    // derivation; ContentCrypto backs Ed25519 and SHA-512 for both content
    // packs and the OTA envelope itself, so a trial image that cannot pass it
    // is an image that could never verify its own replacement. Confirming that
    // one as permanent would strand the device on firmware that can no longer
    // be updated, which on a giveaway device is the same as bricked.
    //
    // ContentCrypto::selfTest() runs a real Ed25519 verify and costs real
    // milliseconds on a 240 MHz Xtensa, so it is paid only while an image is on
    // trial rather than on every ordinary boot.
    if (OtaHealth::isPending()) {
        if (hexpassCryptoOk && ContentCrypto::selfTest()) {
            OtaHealth::report(OTA_HEALTH_CRYPTO);
        } else {
            Serial.println("[OTA] Crypto self-test FAILED on a trial image. "
                           "It will not be confirmed.");
        }
    }

    // ─── OTA serial transport ───────────────────────────────────────────
    //
    // Attaches the session to the real flash target and resets the wire. It
    // does NOT arm anything and cannot: the only route to arm() is the update
    // screen, in response to a person holding the button.
    Serial.println("[BOOT] Init OTA transport...");
    OtaService::begin();
    UIUpdate::instance().init(tft);

    Serial.println("[BOOT] Init Roam...");
    RoamModule::instance().init();
    UIRoam::instance().init(tft);
    Serial.printf("[BOOT] Roam OK (steps %s)\n",
                  RoamModule::stepsMeasured() ? "measured" : "unavailable on this board");

    // ─── STEP 15: Hardware diagnostics (hold button during boot) ────
    Serial.println("[BOOT] Checking HW diagnostic request...");
    if (HWValidator::isBootTestRequested()) {
        Serial.println("[BOOT] Boot button held - entering HW diagnostics");
        HWValidator::instance().init(tft);
        HWValidator::instance().runAll();
        tft->fillScreen(TFT_BLACK);
    }
    Serial.println("[BOOT] HW diagnostics check done");

    // ─── STEP 16: Event subscriptions ───────────────────────────────
    Serial.println("[BOOT] Registering event subscriptions...");
    auto& bus = EventBus::instance();

    bus.subscribe(EVENT_WIFI_DUPLICATE_SSID, [](const Event& e) {
        NotifModule::instance().notify(NOTIF_CRIT, "Duplicate SSID!", "WiFi");
        if (currentScreen == SCREEN_HOME) {
            showAlert(NOTIF_CRIT, "Duplicate SSID detected", "WiFi Scan");
            triggerAnim(ANIM_ALERT, 4000);
        }
    });

    bus.subscribe(EVENT_WIFI_OPEN_NETWORK, [](const Event& e) {
        NotifModule::instance().notify(NOTIF_WARN, "Open network found", "WiFi");
    });

    bus.subscribe(EVENT_BLE_TRACKER_ALERT, [](const Event& e) {
        NotifModule::instance().notify(NOTIF_WARN, "Tracker candidate!", "BLE");
        if (currentScreen == SCREEN_HOME) {
            showAlert(NOTIF_WARN, "BLE tracker candidate nearby", "BLE Scan");
            triggerAnim(ANIM_ALERT, 4000);
        }
    });

    bus.subscribe(EVENT_PET_HATCHED, [](const Event& e) {
        NotifModule::instance().notify(NOTIF_INFO, "Pet hatched!", "System");
    });

    bus.subscribe(EVENT_STAGE_EVOLVED, [](const Event& e) {
        PetStage from = (PetStage)((e.data >> 8) & 0xFF);
        PetStage to   = (PetStage)(e.data & 0xFF);
        startEvolveCutscene(from, to);
    });

    bus.subscribe(EVENT_WIFI_SCAN_DONE, [](const Event& e) {
        if (currentScreen == SCREEN_HOME) {
            triggerAnim(ANIM_HAPPY, 2000);
        }
    });

    bus.subscribe(EVENT_BLE_DEVICE_FOUND, [](const Event& e) {
        if (currentScreen == SCREEN_HOME) {
            triggerAnim(ANIM_HAPPY, 2000);
        }
    });

    bus.subscribe(EVENT_MISSION_COMPLETE, [](const Event& e) {
        NotifModule::instance().notify(NOTIF_INFO, "Mission complete!", "USB");
        triggerAnim(ANIM_LAUGH, 3000);
    });

    bus.subscribe(EVENT_SAVE_REQUESTED, [](const Event& e) {
        StorageModule::instance().savePetState();
    });

    Serial.println("[BOOT] Event subscriptions OK");

    // ─── STEP 17: Boot journal entry ────────────────────────────────
    Serial.println("[BOOT] Writing boot journal...");
    const char* stageName = STAGE_NAMES[PetCore::instance().state().stage - 1];
    char bootDetail[32];
    snprintf(bootDetail, sizeof(bootDetail), "Stage: %s", stageName);
    StorageModule::instance().appendJournal("BOOT", "POWERED ON", bootDetail);

    // Second entry, for diagnosing boots that nobody was watching. Reaching this
    // line is itself the useful part: it means setup() got past storage, the pet
    // and the panel, so a device that LOOKS dead was actually running. Combined
    // with the reset reason it separates "the firmware never got here" from "the
    // firmware ran and the screen is the problem", which is not guessable from
    // the outside. The battery reading is here because a brownout loop and a
    // firmware hang look identical on a blank panel.
    {
        char why[64];
#if HEXHOUND_HAS_BATTERY
        // The divider multiplier is logged, not assumed: it is a per-board macro
        // with a default, which is exactly the shape that silently resolves to
        // the wrong value. Reading it back on the device is the only proof.
        snprintf(why, sizeof(why), "%s %umV x%d", g_resetReasonName,
                 (unsigned)BatteryModule::instance().millivolts(),
                 (int)BATTERY_ADC_MULT);
#else
        snprintf(why, sizeof(why), "%s", g_resetReasonName);
#endif
        StorageModule::instance().appendJournal("BOOT", "RESET REASON", why);
        Serial.printf("[BOOT] Journalled reset reason: %s\n", why);
    }
    Serial.println("[BOOT] Journal OK");

    // Diagnostic builds only. Placed after SPIFFS is mounted, and it does not
    // delete the capture: someone had to go for a walk to produce it, so
    // re-reading must never depend on catching the dump first time.
    stepcap::dumpIfPresent();

    // ─── STEP 18: Final setup ───────────────────────────────────────
    delay(1500);
    lastTickTime  = millis();
    lastUIRefresh = millis();

    switchScreen(SCREEN_HOME);

    // Re-assert backlight AFTER home screen draw - belt and suspenders
#ifndef SIMULATOR_BUILD
    backlightAssert();
#endif

    setupComplete = true;
#if !defined(SIMULATOR_BUILD) && !defined(HEXHOUND_DISABLE_LED)
    if (heartbeatInit) {
        g_leds[0] = CRGB::Green;  // Green = setup complete
        showLED();
    }
#endif

#ifdef HEXHOUND_SOFT_TFT_DEBUG_HOLD
    Serial.println("[DEBUG_HOLD] Drawing post-setup diagnostic screen");
    drawSoftTftDebugHold(millis());
#endif

#ifdef DESKTOP_SIM
    // Test hook: HEXHOUND_SIM_FORCE_EVOLVE="from,to" (stage numbers, default
    // "2,3") plays the evolution cutscene right after boot so it can be
    // captured headlessly. No effect on hardware builds.
    if (const char* fe = getenv("HEXHOUND_SIM_FORCE_EVOLVE")) {
        int from = STAGE_PACKET_PUP, to = STAGE_BEACON_BEAST;
        if (fe[0]) {
            from = atoi(fe);
            if (const char* c = strchr(fe, ',')) to = atoi(c + 1);
        }
        startEvolveCutscene((PetStage)from, (PetStage)to);
    }

    // Test hook: HEXHOUND_SIM_FORCE_SCREEN=<name> renders one screen right after
    // boot so each can be captured headlessly. No effect on hardware builds.
    // Test hook: HEXHOUND_SIM_STAGE=1..5 sets the pet's stage before any screen
    // is drawn. Stage gates recipes, slots and half the HUD, so without it a
    // sim capture could only ever show the EGG's answer - which is why the Kit
    // rendered every cosmetic as "PUP" (locked) instead of as something owned,
    // and the owned rows were the ones being reported on. HEXHOUND_SIM_HUD_STAGE
    // still works and still means the same thing for the HUD alone.

    // Test hook: HEXHOUND_SIM_HUNGER=<0..100> sets the hunger stat and, below
    // the threshold updateAnimState() uses, puts the home animator into
    // ANIM_HUNGRY straight away.
    //
    // Added because the hungry pet was UNRENDERABLE. It is a steady state that
    // only appears after hunger decays below 30, which takes real time on a
    // real board, so the one animation state a keeper sees for long stretches
    // was the one no capture had ever contained. That is how it went unnoticed
    // that a hungry pet dropped to the pixel sprite and wore nothing.
    if (const char* hg = getenv("HEXHOUND_SIM_HUNGER")) {
        const int v = atoi(hg);
        if (v >= 0 && v <= 100) {
            PetCore::instance().state().stats.hunger = (uint8_t)v;
            if (v < 30) {
                UIHome::instance().animator().selectForStage(
                    PetCore::instance().state().stage, ANIM_HUNGRY);
            }
        }
    }

    // Test hook: HEXHOUND_SIM_OWN=cos.lamp,cos.rug,... puts crafted cosmetics in
    // the kit, and stands the den ones in the room. Without it the Den and the
    // Kit could only ever be captured EMPTY, which is why the screens that
    // report what you own and where it goes were being reasoned about from the
    // code instead of looked at. Unknown ids are skipped, not guessed.
    if (const char* ownList = getenv("HEXHOUND_SIM_OWN")) {
        Inventory& inv = Inventory::instance();
        PetState&  st  = PetCore::instance().state();
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", ownList);
        for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
            // A leading '-' grants the item WITHOUT equipping or placing it.
            // The default is to equip, because a capture of a feature usually
            // wants to show it in use - but a recording of the act of putting
            // something on has to start with it off, and there was no way to
            // ask for that.
            bool equip = true;
            if (*tok == '-') { equip = false; tok++; }

            // "id*qty" for materials, which are worth nothing one at a time:
            // a recipe wants five scrap, so a demo or a test that can only
            // grant one can never show the Kit in a craftable state.
            uint16_t qty = 1;
            if (char* star = strchr(tok, '*')) {
                *star = '\0';
                const int q = atoi(star + 1);
                if (q > 0 && q < 1000) qty = (uint16_t)q;
            }
            const uint8_t id = inv.idFor(tok);
            if (id == ITEM_ID_NONE) {
                Serial.printf("[SIM] unknown item id '%s', skipped\n", tok);
                continue;
            }
            inv.add(id, qty);
            // A den item is stood in the room and a worn item is put ON, so a
            // capture shows the state being reported rather than a pet holding
            // its wardrobe in a list. wearSet() enforces the same rules the
            // closet does, so this cannot fabricate a state the UI could not
            // reach.
            if (!equip) continue;
            if (wearIsWearable(id)) {
                wearSet(st, wearAnchorOf(id), id);
                continue;
            }
            if (flourishIsFlourish(id)) {
                flourishSet(st, id);
                continue;
            }
            if (!denIsPlaceable(id)) continue;
            for (uint8_t sl = 0; sl < DEN_SLOT_COUNT; sl++) {
                if (st.denSlots[sl] == DEN_SLOT_EMPTY) { st.denSlots[sl] = id; break; }
            }
        }
        st.dirty = true;
    }

    if (const char* fs = getenv("HEXHOUND_SIM_FORCE_SCREEN")) {
        if (!strcmp(fs, "menu")) {
            switchScreen(SCREEN_MENU);   UIMenu::instance().open();
        } else if (!strcmp(fs, "stats")) {
            switchScreen(SCREEN_STATS);  UIMenu::instance().drawStats();
        } else if (!strcmp(fs, "journal")) {
            switchScreen(SCREEN_JOURNAL);
            UIJournal::instance().loadEntries();
            UIJournal::instance().draw();
        } else if (!strcmp(fs, "config")) {
            switchScreen(SCREEN_CONFIG);  UIConfig::instance().open();
        } else if (!strcmp(fs, "missions")) {
            switchScreen(SCREEN_MISSIONS); UIMissions::instance().open();
        } else if (!strcmp(fs, "quests")) {
            switchScreen(SCREEN_QUESTS); UIQuests::instance().open();
        } else if (!strcmp(fs, "games")) {
            switchScreen(SCREEN_GAMES); UIGames::instance().open();
        } else if (!strcmp(fs, "inventory")) {
            switchScreen(SCREEN_INVENTORY); UIInventory::instance().open();
        } else if (!strcmp(fs, "report")) {
            switchScreen(SCREEN_REPORT); UIReport::instance().open();
        } else if (!strcmp(fs, "den")) {
            switchScreen(SCREEN_DEN); UIDen::instance().open();
        } else if (!strcmp(fs, "closet")) {
            switchScreen(SCREEN_CLOSET); UICloset::instance().open();
        } else if (!strcmp(fs, "update")) {
            switchScreen(SCREEN_UPDATE); UIUpdate::instance().open();
        } else if (!strcmp(fs, "roam")) {
            RoamModule::instance().begin(false);
            switchScreen(SCREEN_ROAM); UIRoam::instance().open();
        } else if (!strcmp(fs, "roamreport")) {
            // The WORST case on purpose: exploration, steps and all four
            // materials is the tallest this screen ever gets, and it is the
            // only arrangement that can push a row past the footer rule. A
            // report drawn with two rows proves nothing about the layout.
            RoamReport r{};
            r.pointsEarned  = 248;
            r.stepsTaken    = 164;
            r.stepsMeasured = true;
            r.samples       = 9;
            r.newToPet      = 17;
            r.newBleToPet   = 6;
            r.durationMs    = 173422;
            r.endReason     = ROAM_END_USER;
            // Field by field rather than braced: RoamFind carries default member
            // initialisers, which stop it being an aggregate under gnu++11, and
            // this file is compiled for three toolchains.
            // Real ITEM ids now: the report lists what the kit actually
            // received, so the fixture has to speak the same language.
            Inventory& finv = Inventory::instance();
            const char* const matKeys[4] = { "mat.scrap", "mat.copper",
                                             "mat.signal", "mat.lens" };
            const uint8_t  mats[4] = { finv.idFor(matKeys[0]), finv.idFor(matKeys[1]),
                                       finv.idFor(matKeys[2]), finv.idFor(matKeys[3]) };
            const uint16_t cnts[4] = {9, 4, 2, 1};
            r.findCount = 4;
            for (int i = 0; i < 4; i++) {
                r.finds[i].material = mats[i];
                r.finds[i].count    = cnts[i];
            }
            if (const char* why = getenv("HEXHOUND_SIM_ROAM_END")) {
                r.endReason = (uint8_t)atoi(why);
            }
            switchScreen(SCREEN_ROAM_REPORT, ROAM_REPORT_HOLD_MS, SCREEN_MENU);
            UIRoam::instance().drawReport(r);
        } else if (!strcmp(fs, "brief")) {
            switchScreen(SCREEN_MISSION_BRIEF);
            UIMissions::instance().drawBriefing(0);
        } else if (!strcmp(fs, "patrol")) {
            switchScreen(SCREEN_PATROL);
            PatrolSummary s{};
            s.wifiCount = 27; s.newWifiCount = 4;
            s.bleCount = 12;  s.newBleCount = 2; s.bleScanned = true;
            s.openAPCount = 3; s.dupeCount = 1; s.trackerCount = 2;
            s.foodGained = 8; s.xpGained = 45;
            // The WORST case on purpose: the tallest this screen ever gets is
            // every optional row plus every material a patrol can grant, and
            // that is the only arrangement that can push a row past the footer.
            {
                Inventory& pinv = Inventory::instance();
                const char* const pk[3] = { "mat.scrap", "mat.signal", "mat.shard" };
                const uint16_t pq[3] = { 4, 1, 1 };
                for (int i = 0; i < 3; i++) {
                    const uint8_t id = pinv.idFor(pk[i]);
                    if (id == ITEM_ID_NONE) continue;
                    s.materialId[s.materialCount]  = id;
                    s.materialQty[s.materialCount] = pq[i];
                    s.materialCount++;
                }
            }
            UIPatrol::instance().draw(s);
        } else if (!strcmp(fs, "alert")) {
            showAlert(NOTIF_WARN,
                      "BLE tracker candidate detected nearby during patrol",
                      "BLE Scan");
        } else if (!strcmp(fs, "hud")) {
            // Optional HEXHOUND_SIM_HUD_STAGE=1..5 to preview a given stage's
            // header (default = current pet stage).
            if (const char* hs = getenv("HEXHOUND_SIM_HUD_STAGE")) {
                int stg = atoi(hs);
                if (stg >= STAGE_EGG && stg <= STAGE_SENTINEL)
                    PetCore::instance().state().stage = (PetStage)stg;
            }
            UIPatrolHUD::instance().begin(PetCore::instance().state().stage);
            // Seed the radar so the capture shows a WORKING screen and not an
            // empty ring: a few networks put dots on the rim and move the
            // counters off zero.
            UIPatrolHUD::instance().onWiFiNetwork("hexhound-lab", false, false, 1);
            UIPatrolHUD::instance().onWiFiNetwork("open-guest",   false, true,  6);
            UIPatrolHUD::instance().onWiFiNetwork("hexhound-lab", true,  false, 11);
            currentScreen = SCREEN_PATROL_HUD;
        } else if (!strcmp(fs, "game")) {
            // Start a minigame so its play field and its GAME OVER screen
            // can be captured. There was NO hook for this, which is why the
            // games were the last screens never rendered at 480 - and why
            // all four GAME OVER screens shipped drawing their text on top
            // of itself.
            //
            // HEXHOUND_SIM_GAME=<n> picks which playable game (default 0).
            // Left running with no input a game reaches its over state on
            // its own, and that screen is held until a press, so a late
            // screenshot catches it without needing a back door into the
            // game's state machine.
            int gi = 0;
            if (const char* gs = getenv("HEXHOUND_SIM_GAME")) {
                gi = atoi(gs);
            }
            Minigame* g = games::playableAt(gi);
            if (g) {
                g_activeGame = g;
                switchScreen(SCREEN_GAME_PLAY);
                g->begin(tft);
                Serial.printf("[Sim] game %d: %s\n", gi, g->name());
            } else {
                Serial.printf("[Sim] no playable game at %d\n", gi);
            }
        }
    }
#endif

#ifdef HEXHOUND_DEMO_EVOLVE
    // Demo build: play one evolution cutscene (current stage -> next) on boot,
    // purely visually. All pet-state saves are compiled out in this build, so
    // the stored stage/XP/stats are never touched. Replug USB to replay.
    {
        PetStage cur = PetCore::instance().state().stage;
        PetStage nxt = (cur < STAGE_SENTINEL) ? (PetStage)(cur + 1) : cur;
        Serial.printf("[DEMO] Forcing evolution cutscene %d -> %d (no persist)\n",
                      cur, nxt);
        startEvolveCutscene(cur, nxt);
    }
#endif

    Serial.println();
    Serial.println("========================================");
    Serial.println("  [BOOT] SETUP COMPLETE - ALL OK");
    Serial.printf("  Total boot time: %lu ms\n", millis());
    Serial.println("========================================");
    Serial.println();
#endif // MINIMAL_BOOT

#ifdef HEXHOUND_RGB_PANEL
    // Push whatever setup() drew - splash, first home screen - before loop()
    // takes over the flushing. Outside the MINIMAL_BOOT guard on purpose: a
    // diagnostic build that draws something must still be able to show it.
    if (tft) tft->present();
#endif
}

// ── Main Loop ──────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

#ifdef HEXHOUND_RGB_PANEL
    // T-RGB draws into a PSRAM canvas rather than straight at the panel, so
    // whatever the previous iteration drew has to be pushed out. See
    // src/hal/tft_compat_rgb.cpp for why that indirection is unavoidable here.
    //
    // At the TOP of loop(), not the bottom: the diagnostic builds below take
    // several early returns, and a trailing flush would be skipped by every
    // one of them. Flushing the previous iteration's work costs one iteration
    // of latency, which at these loop rates is not observable.
    if (tft) tft->present();
#endif

    // ── Pay out anything that finished a quest ─────────────────────
    //
    // Here rather than beside each QuestEngine::reportProgress() call, of which
    // there are four today: a fifth added later would silently stop paying, and
    // an unpaid quest is invisible - the row still ticks over to done. Draining
    // an empty set is a compare and a return.
    PetRules::instance().payCompletedQuests();

    // ── Serial heartbeat - proves serial is alive ──────────────────
    static uint32_t lastSerialBeat = 0;
    if (now - lastSerialBeat >= 5000) {
        lastSerialBeat = now;
#ifdef MINIMAL_BOOT
        Serial.printf("[MINIMAL_BOOT] alive at %lu ms\n", now);
#else
        Serial.printf("[Loop] alive at %lu ms, screen=%d\n", now, currentScreen);
#endif
    }

    // ── OTA trial health ───────────────────────────────────────────────
    //
    // Above every early return below, on purpose. Those returns belong to the
    // diagnostic builds, and an image on trial has to be able to confirm or
    // roll itself back even in one of those; a build that could reach loop()
    // but never reach this line would sit pending forever, which is the exact
    // wedged-but-alive state the whole health check exists to prevent.
    //
    // Reaching loop() at all is the last required milestone: it means setup()
    // returned rather than hanging or panicking partway through.
    //
    // update() is a single boolean test and returns immediately when the
    // running image is permanent, which is every boot except the one directly
    // after an update. Calling it unconditionally every iteration costs
    // nothing and means there is no state in which it can be forgotten.
    OtaHealth::report(OTA_HEALTH_LOOP);
    OtaHealth::update();

    // ── The trial fact the session cannot work out for itself ──────────────
    //
    // WITHOUT THIS LINE the protection against erasing the last known-good
    // image is present in the code and absent on the device. OtaSession refuses
    // to arm, and refuses to write, while the RUNNING image is itself on trial,
    // because the slot an update would erase is not spare space then: it holds
    // the previous firmware and it is the only thing there is to roll back to.
    // The session has no ESP dependency and no way to discover this, so it is
    // injected, and it is injected here because this is where the answer is
    // known.
    //
    // Read AFTER update(), deliberately. update() is what confirms a trial
    // image, so reading before it would carry a "still on trial" for one more
    // iteration. That direction is harmless (a stale value can only refuse an
    // update that would have been fine, never allow one that would not), but
    // there is no reason to be wrong on purpose.
    OtaService::setRunningImageOnTrial(OtaHealth::isPending());

    // Drives the arm window and the receive stall timeout, then drains whatever
    // the host has sent. Unconditional and every iteration: an arm window that
    // is only ticked on some code paths is a device that can sit armed on a
    // desk indefinitely, which is precisely what the window exists to prevent.
    OtaService::pump(now);

#ifdef HEXHOUND_SOFT_TFT_DEBUG_HOLD
    drawSoftTftDebugHold(now);
    delay(50);
    return;
#endif

#ifdef MINIMAL_BOOT
    // ── MINIMAL_BOOT: Just serial heartbeat + backlight re-assert ──
    // Raw SPI test - TFT_eSPI not initialized, no tft-> calls allowed.
    // Screen filled CYAN in setup() and should stay visible (like tft-test).
#ifndef SIMULATOR_BUILD
    // Re-assert backlight every 2s as defensive measure
    static uint32_t lastBLAssert = 0;
    if (now - lastBLAssert >= 2000) {
        lastBLAssert = now;
        hexhoundSetBacklight(true);
    }
#endif
    delay(100);
    return;
#endif

    // ── Display keepalive (full boot only) ─────────────────────────
#ifndef SIMULATOR_BUILD
    static uint32_t lastDispKeepalive = 0;
    if (now - lastDispKeepalive >= 5000) {
        lastDispKeepalive = now;
        tft->writecommand(0x29);  // ST7735_DISPON
    }
#endif

    // ── LED heartbeat - blinks even if everything else fails ─────────
    // Throttled to ~10Hz to avoid excessive SPI bit-banging
#if !defined(SIMULATOR_BUILD) && !defined(HEXHOUND_DISABLE_LED)
    static uint32_t lastLedUpdate = 0;
    if (heartbeatInit && (now - lastLedUpdate >= 100)) {
        lastLedUpdate = now;
        if (setupComplete) {
            g_leds[0] = ((now / 2000) % 2 == 0) ? CRGB::Green : CRGB::Black;
        } else {
            g_leds[0] = ((now / 500) % 2 == 0) ? CRGB::Red : CRGB::Blue;
        }
        showLED();  // re-asserts backlight after every LED update
    }
#endif

    // 0. Active minigame owns the screen. Runs every iteration; the games
    //    rate-limit themselves internally. Returning false ends the round.
    if (currentScreen == SCREEN_GAME_PLAY && g_activeGame) {
        if (!g_activeGame->update(now)) {
            endActiveGame();
        }
    }

    // 0b. Roam runs whether or not its screen is showing, because the whole
    //     point is that it earns while the device is in a pocket. It must run
    //     AFTER IMUModule::update() above, whose sample it reads without
    //     touching the bus itself.
    if (RoamModule::instance().active()) {
        RoamModule::instance().update();
        if (!RoamModule::instance().active()) {
            // The module stopped itself, which today means the battery bound
            // was reached. Show what it brought back.
            endRoam(ROAM_END_BATTERY);
        } else if (currentScreen == SCREEN_ROAM) {
            UIRoam::instance().draw();
        }
    }

    // 1. Timer tick - decay pet stats
    if (now - lastTickTime >= TICK_INTERVAL_MS) {
        lastTickTime = now;
        // Roll the quest day on powered time as well as on power-on, so a
        // device left on a desk still gets fresh quests instead of the same
        // three forever.
        if (PetCore::instance().tickQuestDay()) {
            QuestEngine::instance().rollDaily(PetCore::instance().questDaySeed());
            Serial.println("[Quests] New day rolled");
        }
        EventBus::instance().publish(EVENT_TIMER_TICK);
    }

    // 2. Poll button
    TouchModule::instance().update();
    handleButton();
    BatteryModule::instance().update();
#if HEXHOUND_HAS_IMU
    IMUModule::instance().update();
#endif

    // 3. Update modules
    USBModule::instance().update();
    NotifModule::instance().update();

    // 3b. Evolution cutscene takes full control when active
    if (evolveActive) {
        updateEvolveCutscene();
        return;
    }

    // 3c. Update animation state (home screen only)
    if (currentScreen == SCREEN_HOME) {
        updateAnimState();
    }

    // 4. Update patrol state machine
    updatePatrol();

    // 5. Process event queue + apply rules
    EventBus::instance().processAll();

    // 6. Auto-save if dirty (debounced), but NOT while an expedition is running.
    //
    // A SPIFFS write blocks this loop for 400-530 ms, and the step detector is
    // fed from this loop at 20 Hz. Two real captured walks show what that costs:
    // ten stalls in one 173 s roam, 2.3 s of them inside the walking phase, and
    // 3-4 footfalls that are simply not in the data and that no threshold can
    // recover. The pet's 30 s hunger tick dirties the save, so this fired
    // roughly every 30 s throughout both walks.
    //
    // What is deferred is the pet's stat DECAY for the length of the roam, up to
    // ROAM_MAX_DURATION_MS. Not the expedition's earnings: those live in the
    // module until end() commits them, so an expedition interrupted by a flat
    // battery was already forfeit before this change, and end() sets dirty on a
    // module that is no longer active, which this condition then lets through on
    // the very next iteration.
#ifndef HEXHOUND_DEMO_EVOLVE
    if (PetCore::instance().state().dirty &&
        !RoamModule::instance().active() &&
        (now - lastSaveTime >= SAVE_DELAY_MS)) {
        lastSaveTime = now;
        StorageModule::instance().savePetState();
    }
#endif

    // 7. Screen timeout - return to home, or to the menu for a screen that came
    //    from there. switchScreen() clears the timeout on the way out, so this
    //    fires exactly once however the destination is painted.
    if (screenTimeout > 0 && now > screenTimeout &&
        currentScreen != SCREEN_PATROL_HUD &&
        currentScreen != SCREEN_EVOLVE) {
        // Every destination has to be PAINTED as well as switched to, which is
        // why this is a switch and not an assignment. Anything not listed falls
        // home, and that default is what sent a den toast home: placing an item
        // raises a toast, the toast timed out, and the timeout only knew how to
        // reach the menu or the home screen - so finishing a placement threw you
        // out of the room you were decorating.
        switch (screenTimeoutTo) {
        case SCREEN_MENU:
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
            break;
        case SCREEN_DEN:
            switchScreen(SCREEN_DEN);
            UIDen::instance().draw();
            break;
        case SCREEN_CLOSET:
            switchScreen(SCREEN_CLOSET);
            UICloset::instance().draw();
            break;
        default:
            switchScreen(SCREEN_HOME);
            break;
        }
    }

    // 8. Refresh UI (throttled, applicable screens only)
    if (currentScreen == SCREEN_HOME &&
        now - lastUIRefresh >= UI_REFRESH_MS) {
        lastUIRefresh = now;
        refreshUI();
    }

    // 8b. Redraw config screen periodically (for cursor pulse animation)
    if (currentScreen == SCREEN_CONFIG &&
        now - lastUIRefresh >= 500) {
        lastUIRefresh = now;
        UIConfig::instance().update();
    }

    // 8c. Redraw the update screen while a transfer is running.
    //
    // update() repaints only when something the owner can see has changed, so
    // this costs one integer compare on the overwhelmingly common iteration
    // where the session is IDLE. The 250 ms cadence is a deliberate ceiling
    // rather than a target: the progress bar shares an SPI bus with nothing,
    // but it does share the CPU with the flash writes the transfer is trying
    // to complete, and a smoother bar is not worth a slower update.
    if (currentScreen == SCREEN_UPDATE &&
        now - lastUIRefresh >= 250) {
        lastUIRefresh = now;
        UIUpdate::instance().update();
    }

#ifdef SIMULATOR_BUILD
    // 8c-bis. Test hook: HEXHOUND_SIM_PRESS="5200:s,5800:l" fires short and
    // long presses at those elapsed-ms marks.
    //
    // The simulator could put any screen on the glass and could not TOUCH it,
    // so every capture was a still life: the one thing the device does - you
    // press, it responds - was the one thing no recording could show. It is
    // also why a demo had to start with the pet already dressed, which rather
    // gives away the feature it is demonstrating.
    //
    // Fires through the same onShortPress() / onLongPress() the button and the
    // touch panel use, so a scripted press cannot reach a path a finger
    // cannot.
    {
        static uint32_t marks[32];
        static char     kinds[32];
        static bool     fired[32];
        static int      nMarks = -1;
        if (nMarks < 0) {
            nMarks = 0;
            if (const char* spec = getenv("HEXHOUND_SIM_PRESS")) {
                const char* p = spec;
                while (*p && nMarks < 32) {
                    marks[nMarks] = (uint32_t)strtoul(p, nullptr, 10);
                    const char* colon = strchr(p, ':');
                    kinds[nMarks] = (colon && colon[1]) ? colon[1] : 's';
                    fired[nMarks] = false;
                    nMarks++;
                    const char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            }
        }
        for (int i = 0; i < nMarks; i++) {
            if (!fired[i] && now >= marks[i]) {
                fired[i] = true;
                if (kinds[i] == 'l') onLongPress(); else onShortPress();
            }
        }
    }

    // 8d. Test hook: keep a FORCED patrol HUD animating.
    //
    // updatePatrol() is what normally calls hud.update(), and it only runs
    // while a real scan is in progress. HEXHOUND_SIM_FORCE_SCREEN=hud has no
    // patrol behind it, so the capture was only ever the STATIC frame that
    // begin() paints - and the sweep, the moving crosshair and everything else
    // update() redraws were unverifiable. That gap is exactly how a black box
    // under the pet survived a screenshot that looked right: begin() drew the
    // hub correctly and update() painted straight over it. Sim builds only.
    if (currentScreen == SCREEN_PATROL_HUD && patrolPhase == PATROL_IDLE) {
        UIPatrolHUD::instance().update();
    }
#endif
}

// ── Touch Navigation ───────────────────────────────────────────────────────
//
// A 2.1 inch capacitive panel driven by "short press scrolls one row, long
// press selects" is a bad deal: reaching MENU_ROTATE_DISPLAY costs twelve
// presses, and the owner is looking straight at the row he wants. Reported in
// his words: "On the menu I should be able to touch and hold the item in the
// menu I want to go to, I shouldn't need to press it up and down a bunch of
// times to get it to go to the next one."
//
// Touch is therefore an INPUT SOURCE, converted into the three actions this
// firmware already routes, rather than a feature bolted onto each screen:
//
//     tap or hold on a menu row  ->  put the cursor on that row, then select
//     tap elsewhere              ->  short press
//     hold elsewhere             ->  long press
//     tap on the footer strip    ->  back
//     swipe up / down            ->  scroll the list
//
// Not one of the ~16 screens becomes touch-aware, and the physical button keeps
// working exactly as before, which is the fallback on a board whose only other
// input is BOOT.
//
// Touch used to be OR'd straight into the button pin, which made every contact
// a press and every lingering finger a long press. That is gone: the gesture
// layer owns the panel now, and folding it back into the pin would fire both.
#if HEXHOUND_HAS_TOUCH

// The footer strip is Back. That is where "hold=back" is already printed on
// every leaf screen, so the affordance is one the owner has been reading since
// the first boot, and it keeps Back clear of the rows where a mis-hit would
// open something instead.
static bool touchInFooter(int y) {
#if HEXHOUND_PANEL_ROUND
    return y >= uiround::FOOTER_DIV;
#else
    return y >= uilg::FOOTER_DIV;
#endif
}

// Returns true when a gesture was consumed, in which case the caller skips the
// button state machine for this iteration - the same contract the IMU gesture
// path uses, so an action never fires twice from one input.
// True while a contact that a minigame has already been told about is still
// down, so one press produces exactly one move. Debounced by the recognizer's
// own contact state rather than the raw pin, because the FT3267 drops polls
// mid-contact and a raw read would report a second press for the same finger.
static bool g_gameContact = false;

// Page whichever list screen is up. Returns false when this screen has no
// paging, so the caller falls back to the button behaviour.
//
// One place that knows which screens page, rather than the same
// `if (currentScreen == ...)` written twice with opposite signs.
//
// The log line moved in here too. It used to print "page down" before the
// screen test, so every swipe on a NON-paging screen claimed to have paged -
// which is exactly what the bench log showed while the missions list was still
// stepping one row at a time. A log that lies about what it did is worse than
// no log.
static bool touchPageBy(int deltaPages) {
    const bool fwd = deltaPages > 0;
    switch (currentScreen) {
    case SCREEN_MENU:
        fwd ? UIMenu::instance().pageDown() : UIMenu::instance().pageUp();
        return true;
    case SCREEN_MISSIONS:
        fwd ? UIMissions::instance().pageDown() : UIMissions::instance().pageUp();
        return true;
    case SCREEN_QUESTS:
        fwd ? UIQuests::instance().pageDown() : UIQuests::instance().pageUp();
        return true;
    case SCREEN_GAMES:
        fwd ? UIGames::instance().pageDown() : UIGames::instance().pageUp();
        return true;
    case SCREEN_INVENTORY:
        fwd ? UIInventory::instance().pageDown() : UIInventory::instance().pageUp();
        return true;
    default:
        return false;
    }
}

static bool handleTouch() {
    TouchModule& touch = TouchModule::instance();
    if (!touch.isAvailable()) {
        return false;
    }

    const bool down = touch.isPressed();

    // Someone touched a sleeping pet: that is a check-in, exactly as a button
    // press is. Handled here because touch no longer reaches handleButton().
    if (down && PetCore::instance().isHibernating()) {
        PetCore::instance().rouse();
        PetMemory::recordCheckIn();
    }

    // NOT DURING A ROAM. An expedition is the device being carried in a pocket,
    // and a pocket is a conductor: a capacitive panel pressed against a leg is a
    // far better fake finger than a knock is a fake shake. The IMU gestures were
    // switched off during an expedition for precisely this reason, after two
    // real walks came back already ended and short of the count, and a hold on
    // SCREEN_ROAM still calls endRoam(). Nothing has changed about that trap
    // except which sensor walks into it.
    //
    // Reset rather than skip. A contact left half-parsed would resolve the
    // instant the expedition ended and dismiss the report before it could be
    // read - a worse bug than the one being avoided. The physical button still
    // ends a roam, which is what the roam screen's footer has always said.
    if (RoamModule::instance().active()) {
        g_touchNav.reset();
        return false;
    }

    const TouchPoint& p = touch.point();
    const touchnav::GestureEvent ev =
        g_touchNav.update(down, (int)p.x, (int)p.y, millis());

    // ── A round in progress gets the press on the way DOWN ────────────────
    //
    // Everywhere else a tap is deliberately resolved on RELEASE: it has to be,
    // because only a completed contact can be told apart from a hold or a
    // swipe. A minigame cannot afford that. The press is not reported until the
    // finger lifts AND the panel has stayed quiet for releaseGraceMs, and the
    // whole contact is DISCARDED if the finger slid more than tapSlop - 30 px
    // on this panel, which is nothing during a frantic round. Reported from the
    // field as "in Packet Capture touching the screen doesn't jump every time".
    //
    // So on this one screen the rising edge of contact IS the press, which is
    // what every action game does. The gesture recognizer still runs, because a
    // hold is the only way touch can abandon a round (the footer back zone is
    // deliberately off here), but its tap and swipe are swallowed: they would
    // be a second move for a finger that has already moved once.
    if (currentScreen == SCREEN_GAME_PLAY) {
        const bool contact = g_touchNav.contactInProgress();
        if (contact && !g_gameContact) {
            g_gameContact = true;
            // Logged like every other gesture. Without this the one screen
            // whose input was reported as unreliable was the one screen whose
            // presses were invisible in a serial capture.
            Serial.printf("[Touch] Game press at %d,%d\n", ev.x, ev.y);
            // WHERE the finger landed, not just that it landed. The chip games
            // pick the chip under the point; the games with nothing to aim at
            // inherit the positionless press through the default in
            // Minigame::onPressAt(). Committing whichever chip a sweeping
            // cursor happened to be on was reported from the board as pressing
            // ALLOW and being told OVERBLOCK.
            if (g_activeGame) {
                g_activeGame->onPressAt(ev.x, ev.y);
            } else {
                onShortPress();
            }
            return true;
        }
        if (!contact) {
            g_gameContact = false;
        }
        if (ev.kind == touchnav::GESTURE_HOLD) {
            Serial.println("[Touch] Hold -> abandon round");
            onLongPress();
            return true;
        }
        return false;
    }
    g_gameContact = false;

    switch (ev.kind) {
    case touchnav::GESTURE_NONE:
        return false;

    case touchnav::GESTURE_SWIPE_UP:
        // The list travels with the finger: swiping up brings later rows into
        // view. On the menu that is a PAGE of the window, not a cursor step -
        // a finger can see where it wants to go, so making it walk there one
        // row at a time was thirteen swipes to cross a fourteen-entry menu.
        // The cursor is carried along inside the new window; see UIMenu::
        // pageBy(). Selection is then a tap on the row you can now see.
        if (touchPageBy(1)) {
            Serial.println("[Touch] Swipe up -> page down");
        } else {
            Serial.println("[Touch] Swipe up -> next");
            onShortPress();
        }
        return true;

    case touchnav::GESTURE_SWIPE_DOWN:
        if (touchPageBy(-1)) {
            Serial.println("[Touch] Swipe down -> page up");
            return true;
        }
        Serial.println("[Touch] Swipe down -> previous");
        {
            // Every other list screen exposes only a one-way scroll that wraps.
            // Advancing is not what the finger asked for, but a swipe that did
            // nothing would read as a dead panel, and wrapping does get there.
            // Give those screens a backwards scroll and this becomes symmetric.
            onShortPress();
        }
        return true;

    case touchnav::GESTURE_TAP:
    case touchnav::GESTURE_HOLD:
        break;
    }

    // Two screens keep their footer strip:
    //
    // SCREEN_HOME because Back has nowhere to go from the root, and because the
    // round home screen prints its "hold = menu" hint down there. Swallowing a
    // hold on the one instruction the screen gives would be worse than having no
    // back zone at all.
    //
    // SCREEN_GAME_PLAY because a round in progress owns the whole glass. Its
    // only input is "press", it is played against a clock, and a player tapping
    // low on a 480 px panel must not have that read as "abandon the round".
    // Games are still left with the button, or by running out of time.
    const bool footerIsBack = currentScreen != SCREEN_HOME &&
                              currentScreen != SCREEN_GAME_PLAY;
    if (footerIsBack && touchInFooter(ev.y)) {
        Serial.println("[Touch] Footer -> back");
        onBackPress();
        return true;
    }

    if (currentScreen == SCREEN_MENU) {
        const int row = UIMenu::instance().rowIndexAtY(ev.y);
        if (row < 0) {
            // Dead space between the header rule and the first row. Doing
            // nothing beats guessing at the nearest row: the rows are 52 px
            // tall on the 480 panel, so a finger that missed them all missed on
            // purpose.
            return false;
        }
        // The request, in two lines. Tap and hold do the same thing here
        // deliberately: he asked to "touch and hold the item I want to go to",
        // and a hold that opened something different from a tap would be a
        // second thing to learn for no gain.
        Serial.printf("[Touch] Menu row %d -> select\n", row);
        UIMenu::instance().setCursor(row);
        onLongPress();            // long press on SCREEN_MENU opens the cursor
        return true;
    }

    // ── The sub-lists: land on the row under the finger ───────────────────
    //
    // Missions, quests, games and inventory all draw the same shape - a block
    // of uniform rows - and until now a tap on any of them fell straight
    // through to onShortPress(), which advances the cursor by exactly one.
    // That is the "inside the sub menus it still only moves one at a time"
    // report: the row is right there under the finger, and touch still had to
    // walk to it a step at a time like a button that cannot see.
    //
    // Two verbs, and they are the BUTTON'S OWN two verbs rather than a second
    // vocabulary to learn. All touch changes is that "move" becomes ABSOLUTE
    // instead of one-at-a-time:
    //
    //     tap  -> put the cursor on this row. Nothing else happens.
    //     hold -> put the cursor on this row AND open it, as hold always has.
    //
    // A tap deliberately does NOT open. Row 0 is BACK on all four screens, but
    // the rows under it CRAFT an item and SPEND the day's only reroll, and a
    // mis-hit that spends something the owner cannot get back is not a trade
    // worth one saved gesture. The main menu can afford to open on a tap
    // because every row there is navigation; here the commit is always a hold.
    {
        bool isList = true;
        int  row    = -1;
        switch (currentScreen) {
        case SCREEN_MISSIONS:  row = UIMissions::instance().rowIndexAtY(ev.y);  break;
        case SCREEN_QUESTS:    row = UIQuests::instance().rowIndexAtY(ev.y);    break;
        case SCREEN_GAMES:     row = UIGames::instance().rowIndexAtY(ev.y);     break;
        case SCREEN_INVENTORY: row = UIInventory::instance().rowIndexAtY(ev.y); break;
        default:               isList = false;                                  break;
        }

        if (isList) {
            if (row < 0) {
                // Missed every row: the header, the gap under it, or the rim.
                // Doing nothing beats guessing at the nearest row, and it must
                // NOT fall through to onShortPress() - advancing the cursor on
                // a tap that missed is the exact behaviour being fixed here.
                return false;
            }

            // Set the cursor first either way: onLongPress() reads it back to
            // decide what to open, so the hold path depends on this having
            // already happened. The repaint is only for the tap, which stays on
            // the screen; a hold is about to replace it, and on the 480x480
            // panel a wasted full redraw IS the whole 39 ms frame budget.
            const bool open = (ev.kind == touchnav::GESTURE_HOLD);
            switch (currentScreen) {
            case SCREEN_MISSIONS:
                UIMissions::instance().setCursor(row);
                if (!open) UIMissions::instance().draw();
                break;
            case SCREEN_QUESTS:
                UIQuests::instance().setCursor(row);
                if (!open) UIQuests::instance().draw();
                break;
            case SCREEN_GAMES:
                UIGames::instance().setCursor(row);
                if (!open) UIGames::instance().draw();
                break;
            case SCREEN_INVENTORY:
                UIInventory::instance().setCursor(row);
                if (!open) UIInventory::instance().draw();
                break;
            default:
                break;
            }

            Serial.printf("[Touch] Row %d -> %s\n", row,
                          open ? "open" : "select");
            if (open) {
                onLongPress();
            }
            return true;
        }
    }

    if (ev.kind == touchnav::GESTURE_HOLD) {
        Serial.println("[Touch] Hold -> long press");
        onLongPress();
    } else {
        Serial.println("[Touch] Tap -> short press");
        onShortPress();
    }
    return true;
}

#endif // HEXHOUND_HAS_TOUCH

// ── Button Handling ────────────────────────────────────────────────────────

void handleButton() {
    // Button handled directly by EvolveScene during cutscene
    if (evolveActive) return;

#if HEXHOUND_HAS_TOUCH
    // Touch first, and it owns the panel outright: it no longer feeds the
    // button pin, so nothing below can fire a second time from the same finger.
    if (handleTouch()) {
        return;
    }
#endif

#if HEXHOUND_HAS_TOUCH
    // Touch does NOT fold into the button pin on a board that has a panel.
    // handleTouch() above owns it now, and OR-ing it in here as well would fire
    // every action twice: once as a gesture, once as a press.
    bool pressed = (digitalRead(PIN_BUTTON) == LOW);
#else
    // Left exactly as it was for the boards with no touch panel, where
    // isPressed() has always been a constant false. Deliberately not "tidied
    // up": adding touch must not change a byte of any other board's firmware,
    // and that is verified by byte-comparing the built image, so dead code that
    // predates this work stays put.
    bool pressed = (digitalRead(PIN_BUTTON) == LOW) || TouchModule::instance().isPressed();
#endif

    // Someone touched a sleeping pet: that is a check-in, and it wakes up
    // pleased rather than resentful. Done here rather than inside rouse() so
    // the counter is only bumped by a real human press, not by any internal
    // caller that happens to reset the idle clock.
    if (pressed && PetCore::instance().isHibernating()) {
        PetCore::instance().rouse();
        PetMemory::recordCheckIn();
    }

#if HEXHOUND_HAS_IMU
    // Motion gestures map onto the same two actions the button drives, so no
    // screen needs to become gesture-aware:
    //   tilt  -> short press (scroll / advance)
    //   shake -> long press  (select / open menu)
    // This is the primary input on the round keychain board, whose only button
    // is BOOT and whose USB-C port is a UART bridge with no touch panel.
    //
    // NOT DURING A ROAM. An expedition is the device being carried and jostled
    // on purpose, and no gesture detector can tell "the owner shook it" from
    // "the owner walked" - which is the entire feature. Shake is 2200 mg of
    // total magnitude, and a tap on the back of the board, a tap on the glass,
    // or the board knocking against a leg in a trouser pocket all clear that.
    // Since shake maps to a long press and a long press on SCREEN_ROAM calls
    // endRoam(), the one board in the fleet that can count steps was ending its
    // own expeditions by being walked with. Reported from the field: two pocket
    // walks came back already stopped and short of the count, and the hand-
    // carried one, held still, ran to completion.
    //
    // The physical button still ends a roam, which is what the roam screen's
    // own footer has always said ("hold=end").
    //
    // Gestures are DRAINED rather than skipped. Leaving them pending would park
    // a shake in the module and fire it the instant the expedition ended, which
    // is a worse bug than the one being fixed: the report screen would dismiss
    // itself before it could be read.
    auto& imu = IMUModule::instance();
    if (imu.isAvailable()) {
        if (RoamModule::instance().active()) {
            imu.takeShake();
            imu.takeTiltLeft();
            imu.takeTiltRight();
        } else if (imu.takeShake()) {
            EventBus::instance().publish(EVENT_BUTTON_LONG);
            Serial.println("[IMU] Shake -> long press");
            onLongPress();
            return;
        } else if (imu.takeTiltLeft() || imu.takeTiltRight()) {
            EventBus::instance().publish(EVENT_BUTTON_SHORT);
            Serial.println("[IMU] Tilt -> short press");
            onShortPress();
            return;
        }
    }
#endif

    if (pressed && !buttonDown) {
        buttonDown     = true;
        buttonDownTime = millis();
        buttonHandled  = false;
        // How often does the button go down during an expedition? One press is
        // a person. Fifteen is a pocket working the switch, and that is the
        // difference the journal could not previously express: "button held"
        // was true at the pin and said nothing about who or what held it.
        if (RoamModule::instance().active() && roamButtonEdges < 0xFFFF) {
            roamButtonEdges++;
        }
    }

#if HEXHOUND_HAS_BUTTON_B
    // ── Button B, and both-held soft power off ─────────────────────────────
    //
    // Checked BEFORE A's long-press branch so that holding both never also
    // fires a long press on A on the way into sleep.
    const bool bPressed = (digitalRead(PIN_BUTTON_2) == LOW);

    if (pressed && bPressed) {
        if (bothDownTime == 0) {
            bothDownTime = millis();
        } else if (!bothHandled &&
                   millis() - bothDownTime >= BUTTON_BOTH_HOLD_MS) {
            bothHandled   = true;
            // Neither button's own gesture should also fire.
            buttonHandled = true;
            bHandled      = true;
            softPowerOff();
            return;
        }
    } else {
        bothDownTime = 0;
        bothHandled  = false;
    }

    if (bPressed && !bDown) {
        bDown     = true;
        bDownTime = millis();
        bHandled  = false;
    }

    if (!bPressed && bDown) {
        // B is Back on a short press. B long is deliberately unassigned for now
        // rather than guessed at.
        if (!bHandled && !bothHandled &&
            (millis() - bDownTime >= DEBOUNCE_MS)) {
            Serial.println("[Button] B -> Back");
            onBackPress();
        }
        bDown = false;
    }

    // While both are held, suppress A's gestures so the hold is not also a
    // long press on A.
    if (pressed && bPressed) {
        return;
    }
#endif

    if (pressed && buttonDown && !buttonHandled) {
        if (millis() - buttonDownTime >= BUTTON_LONG_PRESS_MS) {
            buttonHandled = true;
            EventBus::instance().publish(EVENT_BUTTON_LONG);
            Serial.println("[Button] Long press");
            onLongPress();
        }
    }

    if (!pressed && buttonDown) {
        if (!buttonHandled && (millis() - buttonDownTime >= DEBOUNCE_MS)) {
            EventBus::instance().publish(EVENT_BUTTON_SHORT);
            Serial.println("[Button] Short press");
            onShortPress();
        }
        buttonDown = false;
    }
}

// ── Short Press Routing ───────────────────────────────────────────────────

// End the active round, bank the result, and return to the game list. Safe to
// call when no game is running. Every exit path routes through here so a round
// can never be left holding memory, and the score can never be banked twice.
static void endActiveGame() {
    if (!g_activeGame) {
        return;
    }
    Minigame* game = g_activeGame;
    g_activeGame = nullptr;          // clear first: end() must not re-enter

    game->end();
    const MinigameResult r = game->result();

    if (r.completed) {
        // A finished round counts toward the play quest and the score memory.
        // An abandoned one does not, otherwise entering and exiting a game
        // would farm quest progress.
        PetMemory::recordGameScore(r.score);
        QuestEngine::instance().reportProgress(QUEST_CARE, 1);
        PetCore::instance().recordInteraction();

        // A small, purely digital yield: circuits and glitches, not scrap off
        // the pavement. Gated on r.completed with everything else here, so
        // entering and leaving a round cannot farm materials either.
        MaterialGrant mg[MATERIAL_GRANT_MAX];
        const uint8_t mgn = matyield::gameYield(r.score, r.completed, mg);
        Inventory::instance().applyGrants(mg, mgn);
        // Playing is Gremlin evidence; the two puzzle games are Cipher
        // evidence, since solving a cipher is not the same act as dodging a
        // packet and the form axis should be able to tell them apart.
        const char* gid = game->id();
        if (gid && (strcmp(gid, "cipher_sprint") == 0 ||
                    strcmp(gid, "signal_memory") == 0)) {
            PetForms::recordPuzzle(1);
        } else {
            PetForms::recordPlay(1);
        }
        PetForms::refresh(PetCore::instance().state());
    }

    switchScreen(SCREEN_GAMES);
    UIGames::instance().open();
}

// Close an expedition and show what it brought back. Single exit path, so the
// report is committed exactly once however the roam ends: the user stopping
// it, the battery bound being reached, or an alert taking the screen.
static void endRoam(RoamEnd reason) {
    if (!RoamModule::instance().active()) {
        return;
    }
    const RoamReport& r = RoamModule::instance().end(reason);

    // Persist the raw capture ONCE, here, rather than during the walk: writing
    // flash at 20 Hz would perturb the very sample timing being measured.
    // No-op inline unless HEXHOUND_STEP_CAPTURE is defined.
    stepcap::save(r.stepsTaken);

    // ── Say why it ended, somewhere that survives ─────────────────────────
    //
    // The screen alone is not enough: an expedition that ends in a pocket is
    // found minutes later, and whoever finds it deserves better than a number
    // that is short of the walk and no explanation. The journal outlives the
    // report screen, which now returns to the menu on its own.
    // Numbered, because there is no RTC and a journal of undated "ROAM" lines
    // cannot be matched to the walk it came from. roamSessions was already being
    // counted for its own sake and end() has just incremented it, so this is the
    // index of the expedition being reported.
    char d1[40], d2[40];
    snprintf(d1, sizeof(d1), "#%u %s +%lu pts",
             (unsigned)PetCore::instance().state().roamSessions,
             roamEndReasonName(r.endReason), (unsigned long)r.pointsEarned);
    if (r.stepsMeasured) {
        snprintf(d2, sizeof(d2), "%lu stp %lus %usmp btn%u",
                 (unsigned long)r.stepsTaken, (unsigned long)(r.durationMs / 1000),
                 (unsigned)r.samples, (unsigned)roamButtonEdges);
    } else {
        snprintf(d2, sizeof(d2), "%lus %u smp btn%u",
                 (unsigned long)(r.durationMs / 1000), (unsigned)r.samples,
                 (unsigned)roamButtonEdges);
    }
    StorageModule::instance().appendJournal("ROAM", d1, d2);
    Serial.printf("[Roam] ended: %s after %lu ms, %lu pts, %lu steps, %u samples, "
                  "%u button edges\n",
                  roamEndReasonName(r.endReason), (unsigned long)r.durationMs,
                  (unsigned long)r.pointsEarned, (unsigned long)r.stepsTaken,
                  (unsigned)r.samples, (unsigned)roamButtonEdges);

    // Roaming is Pathfinder evidence, credited once here from the finished
    // expedition rather than per sample, which would let a long idle roam
    // outvote everything else the pet ever did.
    if (r.pointsEarned > 0 || r.stepsTaken > 0) {
        PetForms::recordWalk(1);
        PetForms::refresh(PetCore::instance().state());
        QuestEngine::instance().reportProgress(QUEST_EXPLORE, 1);
    }

    // Held, not permanent. An expedition usually ends with the device going
    // straight back into a pocket, and a static report is the one screen the
    // owner is least likely to be looking at when it appears.
    switchScreen(SCREEN_ROAM_REPORT, ROAM_REPORT_HOLD_MS, SCREEN_MENU);
    UIRoam::instance().drawReport(r);
}

void onShortPress() {
    switch (currentScreen) {

    case SCREEN_HOME:
        if (patrolPhase == PATROL_IDLE) {
            startPatrol();
        }
        break;

    case SCREEN_MENU:
        UIMenu::instance().scrollDown();
        break;

    case SCREEN_JOURNAL:
        UIJournal::instance().scrollDown();
        break;

    case SCREEN_CONFIG:
        UIConfig::instance().onShortPress();
        break;

    case SCREEN_STATS:
        switchScreen(SCREEN_MENU);
        UIMenu::instance().draw();
        break;

    case SCREEN_MISSIONS:
        UIMissions::instance().scrollDown();
        break;

    case SCREEN_QUESTS:
        UIQuests::instance().scrollDown();
        break;

    case SCREEN_GAMES:
        UIGames::instance().scrollDown();
        break;

    case SCREEN_INVENTORY:
        UIInventory::instance().scrollDown();
        break;

    case SCREEN_DEN:
        UIDen::instance().scrollDown();
        break;

    case SCREEN_CLOSET:
        UICloset::instance().scrollDown();
        break;

    case SCREEN_REPORT:
        UIReport::instance().scrollDown();
        break;

    case SCREEN_UPDATE:
        UIUpdate::instance().scrollDown();
        break;

    case SCREEN_GAME_PLAY:
        // The only in-game input. Everything else about a round is time.
        if (g_activeGame) {
            g_activeGame->onPress();
        }
        break;

    case SCREEN_MISSION_BRIEF:
        switchScreen(SCREEN_MISSIONS);
        UIMissions::instance().draw();
        break;

    case SCREEN_ALERT:
        switchScreen(SCREEN_HOME);
        break;

    case SCREEN_PATROL:
        patrolPhase = PATROL_IDLE;
        switchScreen(SCREEN_HOME);
        break;

    default:
        break;
    }
}

// ── Long Press Routing ────────────────────────────────────────────────────

void onLongPress() {
    switch (currentScreen) {

    case SCREEN_HOME:
        switchScreen(SCREEN_MENU);
        UIMenu::instance().open();
        break;

    case SCREEN_MENU: {
        // A row can be visible and still not be earned yet. Say what would open
        // it rather than opening nothing: a menu entry that swallows a press is
        // indistinguishable from a broken one, which is the whole reason this
        // row is drawn at all instead of being hidden.
        if (UIMenu::instance().selectedIsLocked()) {
            showAlert(NOTIF_INFO,
                      "Locked. Reach Gremlin Mode to run missions.", "PET");
            break;
        }

        MenuItem sel = UIMenu::instance().select();
        switch (sel) {
        case MENU_PATROL:
            switchScreen(SCREEN_HOME);
            if (patrolPhase == PATROL_IDLE) {
                startPatrol();
            }
            break;
        case MENU_JOURNAL:
            switchScreen(SCREEN_JOURNAL);
            UIJournal::instance().loadEntries();
            UIJournal::instance().draw();
            break;
        case MENU_CONFIG:
            switchScreen(SCREEN_CONFIG);
            UIConfig::instance().open();
            break;
        case MENU_STATS:
            switchScreen(SCREEN_STATS);
            UIMenu::instance().drawStats();
            break;
        case MENU_ROTATE_DISPLAY:
            StorageModule::instance().toggleDisplayFlip();
            applyLandscapeRotation();
            UIMenu::instance().draw();
            StorageModule::instance().appendJournal(
                "CONFIG",
                StorageModule::instance().isDisplayFlipped() ? "DISPLAY FLIPPED" : "DISPLAY NORMAL",
                "");
            break;
        case MENU_MISSIONS:
            switchScreen(SCREEN_MISSIONS);
            UIMissions::instance().open();
            break;
        case MENU_QUESTS:
            switchScreen(SCREEN_QUESTS);
            UIQuests::instance().open();
            break;
        case MENU_GAMES:
            switchScreen(SCREEN_GAMES);
            UIGames::instance().open();
            break;
        case MENU_INVENTORY:
            switchScreen(SCREEN_INVENTORY);
            UIInventory::instance().open();
            break;
        case MENU_DEN:
            switchScreen(SCREEN_DEN);
            UIDen::instance().open();
            break;
        case MENU_CLOSET:
            switchScreen(SCREEN_CLOSET);
            UICloset::instance().open();
            break;
        case MENU_REPORT:
            switchScreen(SCREEN_REPORT);
            UIReport::instance().open();
            break;
        case MENU_UPDATE:
            switchScreen(SCREEN_UPDATE);
            UIUpdate::instance().open();
            break;
        case MENU_ROAM:
            // BLE is stage-gated content, so the caller decides, exactly as
            // patrol does. begin() refuses on a board that can measure charge
            // and does not have enough; the report says why.
            if (RoamModule::instance().begin(
                    PetCore::instance().state().stage >= STAGE_BEACON_BEAST)) {
                roamButtonEdges = 0;
                switchScreen(SCREEN_ROAM);
                UIRoam::instance().open();
            } else {
                // Refused (too little charge). Same hold: this is a message,
                // not a place to be left standing.
                switchScreen(SCREEN_ROAM_REPORT, ROAM_REPORT_HOLD_MS,
                             SCREEN_MENU);
                UIRoam::instance().drawReport(RoamModule::instance().report());
            }
            break;
        default:
            break;
        }
        break;
    }

    case SCREEN_QUESTS:
        if (UIQuests::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
        } else {
            // Reroll uses millis() rather than the day seed, so a reroll is a
            // genuinely different draw and not the same list handed back.
            UIQuests::instance().rerollSelected((uint32_t)millis());
        }
        break;

    case SCREEN_INVENTORY:
        if (UIInventory::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
        } else {
            // craftSelected() spends nothing on failure and returns the same
            // reason the footer is already showing, so there is nothing to
            // decide here: attempt it and let the screen redraw its own state.
            UIInventory::instance().craftSelected();
        }
        break;

    case SCREEN_REPORT:
        // Reading the report is what closes the span. Done here on the way
        // out, never from the draw path, which runs on every scroll and would
        // pin the span at zero.
        UIReport::instance().markReported();
        switchScreen(SCREEN_MENU);
        UIMenu::instance().open();
        break;

    case SCREEN_CLOSET:
        if (UICloset::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
        } else {
            // The toast is required, not decorative, for the same reason the
            // den's is: owning nothing for an anchor is normal for a long time,
            // and a long press that does nothing and says nothing reads as a
            // broken button.
            const ClosetResult r = UICloset::instance().act();
            const char* msg = UICloset::resultText(r);
            if (msg && *msg) {
                // Back to the CLOSET, not home: you are still dressing the pet.
                showAlert(NOTIF_INFO, msg, "Closet", SCREEN_CLOSET);
            }
        }
        break;

    case SCREEN_DEN:
        if (UIDen::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
        } else {
            // The toast is required, not decorative. Owning no den items is
            // normal for a long time, and a long press that does nothing and
            // says nothing reads as a broken button rather than as "you have
            // not made anything to put here yet".
            const DenResult r = UIDen::instance().act();
            const char* msg = UIDen::resultText(r);
            if (msg && *msg) {
                // Back to the DEN, not home: you are still decorating, and the
                // next slot you want is on the screen you just came from.
                showAlert(NOTIF_INFO, msg, "Den", SCREEN_DEN);
            }
        }
        break;

    case SCREEN_ROAM:
        // End the expedition early. Everything earned so far is committed by
        // end(); walking away does not forfeit it.
        endRoam(ROAM_END_USER);
        break;

    case SCREEN_UPDATE:
        // The screen owns the decision and its own redrawing; what comes back
        // is only the part it cannot do for itself. Note that arming happens
        // INSIDE act(), on this button press, by a person holding the device.
        switch (UIUpdate::instance().act(millis())) {
        case UPDATE_EXIT:
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
            break;
        case UPDATE_DO_RESTART:
            // ── The owner takes the restart, nothing takes it for them ────
            //
            // The image is verified and staged and the device is still running
            // the OLD firmware until this line. Everything up to here was
            // reversible by simply not doing it; this is the point the new
            // firmware gets its one chance to prove itself, and a person asked
            // for it explicitly.
            //
            // Saved first. The pet survives the partition write either way, but
            // it does not survive being up to thirty seconds stale, and a
            // restart the owner chose is not a reason to lose an evening's XP.
            StorageModule::instance().savePetState();
            StorageModule::instance().appendJournal("SYSTEM", "UPDATE INSTALLED",
                                                    "Restarting");
            Serial.println("[OTA] Owner confirmed the restart. Rebooting into "
                           "the new image, which must confirm itself or be "
                           "rolled back.");
            Serial.flush();
#ifndef SIMULATOR_BUILD
            ESP.restart();
#endif
            break;
        case UPDATE_STAY:
        default:
            break;
        }
        break;

    case SCREEN_ROAM_REPORT:
        switchScreen(SCREEN_MENU);
        UIMenu::instance().open();
        break;

    case SCREEN_GAMES:
        if (UIGames::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().open();
        } else {
            Minigame* game = UIGames::instance().selectedGame();
            if (game) {
                g_activeGame = game;
                switchScreen(SCREEN_GAME_PLAY);
                game->begin(tft);
                PetCore::instance().recordInteraction();
            }
        }
        break;

    case SCREEN_GAME_PLAY:
        // Long press always abandons. A player must never be trapped in a
        // round, whatever state the game itself has got into.
        endActiveGame();
        break;

    case SCREEN_JOURNAL:
        switchScreen(SCREEN_MENU);
        UIMenu::instance().draw();
        break;

    case SCREEN_CONFIG:
        if (UIConfig::instance().onLongPress()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().draw();
        }
        break;

    case SCREEN_STATS:
        switchScreen(SCREEN_MENU);
        UIMenu::instance().draw();
        break;

    case SCREEN_MISSIONS: {
        if (UIMissions::instance().backSelected()) {
            switchScreen(SCREEN_MENU);
            UIMenu::instance().draw();
            break;
        }
        currentMission = UIMissions::instance().selectedMission();
        switchScreen(SCREEN_MISSION_BRIEF);
        UIMissions::instance().drawBriefing(currentMission);
        break;
    }

    case SCREEN_MISSION_BRIEF: {
        auto& usb = USBModule::instance();
        if (!usb.isConnected()) {
            showAlert(NOTIF_WARN, "USB host not connected. Plug into a computer first.", "USB");
            break;
        }

        // A mission that presses the host's GUI modifier takes two holds: the
        // first arms it and repaints the brief with a warning naming the key
        // combination, the second runs it. Returns true on the first call for
        // the seven missions that only type text, so their path is unchanged.
        // See item H in docs/open-work.md.
        //
        // The break matters: the arming repaint has already happened inside the
        // gate, so there is nothing further to do on this press.
        if (!UIMissions::instance().longPressShouldExecute(currentMission)) break;

        const Mission& m = usb.getMission(currentMission);
        if (usb.executeMission(currentMission, GuiKeyConsent::ConfirmedOnGlass)) {
            if (m.flags & MISSION_HIGH_MISCHIEF) {
                PetCore::instance().changeMischief(10);
            }
            PetCore::instance().addXP(25);

            char d1[32];
            snprintf(d1, sizeof(d1), "M%d: %s", currentMission, m.name);
            StorageModule::instance().appendJournal("MISSION", d1, "");
            StorageModule::instance().savePetState();
        } else {
            // A failed mission used to be silent: the screen switched straight
            // home, so a build that CANNOT type looked exactly like a mission
            // that ran. Say which it was, the same way the not-connected case
            // a few lines above already does.
            //
            // USB_MISSION_FAILED_MSG is chosen at compile time in
            // usb_module.h, so no board carries a runtime check whose answer
            // its own build already fixed. The `break` matters: showAlert()
            // switches to SCREEN_ALERT, and falling through to the
            // switchScreen(SCREEN_HOME) below would erase the alert before it
            // was ever drawn.
            showAlert(NOTIF_WARN, USB_MISSION_FAILED_MSG, "USB");
            break;
        }

        switchScreen(SCREEN_HOME);
        break;
    }

    case SCREEN_ALERT:
        switchScreen(SCREEN_HOME);
        break;

    case SCREEN_PATROL:
        patrolPhase = PATROL_IDLE;
        switchScreen(SCREEN_HOME);
        break;

    case SCREEN_PATROL_HUD:
        // Was an explicit no-op, which is what made this screen a trap with no
        // way out. A long press now ends the scan and keeps what it found.
        abortPatrol();
        break;

    default:
        switchScreen(SCREEN_HOME);
        break;
    }
}

#if HEXHOUND_HAS_BUTTON_B || HEXHOUND_HAS_TOUCH

// ── Back Routing (button B, and the touch footer strip) ───────────────────
//
// Button B is Back, and only Back. Button A keeps the one-button gestures every
// other board uses, so muscle memory carries across the range and the boards
// with neither a second button nor a touch panel compile none of this.
//
// A touch board reaches the same routing by tapping the footer strip. It is
// deliberately the same function and not a touch-flavoured variant: Back means
// one thing on this device, and two implementations of "up one level" would
// disagree the first time a screen was added.
//
// The rule is "up one level", not "home": Back from a leaf screen lands on the
// menu it was opened from, and Back from the menu lands on home. Jumping
// straight home from a deep screen would make B and a long press
// indistinguishable, which defeats having a second button.
void onBackPress() {
    switch (currentScreen) {

    case SCREEN_HOME:
        // Already the root. Doing nothing is correct; there is nowhere up.
        break;

    case SCREEN_MENU:
        switchScreen(SCREEN_HOME);
        refreshUI();
        break;

    case SCREEN_EVOLVE:
        // A cutscene is not a screen you back out of. It owns the button
        // (handleButton returns early while evolveActive) and it is over in
        // seconds; interrupting it would leave the stage change half-presented.
        break;

    case SCREEN_PATROL_HUD:
        // Same as a long press here: stop scanning, keep what was found.
        abortPatrol();
        break;

    case SCREEN_ROAM:
        endRoam(ROAM_END_USER);
        break;

    case SCREEN_GAME_PLAY:
        // Abandon the round rather than sit through it. endActiveGame() banks
        // whatever the round is worth, exactly as running out of time does.
        endActiveGame();
        break;

    case SCREEN_PATROL:
        patrolPhase = PATROL_IDLE;
        switchScreen(SCREEN_HOME);
        refreshUI();
        break;

    case SCREEN_ALERT:
        switchScreen(SCREEN_HOME);
        refreshUI();
        break;

    default:
        // Every remaining screen is a menu leaf: journal, config, stats,
        // missions, quests, games, inventory, den, report, roam report, update.
        switchScreen(SCREEN_MENU);
        UIMenu::instance().open();
        break;
    }
}

#endif // HEXHOUND_HAS_BUTTON_B || HEXHOUND_HAS_TOUCH

#if HEXHOUND_HAS_BUTTON_B

// ── Soft power off (both buttons held) ────────────────────────────────────
//
// Matches the convention already used by the sibling DEF CON badges, quoting
// flock-you's own plan: "deep-sleep soft power (both buttons; wake on Button 2)".
//
// These boards have no power switch, so this is the only way to put one in a bag
// without it burning battery all weekend. It is deep sleep rather than a halt so
// it wakes on a single press and comes back through the normal boot.
static void softPowerOff() {
    Serial.println("[Power] Both buttons held - soft power off");

    // The pet is the only thing on the device that cannot be rebuilt from the
    // repo, and sleep is a planned shutdown, so save unconditionally rather than
    // waiting for the debounced autosave window.
    StorageModule::instance().savePetState();
    StorageModule::instance().appendJournal("SYSTEM", "SLEEP", "Both buttons");

    if (tft) {
        tft->fillScreen(TFT_BLACK);
        tft->setTextColor(TFT_CYAN, TFT_BLACK);
        tft->setTextSize(SCREEN_H > 100 ? 2 : 1);
        const char* msg = "Sleeping";
        int w = (int)strlen(msg) * 6 * (SCREEN_H > 100 ? 2 : 1);
        tft->setCursor((tft->width() - w) / 2, tft->height() / 2 - 8);
        tft->print(msg);
    }
    delay(600);   // long enough to read it, short enough not to feel stuck

#ifndef SIMULATOR_BUILD
    if (tft) tft->fillScreen(TFT_BLACK);
    hexhoundSetBacklight(false);

    // Turning the backlight "off" is NOT enough to make sleep look asleep.
    // Without something holding a pin, its level is undefined once the CPU
    // stops, and sleep presents as "just a backlit screen", which is
    // indistinguishable from a crash to whoever is holding the device. So the
    // panel's power rail is cut and HELD.
    //
    // CORRECTION, 2026-09-01. This comment used to say PIN_TFT_BL is GPIO38,
    // "outside the S3's RTC range (0..21), so it cannot be held". That is
    // FALSE. Digital pads GPIO26..GPIO48 have their own per-pin hold bits in
    // RTC_CNTL, and gpio_hold_en() followed by gpio_deep_sleep_hold_en() holds
    // them through deep sleep; that is exactly what the two Waveshare badges do
    // with their backlights in src/power/soft_sleep_tft.h. This board could
    // have held its backlight instead of cutting the rail.
    //
    // The rail-cutting is kept anyway. It works, it is proven on hardware, and
    // it is strictly stronger than holding the backlight because an unpowered
    // panel cannot show anything at all. Changing a demoed board's power
    // sequence to find out whether the lighter option is as good is not worth
    // the risk. The false claim is removed because the next person to write a
    // sleep routine reads this comment as a specification, and one lane already
    // began designing around this constraint before checking the TRM.
#if defined(PIN_TFT_POWER) && (PIN_TFT_POWER >= 0)
    digitalWrite(PIN_TFT_POWER, LOW);
    gpio_hold_en((gpio_num_t)PIN_TFT_POWER);
    gpio_deep_sleep_hold_en();
#endif

    // Wake on B going LOW. ext0 needs an RTC-capable pad, and GPIO14 is one.
    //
    // CORRECTION, 2026-09-01. This used to add "which is why B is the wake
    // button rather than A", implying A could not have served. That is FALSE:
    // the S3's RTC pads are GPIO0..GPIO21 and button A is PIN_BUTTON 0, inside
    // that range. Both buttons are RTC-capable here, so B is the ERGONOMIC
    // choice, not the only possible one. The three single-button boards added
    // later wake on GPIO0 precisely because it is RTC-capable.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON_2, 0);

    // Hold the pin's pull-up through sleep, or a floating input wakes it
    // immediately and the device appears not to sleep at all.
    gpio_pullup_en((gpio_num_t)PIN_BUTTON_2);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON_2);

    // Do not sleep while B is still down, or the press that asked for sleep is
    // also the press that wakes it.
    while (digitalRead(PIN_BUTTON_2) == LOW) {
        delay(20);
    }
    delay(120);   // debounce the release

    Serial.println("[Power] Entering deep sleep. Press B to wake.");
    // Deliberately NOT flushed, for the same reason the one before tft->init()
    // was removed: on a CDC-on-boot board flush() waits for the host to drain
    // the ring buffer, and a host that holds the port open without reading it
    // drains it so slowly that the wait ran to tens of seconds. Here that would
    // present as "held both buttons and nothing happened", which is the same
    // false-hang this whole hunt was about.
    esp_deep_sleep_start();
#endif
}

#endif  // HEXHOUND_HAS_BUTTON_B

// ── Patrol System (with Live HUD) ──────────────────────────────────────────

void startPatrol() {
    Serial.println("[Patrol] Starting with live HUD...");
    memset(&patrolSummary, 0, sizeof(patrolSummary));
    patrolSummary.bleScanned = false;

    auto& pet = PetCore::instance();
    pet.recordInteraction();

    prePatrolXP = pet.state().stats.xp;
    pet.resetFoodAccum();

    // Start the material tally with the run, not with the report: a scan that
    // lands after the previous patrol's screen was dismissed must not be
    // counted twice.
    PetRules::instance().beginPatrolTally();

    auto& hud = UIPatrolHUD::instance();

    WiFiModule::instance().setOnNetworkFound(
        [&hud](const char* ssid, bool isDupe, bool isOpen, int ch) {
            hud.onWiFiNetwork(ssid, isDupe, isOpen, ch);
        });

    BLEModule::instance().setOnDeviceFound(
        [&hud](const char* addr, bool isTracker) {
            hud.onBLEDevice(addr, isTracker);
        });

    currentScreen = SCREEN_PATROL_HUD;
    hud.begin(pet.state().stage);

    WiFiModule::instance().startScan();
    patrolPhase = PATROL_HUD_WIFI;
    hud.setPhase(HUD_PHASE_WIFI);

    Serial.println("[Patrol] WiFi scan started, HUD active");
}

void updatePatrol() {
    auto& hud = UIPatrolHUD::instance();

    switch (patrolPhase) {

    case PATROL_HUD_WIFI:
        hud.update();
        if (WiFiModule::instance().pollScan()) {
            auto& wifi = WiFiModule::instance();
            patrolSummary.wifiCount   = wifi.resultCount();
            patrolSummary.openAPCount = wifi.openCount();
            patrolSummary.dupeCount   = wifi.dupeCount();

            auto& pet = PetCore::instance();
            if (pet.state().stage >= STAGE_BEACON_BEAST) {
                hud.setPhase(HUD_PHASE_BLE);
                BLEModule::instance().startAsyncScan();
                patrolPhase = PATROL_HUD_BLE;
                Serial.println("[Patrol] BLE async scan started");
            } else {
                hud.setPhase(HUD_PHASE_COMPLETE);
                patrolPhase = PATROL_HUD_COMPLETE;
            }
        }
        break;

    case PATROL_HUD_BLE:
        hud.update();
        {
            int newDevices = BLEModule::instance().pollNewDevices(blePollBuf, 4);
            (void)newDevices;
        }
        if (BLEModule::instance().isAsyncScanDone()) {
            while (BLEModule::instance().pollNewDevices(blePollBuf, 4) > 0) {}
            auto& ble = BLEModule::instance();
            patrolSummary.bleScanned  = true;
            patrolSummary.bleCount     = ble.resultCount();
            patrolSummary.trackerCount = ble.trackerCount();
            hud.setPhase(HUD_PHASE_COMPLETE);
            patrolPhase = PATROL_HUD_COMPLETE;
            Serial.println("[Patrol] BLE scan done, completion animation");
        }
        break;

    case PATROL_HUD_COMPLETE:
        hud.update();
        if (hud.isComplete()) {
            hud.end();
            WiFiModule::instance().clearOnNetworkFound();
            BLEModule::instance().clearOnDeviceFound();
            finishPatrol();
        }
        break;

    case PATROL_RESULTS:
        if (millis() > patrolResultsEnd) {
            patrolPhase = PATROL_IDLE;
            switchScreen(SCREEN_HOME);
        }
        break;

    case PATROL_DONE:
        patrolPhase = PATROL_IDLE;
        switchScreen(SCREEN_HOME);
        break;

    case PATROL_IDLE:
    default:
        break;
    }
}

// Leave the patrol HUD early, at the owner's request.
//
// This screen used to have NO exit at all: onLongPress() carried an explicit
// `case SCREEN_PATROL_HUD: break;` that overrode the default "go home", short
// press had no case so it fell through to nothing, and the screen is exempt from
// the screen timeout. The device held you until the scan finished, with no
// feedback and nothing to press. Observed on a T-Display S3 and reported as
// "you cannot back out", which was exactly right.
//
// What the scan already found is kept, because discoveries are applied to the
// pet through the event bus as they arrive rather than at the end, so there is
// nothing to hand back. Credit for the outing follows endRoam()'s rule instead
// of finishPatrol()'s: pay it only if the trip actually did something, which
// keeps "walking away does not forfeit it" true without letting a
// start-then-instantly-abort loop farm quest progress.
static void abortPatrol() {
    auto& hud = UIPatrolHUD::instance();
    auto& pet = PetCore::instance();

    hud.end();                                  // also restores landscape
    WiFiModule::instance().clearOnNetworkFound();
    BLEModule::instance().clearOnDeviceFound();

    // Anything still queued from the partial scan belongs to the pet.
    EventBus::instance().processAll();

    // Counted, not derived. The seen-table is a ring: once full it evicts
    // rather than grows, so its COUNT stops changing while the pet is still
    // meeting things it has never met. Comparing the count against a
    // pre-patrol snapshot therefore reports "nothing new" forever.
    const bool foundSomething =
        (pet.state().stats.xp != prePatrolXP) ||
        (PetRules::instance().patrolNewWifi() > 0);

    if (foundSomething) {
        QuestEngine::instance().reportProgress(QUEST_CYBER, 1);
        QuestEngine::instance().reportProgress(QUEST_EXPLORE, 1);
        PetForms::recordWalk(1);
        PetForms::refresh(pet.state());
    }

    Serial.printf("[Patrol] Aborted by owner (%s)\n",
                  foundSomething ? "banked what it found"
                                 : "nothing found, no credit");

    patrolPhase = PATROL_IDLE;
    switchScreen(SCREEN_HOME);
    refreshUI();
}

void finishPatrol() {
    auto& pet = PetCore::instance();
    auto& st  = pet.state().stats;

    // A completed patrol is what the cyber and exploration quests ask for.
    // Reported here rather than at startPatrol() so abandoning a scan does not
    // count. The engine only tracks progress; the reward is paid by the rules
    // layer below, which is what stops a content pack granting its own XP.
    QuestEngine::instance().reportProgress(QUEST_CYBER, 1);
    QuestEngine::instance().reportProgress(QUEST_EXPLORE, 1);

    // Behavioural evidence. A patrol is the pet going out and looking, which
    // is Pathfinder evidence; anything it flags is Guardian evidence; anything
    // new it logs is Archivist evidence. Recorded from the completed patrol,
    // not from starting one, for the same reason quest progress is.
    PetForms::recordWalk(1);

    EventBus::instance().processAll();

    // What the run banked. PetRules tallied it as each scan landed, because
    // that is the only place that knows which identifiers were new.
    {
        Inventory& inv = Inventory::instance();
        const MaterialGrant* t = PetRules::instance().patrolTally();
        const uint8_t tn = PetRules::instance().patrolTallyCount();
        patrolSummary.materialCount = 0;
        for (uint8_t i = 0; i < tn && patrolSummary.materialCount < PATROL_MAX_MATERIALS; i++) {
            const uint8_t id = inv.idFor(t[i].item);
            if (id == ITEM_ID_NONE || t[i].qty == 0) continue;
            patrolSummary.materialId[patrolSummary.materialCount]  = id;
            patrolSummary.materialQty[patrolSummary.materialCount] = t[i].qty;
            patrolSummary.materialCount++;
        }
    }

    patrolSummary.foodGained = pet.foodAccum();
    patrolSummary.xpGained   = (int)(st.xp - prePatrolXP);
    patrolSummary.newWifiCount = (int)PetRules::instance().patrolNewWifi();
    patrolSummary.newBleCount  = patrolSummary.bleScanned
        ? (int)PetRules::instance().patrolNewBle()
        : 0;
    if (patrolSummary.foodGained < 0) patrolSummary.foodGained = 0;
    if (patrolSummary.newWifiCount < 0) patrolSummary.newWifiCount = 0;
    if (patrolSummary.newBleCount < 0) patrolSummary.newBleCount = 0;

    char d1[40], d2[40];
    if (patrolSummary.bleScanned) {
        snprintf(d1, sizeof(d1), "WiFi:%d BLE:%d",
                 patrolSummary.wifiCount, patrolSummary.bleCount);
        snprintf(d2, sizeof(d2), "New:%d/%d +%dXP",
                 patrolSummary.newWifiCount, patrolSummary.newBleCount,
                 patrolSummary.xpGained);
    } else {
        snprintf(d1, sizeof(d1), "WiFi:%d BLE:LOCK",
                 patrolSummary.wifiCount);
        snprintf(d2, sizeof(d2), "New:%d BLE locked +%dXP",
                 patrolSummary.newWifiCount, patrolSummary.xpGained);
    }
    StorageModule::instance().appendJournal("PATROL", d1, d2);

    char summary[192];
    if (patrolSummary.bleScanned) {
        snprintf(summary, sizeof(summary),
                 "WiFi Networks: %d\nNew WiFi: %d\nOpen APs: %d\nDuplicate SSIDs: %d\n"
                 "BLE Devices: %d\nNew BLE: %d\nTrackers: %d\nXP Gained: %d\n",
                 patrolSummary.wifiCount, patrolSummary.newWifiCount,
                 patrolSummary.openAPCount, patrolSummary.dupeCount,
                 patrolSummary.bleCount, patrolSummary.newBleCount,
                 patrolSummary.trackerCount, patrolSummary.xpGained);
    } else {
        snprintf(summary, sizeof(summary),
                 "WiFi Networks: %d\nNew WiFi: %d\nOpen APs: %d\nDuplicate SSIDs: %d\n"
                 "BLE Devices: LOCKED\nNew BLE: LOCKED\nTrackers: LOCKED\nXP Gained: %d\n",
                 patrolSummary.wifiCount, patrolSummary.newWifiCount,
                 patrolSummary.openAPCount, patrolSummary.dupeCount,
                 patrolSummary.xpGained);
    }
    USBModule::instance().setLastPatrolSummary(summary);
    StorageModule::instance().savePetState();

    currentScreen = SCREEN_PATROL;
    UIPatrol::instance().draw(patrolSummary);
    patrolResultsEnd = millis() + 5000;
    patrolPhase = PATROL_RESULTS;

    triggerAnim(ANIM_HAPPY, 5000);

    Serial.printf("[Patrol] Results: WiFi=%d BLE=%d Open=%d Dupes=%d Trackers=%d\n",
                  patrolSummary.wifiCount, patrolSummary.bleCount,
                  patrolSummary.openAPCount, patrolSummary.dupeCount,
                  patrolSummary.trackerCount);
}

// ── Evolution Cutscene ────────────────────────────────────────────────────

void startEvolveCutscene(PetStage from, PetStage to) {
    Serial.printf("[Main] Evolution cutscene: %d -> %d\n", from, to);

    if (patrolPhase != PATROL_IDLE && patrolPhase != PATROL_RESULTS) {
        UIPatrolHUD::instance().end();
        WiFiModule::instance().clearOnNetworkFound();
        BLEModule::instance().clearOnDeviceFound();
        patrolPhase = PATROL_IDLE;
        Serial.println("[Main] Patrol cancelled for evolution cutscene");
    }

#ifndef HEXHOUND_DEMO_EVOLVE
    StorageModule::instance().savePetState();
#endif

    evolveActive    = true;
    evolveFromStage = from;
    evolveToStage   = to;
    currentScreen   = SCREEN_EVOLVE;
    screenTimeout   = 0;

    EvolveScene::instance().begin(from, to);
}

void updateEvolveCutscene() {
    auto& scene = EvolveScene::instance();
    scene.update();
    updateEvolveLED();

    if (scene.isComplete()) {
        evolveActive = false;

#ifdef HEXHOUND_DEMO_EVOLVE
        // Visual-only demo: do NOT mutate or persist pet state. Return home
        // showing the pet's REAL (unchanged) stage so nothing is lost.
        NotifModule::instance().clear();
        animOverrideEnd = 0;
        UIHome::instance().animator().selectForStage(
            PetCore::instance().state().stage, ANIM_IDLE);
        switchScreen(SCREEN_HOME);
        Serial.println("[Main] DEMO evolution cutscene finished (no persist)");
#else
        PetCore::instance().addXP(50);

        const char* name = STAGE_NAMES[evolveToStage - 1];
        StorageModule::instance().appendJournal("EVOLVED", name, "");
        StorageModule::instance().savePetState();

        NotifModule::instance().clear();
        animOverrideEnd = 0;
        UIHome::instance().animator().selectForStage(evolveToStage, ANIM_IDLE);

        switchScreen(SCREEN_HOME);
        Serial.println("[Main] Evolution cutscene finished");
#endif
    }
}

void updateEvolveLED() {
    auto& notif = NotifModule::instance();
    EvolvePhase phase = EvolveScene::instance().phase();

    uint16_t sc = STAGE_COLOR[evolveToStage - 1];
    uint8_t lr = ((sc >> 11) & 0x1F) * 8;
    uint8_t lg = ((sc >> 5)  & 0x3F) * 4;
    uint8_t lb = ( sc        & 0x1F) * 8;

    switch (phase) {
    case EVOLVE_FLASH:
    case EVOLVE_DISSOLVE:
        notif.ledClear();
        break;

    case EVOLVE_ENERGY: {
        uint32_t t = millis();
        if ((t / 80) % 2 == 0) {
            notif.setLEDColor(lr, lg, lb);
        } else {
            notif.ledClear();
        }
        break;
    }

    case EVOLVE_NAME_REVEAL: {
        uint32_t t = millis();
        if ((t / 400) % 2 == 0) {
            notif.setLEDColor(lr / 2, lg / 2, lb / 2);
        } else {
            notif.setLEDColor(lr, lg, lb);
        }
        break;
    }

    case EVOLVE_SPRITE_REVEAL:
    case EVOLVE_FANFARE:
    case EVOLVE_HOLD:
        notif.setLEDColor(lr, lg, lb);
        break;

    case EVOLVE_WIPE_OUT: {
        static uint32_t wipeStart = 0;
        if (wipeStart == 0) wipeStart = millis();
        uint32_t elapsed = millis() - wipeStart;
        uint8_t fade = (elapsed < 300) ? (uint8_t)(3 - elapsed * 3 / 300) : 0;
        notif.setLEDColor(lr * fade / 4, lg * fade / 4, lb * fade / 4);
        if (elapsed >= 300) wipeStart = 0;
        break;
    }

    default:
        notif.ledClear();
        break;
    }
}

// ── UI Helpers ─────────────────────────────────────────────────────────────

void switchScreen(Screen scr, uint32_t timeoutMs, Screen timeoutTo) {
    currentScreen = scr;
    screenTimeout = timeoutMs > 0 ? millis() + timeoutMs : 0;
    screenTimeoutTo = timeoutTo;

    if (scr != SCREEN_PATROL_HUD) {
        applyLandscapeRotation();
    }

    if (scr == SCREEN_HOME) {
        UIHome::instance().draw(true);
#ifndef SIMULATOR_BUILD
        backlightAssert();
#endif
    }
}

void applyLandscapeRotation() {
    if (tft) {
        uint8_t rotation = StorageModule::instance().getLandscapeRotation();
        tft->setRotation(rotation);
        TouchModule::instance().setDisplayRotation(rotation, tft->width(), tft->height());
    }
}

void refreshUI() {
    if (currentScreen == SCREEN_HOME) {
        UIHome::instance().draw(false);
    }

    if (NotifModule::instance().hasActive() && currentScreen == SCREEN_HOME) {
        auto& n = NotifModule::instance().current();
        switchScreen(SCREEN_ALERT, 4000);
        UIAlert::instance().draw(n.level, n.message, n.source);
    }
}

#ifdef HEXHOUND_SOFT_TFT_DEBUG_HOLD
void drawSoftTftDebugHold(uint32_t now) {
    static uint32_t lastDraw = 0;
    static uint32_t frame = 0;
    if (now - lastDraw < 1000) {
        return;
    }
    lastDraw = now;
    frame++;

    // Stay in the panel's proven native portrait mapping for this hold test.
    // If these repeated full-screen fills remain visible, the low-level driver
    // is good and our rotated/UI drawing layer is the remaining problem.
    tft->setRotation(0);

    const uint16_t colors[] = {
        TFT_RED,
        TFT_GREEN,
        TFT_BLUE,
        TFT_YELLOW,
        TFT_CYAN,
        TFT_MAGENTA,
        TFT_WHITE
    };
    uint16_t color = colors[(frame - 1) % (sizeof(colors) / sizeof(colors[0]))];
    tft->fillScreen(color);

#ifndef SIMULATOR_BUILD
    hexhoundSetBacklight(true);
    tft->writecommand(0x29);  // ST7735_DISPON
#endif
    Serial.printf("[DEBUG_HOLD] frame=%lu color=0x%04X\n", (unsigned long)frame, color);
}
#endif

void showAlert(NotifLevel level, const char* msg, const char* src,
               Screen returnTo) {
    switchScreen(SCREEN_ALERT, 5000, returnTo);
    UIAlert::instance().draw(level, msg, src);
}

// ── Animation Helpers ──────────────────────────────────────────────────────

void triggerAnim(AnimID anim, uint32_t durationMs) {
    auto& animator = UIHome::instance().animator();
    PetStage stage = PetCore::instance().state().stage;
    animator.selectForStage(stage, anim);
    animOverrideEnd = millis() + durationMs;
}

void updateAnimState() {
    auto& animator = UIHome::instance().animator();
    auto& pet = PetCore::instance();
    uint32_t now = millis();

    if (animOverrideEnd > 0 && now > animOverrideEnd) {
        animOverrideEnd = 0;

        if (pet.state().stats.hunger < 30) {
            animator.selectForStage(pet.state().stage, ANIM_HUNGRY);
        } else {
            animator.selectForStage(pet.state().stage, ANIM_IDLE);
        }
    }
}

