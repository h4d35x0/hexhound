# HexHound Architecture

## System Overview

HexHound is an event-driven embedded application. Hardware modules (WiFi, BLE, USB, touch, battery) publish or expose state to the rest of the system through narrow interfaces. PetRules subscribes to events and translates them into stat changes on PetCore. The UI layer reads PetCore state and renders to the TFT display. StorageModule persists state to onboard SPIFFS and mirrors to SD when an SD card is available. NotifModule drives the RGB LED on boards that have one. All coordination flows through events or board support helpers, so most modules stay loosely coupled.

On the LilyGo T-Dongle S3, the current known-good display path is a vendor-backed ST7735 implementation using `esp_lcd`, wrapped behind HexHound's `TFT_eSPI`-shaped compatibility layer in `src/hal/tft_compat.*`. This replaced the earlier direct TFT_eSPI boot path after hardware bring-up showed that the board's screen and backlight were stable on LilyGo's official stack but not on the original app stack.

On the Waveshare 1.47-inch boards, HexHound uses board-specific TFT_eSPI definitions for the ST7789 panel, optional capacitive touch input, and battery sensing where available.

## Module Map

```
  ┌─────────────────┐    events     ┌──────────────────┐
  │  wifi_module     │ ────────────>│                  │
  │  ble_module      │              │    event_bus     │
  │  usb_module      │ <────────────│                  │
  └─────────────────┘    commands   └────────┬─────────┘
                                             │
                                             ▼
                                     ┌──────────────┐
                                     │  pet_rules   │
                                     │ (subscribers) │
                                     └──────┬───────┘
                                            │ stat changes
                                            ▼
                                     ┌──────────────┐
                                     │   pet_core   │
                                     │  (state +    │
                                     │  evolution)  │
                                     └──────┬───────┘
                                            │
                          ┌─────────────────┼─────────────────┐
                          ▼                 ▼                  ▼
                    ┌──────────┐     ┌──────────┐      ┌───────────┐
                    │ UI layer │     │ storage  │      │  notif    │
                    │ (TFT)    │     │(SPIFFS+SD)│      │  (LED)    │
                    └──────────┘     └──────────┘      └───────────┘
```

### Module Responsibilities

| Module | File | Role |
|--------|------|------|
| **EventBus** | `events/event_bus.h/.cpp` | Ring buffer pub/sub. Modules publish events, subscribers get callbacks. Processed each loop iteration. |
| **PetCore** | `pet/pet_core.h/.cpp` | Owns `PetState` struct (stats, stage, traits). Handles stat clamping, evolution checks, JSON serialization. |
| **PetRules** | `pet/pet_rules.h/.cpp` | Subscribes to events (scan done, mission complete, timer tick) and calls PetCore stat modifiers. The "game logic" layer. |
| **WiFiModule** | `modules/wifi_module.h/.cpp` | Async WiFi scanning with per-network callbacks. Detects open APs, duplicate SSIDs. |
| **BLEModule** | `modules/ble_module.h/.cpp` | NimBLE BLE scanning (async + blocking modes). Detects devices, flags tracker candidates. |
| **USBModule** | `modules/usb_module.h/.cpp` | USB HID keyboard missions. 8 missions including dynamic WiFi audit, lock screen, URL open. |
| **StorageModule** | `modules/storage_module.h/.cpp` | Persistence backends. Loads the newest valid pet save from SPIFFS and/or SD, saves pet state (JSON), journal (line log), and config (trusted SSIDs/hosts plus display flip state). |
| **NotifModule** | `modules/notif_module.h/.cpp` | APA102 RGB LED control. Notification queue with level-based colors. Public LED API for cutscenes. |
| **TouchModule** | `modules/touch_module.h/.cpp` | Optional capacitive touch polling and coordinate rotation mapping for supported boards. |
| **BatteryModule** | `modules/battery_module.h/.cpp` | Optional battery voltage sampling and percentage estimation for boards with battery measurement hardware. |
| **Animator** | `ui/animator.h/.cpp` | Sprite frame cycling. Stage-aware animation selection (idle, scan, happy, hungry, alert, etc.). |
| **UI Screens** | `ui/ui_*.h/.cpp` | Screen-specific rendering. Each screen owns its draw/update logic. Singleton pattern. |
| **HWValidator** | `diagnostics/hw_validator.h/.cpp` | Boot-hold hardware test suite. Tests all subsystems, displays results, saves log. |
| **Display Wrapper** | `hal/tft_compat.h/.cpp` | Common drawing API used by the UI. Routes to simulator, legacy soft-TFT, or vendor `esp_lcd` backend. |
| **Backlight Helper** | `hal/backlight.h/.cpp` | Centralized GPIO 38 backlight control, including the active-low PWM behavior required by the T-Dongle S3 vendor path. |
| **Board Layer** | `board/board_profile.h`, `board/board_support.h` | Compile-time board capabilities, shared pin behavior, and board-specific helper logic. |

## Event Flow: Patrol Cycle

A complete patrol cycle demonstrates the full event flow:

```
1. User short-presses button on HOME screen
   └─> main.cpp::onShortPress() calls startPatrol()

2. startPatrol()
   ├─> PetCore::recordInteraction() (for egg hatch tracking)
   ├─> Switch to SCREEN_PATROL_HUD (portrait mode)
   ├─> WiFiModule::startScan() (async)
   └─> patrolPhase = PATROL_HUD_WIFI

3. WiFi scan runs (per-network callback → UIPatrolHUD::onWiFiNetwork)
   ├─> Each network: HUD updates live count
   ├─> Open AP found → EVENT_WIFI_OPEN_NETWORK
   └─> Duplicate SSID found → EVENT_WIFI_DUPLICATE_SSID

4. WiFiModule::pollScan() returns true (scan complete)
   ├─> EVENT_WIFI_SCAN_DONE published
   ├─> PetRules subscriber: feedHunger, addXP
   ├─> If stage >= BEACON_BEAST: start BLE scan
   └─> patrolPhase = PATROL_HUD_BLE

5. BLE scan runs (per-device callback → UIPatrolHUD::onBLEDevice)
   ├─> Tracker candidate → EVENT_BLE_TRACKER_ALERT
   └─> BLE scan completes

6. finishPatrol()
   ├─> EventBus::processAll() (flush pending events)
   ├─> PetRules applies all stat changes
   ├─> PetCore::checkEvolution() (may trigger EVENT_STAGE_EVOLVED)
   ├─> StorageModule::appendJournal("PATROL", ...)
   ├─> USBModule::setLastPatrolSummary(...) (for WiFi Audit mission)
   ├─> Switch to SCREEN_PATROL (landscape, results screen)
   └─> patrolPhase = PATROL_RESULTS (5s timeout → HOME)
```

## Event Flow: Evolution

```
1. PetCore::checkEvolution() detects XP threshold crossed
   └─> Publishes EVENT_STAGE_EVOLVED with payload: (fromStage << 8) | toStage

2. main.cpp subscriber catches EVENT_STAGE_EVOLVED
   └─> Calls startEvolveCutscene(from, to)

3. startEvolveCutscene()
   ├─> Saves pet state immediately
   ├─> Sets evolveActive = true (takes over main loop)
   ├─> Switches to SCREEN_EVOLVE
   └─> EvolveScene::begin(from, to)

4. Main loop short-circuits to updateEvolveCutscene()
   ├─> EvolveScene::update() advances through 8 phases:
   │   Phase 0: FLASH (200ms) - white screen burst
   │   Phase 1: DISSOLVE (600ms) - Knuth hash pixel dissolve
   │   Phase 2: ENERGY (800ms) - expanding color rings
   │   Phase 3: NAME_REVEAL (1000ms) - typewriter new stage name
   │   Phase 4: SPRITE_REVEAL (800ms) - dot → 1x → 2x scale + bounce
   │   Phase 5: FANFARE (1200ms) - stat lines slide in from left
   │   Phase 6: HOLD (1500ms) - animated sprite + "LEVEL UP" (button skip)
   │   Phase 7: WIPE_OUT (300ms) - progressive row wipe to black
   └─> updateEvolveLED() syncs APA102 LED to current phase

5. EvolveScene::isComplete() returns true
   ├─> Awards +50 XP bonus
   ├─> Writes "EVOLVED" journal entry
   ├─> Saves pet state
   ├─> Clears LED
   └─> Returns to SCREEN_HOME
```

## Screen State Machine

```
                    ┌──────────────┐
      boot hold ──>│   HWTEST     │──> (button) ──> normal boot
                    └──────────────┘

                    ┌──────────────┐
          ┌────────│     HOME     │────────┐
          │ long   └──────┬───────┘ short  │
          ▼               │                ▼
   ┌──────────┐           │         ┌──────────────┐
   │   MENU   │           │         │  PATROL_HUD  │
   └────┬─────┘           │         └──────┬───────┘
        │ long-select     │                │ auto
        ├───────────────────────┐          ▼
        │          │      │     │   ┌──────────────┐
        ▼          ▼      ▼     ▼   │   PATROL     │──> timeout ──> HOME
   ┌────────┐ ┌──────┐ ┌────┐ ┌────────┐ └──────────┘
   │JOURNAL │ │CONFIG│ │STATS│ │MISSIONS│
   └───┬────┘ └──┬───┘ └──┬─┘ └───┬────┘
       │ long    │ long   │ long  │ long (execute)
       └─────────┴────────┴───────┴──────────> HOME or MENU

                    ┌──────────────┐
   (any event) ──>│    ALERT     │──> timeout/press ──> HOME
                    └──────────────┘

                    ┌──────────────┐
   (evolution) ──>│   EVOLVE     │──> auto complete ──> HOME
                    └──────────────┘
```

Notes:
- PATROL_HUD uses portrait mode (80x160); all other screens use landscape (160x80)
- EVOLVE takes full control of the main loop - no button routing, no screen timeout
- MISSIONS only appears in the menu at Stage 4 (Gremlin Mode) and above
- Long press from sub-screens returns to MENU (not HOME)
- **Button B (Back) exists only where `PIN_BUTTON_2` is defined**, which today is
  the T-Display S3 alone. The capability is derived in `board_profile.h` as
  `HEXHOUND_HAS_BUTTON_B`, and every other board compiles none of the
  two-button code, so the one-button gesture set is unchanged on the six
  shipping boards. Proven per image by checking for the two-button string
  literals; static helpers get inlined so a symbol check finds nothing.
- Back moves UP ONE LEVEL (sub-screen -> menu -> home) rather than jumping home,
  which is what keeps it distinguishable from a long press.
- **Holding A+B for 2s is a soft power off** (deep sleep, wake on B), matching
  the sibling DEF CON badges. These boards have no power switch. That gesture
  needs a second button, so it exists only on the T-Display S3.
- **A board can be told to stop even with one button.** `HEXHOUND_HAS_SOFT_SLEEP`
  is the CAPABILITY ("this board's power hardware can be stopped safely") and is
  declared per board rather than derived from `HEXHOUND_HAS_BUTTON_B`, which is
  only the GESTURE. Conflating the two is why three battery-powered boards had no
  way to stop at all. It is set on the T-RGB, the Waveshare 1.28 round and the
  Waveshare Touch 1.47; every other board compiles none of it.
  - Entry on those three is a red `[SLEEP]` row directly under the CONFIG tab
    bar, committed only by a HOLD. A short press walks past it, and the cursor
    parks on the tab bar so the habitual hold-to-go-back can never land on it.
  - `src/power/soft_sleep.h` is the seam and it is a hard `#error` if a board
    sets the macro without binding an implementation. The T-RGB's is
    `trgb::enterDeepSleep()` (panel reset latched through its I2C expander); the
    two Waveshares share `src/power/soft_sleep_tft.h`, which is one file but not
    one recipe: the panel reset is an RTC pad on the 1.28 (GPIO12) and a digital
    pad on the Touch 1.47 (GPIO40), so they release their pad holds through
    different registers.
  - **An ESP32-S3 pad hold survives the core reset that a deep-sleep wake
    performs.** Whatever a sleep routine holds, some boot path must release
    unconditionally, or the first sleep is the last useful boot. The T-Display S3
    shipped exactly that bug. Releases live at the top of `trgb::begin()` and in
    `hexhoundInitBoardPower()`.
- `SCREEN_PATROL_HUD` used to have NO exit: `onLongPress()` carried an explicit
  no-op case that overrode the default, short press had no case, and the screen
  is exempt from the screen timeout. A long press now aborts the scan and banks
  what it found, following `endRoam()`'s rule of crediting the outing only if it
  actually did something.
- Long press from MISSIONS executes the selected mission, then returns to HOME
- `FLIP DISPLAY` toggles the stored orientation and updates both display and touch mapping on supported boards

## Supported Hardware

| Target | Display Path | Extras |
|--------|--------------|--------|
| LilyGo T-Dongle S3 | Vendor-backed `esp_lcd` ST7735 wrapper | RGB LED, optional SD, native USB |
| Waveshare ESP32-S3-LCD-1.47B | TFT_eSPI ST7789 | Battery sensing |
| Waveshare ESP32-S3-Touch-LCD-1.47 | TFT_eSPI ST7789 | Capacitive touch, battery sensing, soft sleep |
| Waveshare ESP32-S3-LCD-1.28 (round) | TFT_eSPI GC9A01A 240x240 | Round layout family, QMI8658 IMU input, battery sensing, soft sleep, no USB HID |
| LilyGo T-RGB (round) | **RGB parallel** ST7701S 480x480 + PSRAM framebuffer | Round layout family, FT3267 capacitive touch, XL9535 I2C expander, battery sensing, soft sleep, native USB |
| LilyGo T-Dongle C5 | Vendor-backed `esp_lcd` wrapper, C5 pins | SPIFFS only; SD shares SPI with the LCD |
| Desktop simulator | SDL2 software renderer | Fake WiFi/BLE/USB/storage |

## Display Stack Notes

### Current Recommended T-Dongle S3 Path

- Environment: `lilygo-t-dongle-s3-vendor-app`
- Panel driver: `src/esp_lcd_st7735.c` + `src/esp_lcd_st7735.h`
- Wrapper: `src/hal/tft_compat.cpp`
- Backlight control: `src/hal/backlight.cpp`

### Waveshare Paths

- Environments: `waveshare-esp32-s3-lcd-147b`, `waveshare-esp32-s3-touch-lcd-147`
- Panel path: TFT_eSPI with board-specific ST7789 pin definitions from `platformio.ini`
- Touch path: `src/modules/touch_module.cpp` on the touch variant
- Battery path: `src/modules/battery_module.cpp` on both Waveshare variants

### Round Panel Path (Waveshare ESP32-S3-LCD-1.28)

- Environment: `waveshare-esp32-s3-lcd-128`
- Panel path: TFT_eSPI `GC9A01_DRIVER`, 240x240, SCLK=10 MOSI=11 CS=9 DC=8
  RST=12 BL=40. `patch_tft_espi.py` is required (the `REG_SPI_BASE` fix), which
  is why this target does not need the `USE_HSPI_PORT` workaround.
- Layout family: `src/ui/ui_round.{h,cpp}`, selected by `HEXHOUND_PANEL_ROUND`.
  Every screen keeps its existing rectangular body untouched behind an
  `#if HEXHOUND_PANEL_ROUND` guard, so the round layouts add zero bytes and zero
  behaviour change to the other six boards.
- Input path: `src/modules/imu_module.cpp` (QMI8658). No touch controller on this
  variant; gestures map onto the existing short/long press actions in `main.cpp`.
- Not available here: SD (`HEXHOUND_HAS_SPI_SD 0`), addressable LED, and USB HID
  missions (`HEXHOUND_HAS_USB_HID 0` - the USB-C port is a CH343P UART bridge).

### Round Panel Path (LilyGo T-RGB, 480x480)

This is a **different display stack**, not a bigger panel.

- Environment: `lilygo-t-rgb`; bring-up target `lilygo-t-rgb-bringup`.
- Panel path: ST7701S driven by the ESP32-S3's **RGB parallel LCD peripheral**
  with a framebuffer in PSRAM. `src/hal/rgb_panel_trgb.*` and
  `src/hal/tft_compat_rgb.cpp`. **TFT_eSPI cannot drive this panel at all.**
- The panel's own SPI init lines are pins on an **XL9535 I2C expander at 0x20**
  (SDA 8 / SCL 48), not GPIOs: CS=IO3, MOSI=IO4, SCLK=IO5, RST=IO6, TP_RST=IO1,
  SDMMC_CS=IO7, and **power_enable=IO2 which must be driven HIGH** or the board
  is dead on battery.
- Once `esp_lcd` owns the bus there is no command channel, so `writecommand`,
  `writedata` and `invertDisplay` are no-ops, and **`setRotation()` is a no-op
  too**: it records the value and applies nothing, because the panel's scan
  already lands upright in the product orientation and a software rotation would
  cost a full-canvas transform per frame. Code that assumes a rotation took
  effect is wrong on this board.
- Scan-out is fixed at **26 Hz**; a full redraw is about 39 ms, which is the
  entire budget. Hence the dirty-Y-range design in `tft_compat_rgb.cpp`.
- Layout family: the same `src/ui/ui_round.*` as the 240 board, generalised
  rather than forked. `uiround::scaled(x)` = `x * DIAM / 240` reproduces every
  240 constant exactly by construction, pinned by `static_assert`.
- Touch: FT3267 at 0x38, on the same I2C bus as the expander.
- Backlight on GPIO46 is a **pulse-counted 16-step dimmer, not PWM**: hold LOW
  to reset, a LOW->HIGH edge selects full brightness, each further pulse steps
  down.
- SD is **SDMMC** (39/40/38), the first non-SPI card slot in this codebase, and
  is declared absent rather than mis-declared as SPI.
- It is the only round board with native USB, so it is the first that can reach
  the USB HID missions screen.

### Touch as an Input Source

On the boards with a capacitive panel, touch is deliberately **not** a
per-screen feature. `src/ui/touch_nav.h` is a pure gesture recognizer with no
knowledge of the controller, the panel geometry or the screens;
`handleTouch()` in `main.cpp` maps what it emits onto the same short-press,
long-press and back actions the physical button already drives. No screen became
touch-aware, and the button behaves exactly as before.

That split is also the only reason any of it is testable: the simulator has no
touch panel and the native runner has no board, so anything entangled with the
hardware read could only be checked by flashing and waving a finger at it.

- A release is only believed after a 60 ms quiet window, because the FT3267
  drops polls mid-contact and the first empty read would split one press into
  two.
- A hold fires **while the finger is still down**; a hold that only resolved on
  release felt like nothing had happened.
- Row targeting is answered from the geometry the last `draw()` actually used
  (`RowHitBox`), never recomputed, so a hitbox cannot drift from the rows it is
  a hitbox for.
- A minigame is the one exception: it takes the press on the **rising edge of
  contact**, because a tap resolved on release - after the quiet window, and
  discarded entirely if the finger slides past the tap slop - is unusable in a
  round played against a clock.
- Touch is suppressed and the recognizer reset **during a roam**. A pocket is a
  conductor, and a hold on the roam screen ends the expedition.

### Why This Exists

The original firmware path used TFT_eSPI-oriented build flags and S3-specific pin assumptions. The board flashed successfully, but the app black-screened while LilyGo's own factory screen code worked. The final architecture keeps the app's existing screen/UI code untouched while swapping the low-level transport to the vendor-proven `esp_lcd` ST7735 path.

### Important Hardware Behavior

- GPIO `38` controls the T-Dongle S3 backlight.
- On the tested board, the backlight must be treated as active-low in the vendor PWM path.
- Resetting GPIO `38` after PWM/backlight setup can turn the screen dark even while the application is still running normally.

## Evolution Requirements

| From | To | Requirement |
|------|-----|-------------|
| Egg | Packet Pup | 10 interactions + 1 WiFi scan + 1 USB connect |
| Packet Pup | Beacon Beast | 300 XP |
| Beacon Beast | Gremlin Mode | 900 XP |
| Gremlin Mode | Sentinel | 2,400 XP |

Evolution is checked after every stat change in PetCore. When triggered, the packed event `(fromStage << 8) | toStage` is published as `EVENT_STAGE_EVOLVED`.

After `Sentinel`, the pet continues progressing through Mastery ranks instead of further stage evolutions. Mastery XP unlocks long-tail perks and keeps late-game patrols meaningful.

## Storage Schema

### Persistence Backends

The current T-Dongle S3 vendor-app path uses two persistence backends:

- Primary verified backend: onboard `SPIFFS`
- Optional mirror/backend: SD card mounted under `/sd`

On boot, `StorageModule` scans both backends and loads the newest valid pet save using:

- `saveSeq`
- `stage`
- `hatched`
- `xp`

This prevents an older readable egg save on SD from overriding a newer save in onboard flash.

**Pet saves are written to alternating A/B slots.** Each backend holds two save
files, and a save always targets the slot that is *not* the newest good copy:

| Backend | Slot A | Slot B | Legacy (read-only) |
|---------|--------|--------|--------------------|
| SPIFFS  | `/pet_state_a.json` | `/pet_state_b.json` | `/pet_state.json` |
| SD      | `/sd/pet_state_a.json` | `/sd/pet_state_b.json` | `/sd/pet_state.json` |

Writing is not atomic on SPIFFS: the target file is deleted before being
recreated, so for a moment it does not exist. With a single save file, losing
power in that window destroyed the pet outright and the device booted as a
fresh egg. Alternating slots make that window survivable, because the other
slot still holds a complete save with a lower `saveSeq`. The worst case is
losing the most recent save interval, never the whole pet.

The single-file paths above are still *read* as candidates, so a unit running
firmware that predates the A/B scheme keeps its pet across the update. They are
never written to again.

`StorageModule::_saveSlot` chooses the next target. It is set at load time from
the freshest candidate, and only flips after a write actually lands, so a failed
write retries the same slot instead of consuming the good one.

If SPIFFS has never been initialized before, the current firmware will attempt to format and mount it automatically on first boot.

### `/pet_state.json` and `/sd/pet_state.json`

```json
{
  "name": "HexHound",
  "stage": 2,
  "hunger": 75,
  "mood": 60,
  "energy": 90,
  "xp": 150,
  "trust": 30,
  "mischief": 5,
  "health": 100,
  "trait0": 0,
  "trait1": 5,
  "interactions": 42,
  "wifiScans": 8,
  "usbConnects": 3,
  "saveSeq": 12,
  "hatched": true
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Pet name (default: "HexHound") |
| `stage` | uint8 | Current PetStage (1-5) |
| `hunger` | int16 | Hunger stat (0-100) |
| `mood` | int16 | Mood stat (0-100) |
| `energy` | int16 | Energy stat (0-100) |
| `xp` | uint32 | XP total |
| `trust` | int16 | Trust stat (0-100) |
| `mischief` | int16 | Mischief stat (0-100) |
| `health` | int16 | Health stat (0-100) |
| `trait0` | uint8 | First Trait enum value assigned at hatch |
| `trait1` | uint8 | Second Trait enum value assigned at hatch |
| `interactions` | uint16 | Total button interactions (for egg hatching) |
| `wifiScans` | uint16 | Total WiFi scans completed |
| `usbConnects` | uint16 | Total USB host connections |
| `saveSeq` | uint32 | Monotonic save sequence used to pick the newest valid save across backends |
| `hatched` | bool | Whether the egg has hatched |

### `/journal.log` and `/sd/journal.log`

CSV format, one entry per line:

```
millis|TYPE|detail1|detail2
```

Example entries:
```
15000|BOOT|POWERED ON|Stage: PACKET PUP
45000|PATROL|WiFi:12 BLE:3|Open:1 Dupe:0 +25XP
120000|EVOLVED|BEACON BEAST|
180000|MISSION|M3: WiFi Audit|
```

| Field | Description |
|-------|-------------|
| `millis` | System uptime in milliseconds when entry was written |
| `TYPE` | Entry category: BOOT, PATROL, EVOLVED, MISSION, ALERT, HATCH |
| `detail1` | Primary detail (type-specific) |
| `detail2` | Secondary detail (optional) |

Legacy format `[HH:MM:SS] text` is also parsed for backwards compatibility.

### `/config.json` and `/sd/config.json`

```json
{
  "trusted_ssids": ["HomeNetwork", "OfficeWiFi"],
  "trusted_hosts": ["workstation.local"],
  "display_flipped": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `trusted_ssids` | string[] | Up to 16 SSIDs excluded from alerts |
| `trusted_hosts` | string[] | Up to 8 host identifiers excluded from alerts |
| `display_flipped` | bool | Persists the user's preferred flipped orientation for display and touch mapping |

Notes:

- Config is loaded from SD first when present, then SPIFFS fallback.
- Pet state chooses the newest valid save across both backends instead of first-readable wins.

### /sd/hwtest_results.log

Plain text log written by HWValidator during boot diagnostics:

```
=== HexHound HW Test Results ===
Timestamp: 2500 ms
-------------------------------------------
Display    PASS
SD Card    PASS
WiFi       PASS
BLE        PASS
LED        PASS
USB        PASS
Button     PASS
-------------------------------------------
PASS: 7 / 7
FAIL: 0
===========================================
```

## Adding a New Mission

1. **Increment** `MAX_MISSIONS` in `src/modules/usb_module.h`

2. **Add the mission** to the `_missions[]` array in `usb_module.cpp`:
   ```cpp
   { 8, "My Mission",
     "Text payload to type on host keyboard",
     MISSION_NORMAL },  // or MISSION_HIGH_MISCHIEF
   ```

3. **For special missions** (not just typing text), add a `case` in `executeMission()`:
   ```cpp
   case 8:
       // Custom execution logic
       executeMyCustomAction();
       break;
   ```

4. **Implement helper** if needed (e.g., `executeMyCustomAction()`) - declare in the header's private section

5. **Set the flag** `MISSION_HIGH_MISCHIEF` if the mission is aggressive/stealthy. This:
   - Shows the `>M` indicator in the mission select UI
   - Awards +10 mischief when executed

6. **Test in HID mode** - flash with `t-dongle-s3-hid` environment and verify on a test machine

## Adding a New Sprite

1. **Design** a 16x16 pixel art frame (or 20x20 for Sentinel stage). Use a limited palette - see existing sprites for the color vocabulary.

2. **Convert to RGB565** - each pixel becomes a `uint16_t` value:
   ```
   RGB565 = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)
   ```
   Use magenta (`0xF81F`) for transparent pixels.

3. **Add to `sprites.h`** as a PROGMEM array:
   ```cpp
   static const uint16_t MY_SPRITE[] PROGMEM = {
       0xF81F, 0xF81F, 0x8410, ...  // 16x16 = 256 values
   };
   ```

4. **Register in Animator** - add the frame pointer to the appropriate stage's frame array in `animator.cpp`, and update `getStageIdleFrameCount()` if adding idle frames.

5. **Wire to AnimID** if it's a new animation type - add to the `AnimID` enum in `animator.h` and handle in `selectForStage()`.
