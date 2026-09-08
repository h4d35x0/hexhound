# HexHound - Hardware Bring-Up Checklist

Target board: **LilyGo T-Dongle S3** (ESP32-S3-FN4R2)

---

## Pre-Flight

- [ ] PlatformIO project builds without errors
- [ ] USB-C cable is **data-capable** (not charge-only)
- [ ] Board appears as COM port / ttyUSB when plugged in
- [ ] `platformio.ini` target is a T-Dongle S3 environment (`board = dongles3`)
- [ ] Serial Monitor baud rate set to **115200**
- [ ] Recommended firmware target selected:
  - `lilygo-t-dongle-s3-vendor-baseline` for board validation
  - `lilygo-t-dongle-s3-vendor-app` for full HexHound firmware
- [ ] Expect the current vendor-app build to auto-format onboard SPIFFS on first boot if needed

## 1. Display (ST7735S 0.96" TFT)

- [ ] Run the vendor baseline first:
  - `pio run -e lilygo-t-dongle-s3-vendor-baseline -t upload`
- [ ] Screen cycles full-frame colors reliably:
  - red, green, blue, yellow, cyan, magenta, white, black
- [ ] Backlight stays on during the full color cycle
- [ ] HexHound full firmware uses the vendor display path:
  - `pio run -e lilygo-t-dongle-s3-vendor-app -t upload`
- [ ] `tft.setRotation(1)` gives landscape 160×80 in the app UI
- [ ] Backlight uses GPIO `38` with the active-low vendor control model
- [ ] Colors are correct (not inverted / shifted)
- [ ] Boot splash text visible at textSize(1)
- [ ] `fillScreen(TFT_BLACK)` clears to true black
- [ ] If the backlight turns on and then immediately turns off:
  - verify you are not running an older pre-vendor-app build
  - verify GPIO 38 is not being reset after PWM/backlight init

## 2. Persistence (Onboard SPIFFS + Optional MicroSD)

- [ ] On first boot, check serial output for one of:
  - `[Storage] SPIFFS ready`
  - `[Storage] SPIFFS formatted and mounted`
- [ ] If SPIFFS mount fails on a fresh board, reboot once on the current vendor-app build and confirm it formats
- [ ] `pet_state.json` persists across reboots even with no SD card inserted
- [ ] `journal.log` appends entries correctly on onboard flash
- [ ] If both SD and SPIFFS are available, the newest valid pet save wins on boot

## 3. SD Card (MicroSD via SPI)

- [ ] MicroSD card formatted as **FAT32**
- [ ] Card inserted fully (audible click)
- [ ] `SD.begin(PIN_SD_CS)` returns true (check Serial output)
  - Pins: CS=10, MOSI=11, SCK=12, MISO=13
- [ ] If `SD.begin()` fails:
  - Try `SPI.begin(12, 13, 11, 10)` before `SD.begin(10)`
  - Try a different card (some cards incompatible)
  - Check if board uses SDMMC interface instead
- [ ] Can write a test file: `SD.open("/test.txt", FILE_WRITE)`
- [ ] Can read the file back
- [ ] SD mirror files appear under `/sd/pet_state.json`, `/sd/journal.log`, and `/sd/config.json` when a compatible card is present

## 4. WiFi (ESP32-S3 Internal Radio)

- [ ] `WiFi.mode(WIFI_STA)` initializes without crash
- [ ] `WiFi.scanNetworks()` returns > 0 networks
- [ ] Known SSID appears in scan results
- [ ] No watchdog timeout during scan (if so, increase `WIFI_SCAN_TIMEOUT_MS`)
- [ ] WiFi properly de-initialized after scan (`WiFi.scanDelete()`)
- [ ] Patrol HUD shows WiFi networks in real-time

## 5. BLE (NimBLE Stack)

- [ ] NimBLE-Arduino library installed in PlatformIO
- [ ] `CONFIG_BT_ENABLED` defined in build (check with `#ifdef`)
- [ ] BLE scan starts without crash
- [ ] Nearby BLE devices detected (phone, fitness tracker, etc.)
- [ ] Scan completes within `BLE_SCAN_DURATION_S` (5s)
- [ ] No stack overflow - if crash, increase task stack size:
  - `build_flags = -DCONFIG_BT_NIMBLE_TASK_STACK_SIZE=8192`
- [ ] Async scan (`startAsyncScan()`) works for patrol HUD

## 6. RGB LED (APA102)

- [ ] FastLED configured: `FastLED.addLeds<APA102, 40, 39>(leds, 1)`
- [ ] LED lights up with `leds[0] = CRGB::Red; FastLED.show();`
- [ ] All three channels work: Red, Green, Blue
- [ ] `LED_BRIGHTNESS 40` is not too bright / too dim
- [ ] LED turns off cleanly: `leds[0] = CRGB::Black; FastLED.show();`
- [ ] Notification flash visible during alerts
- [ ] Evolution cutscene LED sync works

## 7. USB HID

- [ ] **WARNING:** HID requires `ARDUINO_USB_MODE=0` (OTG mode)
  - Default CDC mode (`USB_MODE=1`) does NOT support HID
- [ ] If using CDC mode: missions will only output to Serial (expected)
- [ ] If using OTG mode:
  - [ ] `USB.begin()` and `Keyboard.begin()` succeed
  - [ ] `Keyboard.print("test")` types on host computer
  - [ ] No conflicts with Serial (use `USBSerial` for debug output in OTG mode)
- [ ] Mission execution logs to Serial
- [ ] `EVENT_MISSION_COMPLETE` fires after execution

## 8. Button (GPIO 0)

- [ ] `pinMode(0, INPUT_PULLUP)` - reads HIGH when not pressed
- [ ] Short press detected (< 1000ms)
- [ ] Long press detected (>= 1000ms)
- [ ] Debounce works - no double-triggers
- [ ] Boot hold test: hold button during power-on → HW diagnostics runs
- [ ] Button works during evolution cutscene hold phase

## 9. Power & Stability

- [ ] No brownout resets (check Serial for "brownout detector" messages)
- [ ] Runs stable for 10+ minutes without crash
- [ ] No WDT (watchdog timer) resets during scans
- [ ] Flash usage reasonable: `ESP.getSketchSize()` < 2MB

## 10. Full Integration Test

- [ ] Boot → splash → home screen displays pet
- [ ] Short press → patrol → WiFi scan → BLE scan → results
- [ ] Long press → menu opens → all 4 items navigate correctly
- [ ] Journal shows patrol entries
- [ ] Config shows trusted SSIDs
- [ ] Pet Stats shows all 6 stat bars
- [ ] Evolution triggers cutscene (set XP threshold low to test)
- [ ] State persists across reboot (unplug and replug)
- [ ] LED responds to notifications and evolution

---

## Diagnostic Mode

Hold the button during boot to enter hardware diagnostics.
Tests run automatically and results display on screen.
Results are saved to `/sd/hwtest_results.log` when a working SD card is present.

Press button after tests complete to continue to normal boot.

---

## Board-specific bring-up: LilyGo T-RGB

This board does not follow the checklist above, because it is a different
display stack. Use the standalone `lilygo-t-rgb-bringup` environment, which is
one file in `build_src_filter` with empty `lib_deps` so it cannot disturb any
other target.

| # | Check | Pass criteria |
|---|-------|---------------|
| 1 | Identify the board **before** flashing | `esptool flash_id` reports ESP32-S3 rev v0.2, `Embedded PSRAM 8MB (AP_3v3)`, flash `ef 4018`, 16 MB. A wrong target boots perfectly and leaves the screen dark |
| 2 | I2C expander | `[TRGB] XL9535 at 0x20` on SDA 8 / SCL 48. If this fails nothing else will work |
| 3 | Panel power | expander IO2 (`power_enable`) driven HIGH. Without it the board is dead on battery but fine on USB, which is a confusing pair of symptoms |
| 4 | Panel init | V1 init table + V1 data pin map + 8 MHz pclk + RGB order. Colour bars and smooth ramps, no tearing |
| 5 | Flat field | a 7-second white/red/green/blue/black sweep runs unprompted at boot. Judge white **by eye**, not from a photo |
| 6 | Touch | `[Touch] FT3267 (0x38) ready`, chip_id 0x33, vendor_id 0x11 |
| 7 | Backlight | GPIO46 is a pulse-counted 16-step dimmer, **not PWM**: hold LOW to reset, a LOW->HIGH edge is full brightness, each further pulse steps down |
| 8 | Reset after flash | the board parks in ROM download mode after every upload; press reset or replug. Expected |

Two traps recorded from the bring-up:

- **Do not trust LilyGo's auto-detect.** Their `LilyGo_RGBPanel.cpp` fallback
  switches on a `LilyGo_RGBPanel_TouchType` value while comparing it against
  `LilyGo_RGBPanel_Type` enumerators - different enums - so the init table
  silently stays NULL. Pin the panel type explicitly.
- **A photograph is not a measurement.** A pale-blue white and a yellow-green
  yellow in two separate photos suggested a red-channel fault that did not
  exist; it was the camera's auto white balance against a warm desk. Only a flat
  field judged by eye settled it, which is why the sweep now runs at boot.
  Likewise, orientation read off a single photo of a round board gave the wrong
  answer, because board position and scan orientation are indistinguishable
  until you take two photos at different angles.

## Common Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Black screen on HexHound, but vendor baseline color-cycle works | Old display/backend path | Flash `lilygo-t-dongle-s3-vendor-app` |
| Backlight on briefly, then off | GPIO 38 backlight control reset after init | Use current vendor-app build |
| Boots as Egg after reboot | No readable persistence backend or no post-fix non-egg save yet | Check serial for SPIFFS mount, run one patrol, wait 5 seconds, then reboot |
| `SPIFFS init FAILED` | Uninitialized or corrupt onboard flash filesystem | Use the current vendor-app build; it should auto-format SPIFFS on first boot |
| Blank screen | Wrong TFT pins or driver | Run vendor baseline first |
| White screen | Missing `TFT_RST` connection | Verify GPIO 1 |
| Mirrored display | Wrong rotation/orientation mapping | Check `src/hal/tft_compat.cpp` vendor rotation settings |
| SD init fails | Wrong SPI pins or incompatible/no card | Pet persistence should still work from SPIFFS; check wiring or try a different card if SD logging matters |
| WiFi scan crashes | Stack overflow | Increase task stack size |
| BLE won't start | NimBLE not configured | Check `platformio.ini` lib_deps |
| LED wrong color | DATA/CLK swapped | Swap GPIO 39 and 40 |
| HID doesn't type | CDC mode active | Rebuild with USB_MODE=0 |
| Button bounces | Debounce too short | Increase DEBOUNCE_MS |
| Brownout resets | USB power insufficient | Use powered hub or shorter cable |

---

# Pre-release bench pass (T-RGB, about an hour)

Written 2026-08-30, after a day in which the item table, the save schema, the
home screen, the den, the closet, the patrol HUD, all four games and the shared
round header all changed, and exactly ONE screen was seen on hardware.

Everything below is verified in the simulator and by unit tests. **None of it
except item 1 has run on a real panel.** That is the gap this pass closes, and
it is the reason not to ship first.

Work top to bottom; each step feeds the next. Note anything odd immediately,
because a defect found at step 9 is hard to attribute back to step 3.

## 1. The save survived the schema bump (2 min)

Schema went 4 -> 5 today. This is the only item with real hardware evidence
already (a den that still had its furniture after a reset), but the rest of the
save has not been looked at.

- [ ] Pet name, stage and XP are what they were before the flash.
- [ ] The den still holds the same items in the same places.
- [ ] The closet still shows the hat and scarf ON the pet.
- [ ] KIT still lists what you had made.

If any of these came back empty, STOP. That is a migration defect and nothing
below matters until it is understood.

## 2. The pet wears its things everywhere (10 min)

The anchor table is normalised, so a fault here shows as the same offset on
every screen rather than one bad screen.

- [ ] Home: hat on the head, scarf at the neck, both clear of the stat arcs.
- [ ] Den: same, at the den's smaller pet size.
- [ ] Closet: same, and the three rows read HEAD / NECK / FX.
- [ ] Start a patrol. The hound at the centre of the radar wears both, and the
      sweep passes BEHIND it with no black box and no empty hole.

## 3. The hungry portrait (15 min, mostly waiting)

**Never seen on hardware.** It is a steady state, not a timed one, which is why
it went unnoticed for so long and why it matters more than it sounds.

- [ ] Let hunger fall under 30, or leave the board alone and come back.
- [ ] The pet dims and its neon goes dull. It must NOT drop to the blocky pixel
      sprite.
- [ ] It is still wearing the hat and scarf.
- [ ] Feed it. The bright portrait comes straight back.

## 4. Craft the new den items (10 min)

Three were added today and **none has ever been crafted.**

- [ ] KIT lists CABLE CRATE. Recipe is 5 SCRAP + 3 COPPER WIRE, at Packet Pup.
- [ ] Its icon is a crate and not a smear or another item's picture. (This is
      the one to look hardest at: a generator defect found the same day would
      have mis-drawn every cosmetic from item id 8 up.)
- [ ] Craft it. KIT shows HAVE:DEN.
- [ ] Place it in the den. The room grows to hold it.
- [ ] Check every other KIT icon still matches its name, especially ANTENNA
      HAT, SIGNAL SCARF, RECON GOGGLES and LED COLLAR.

## 5. Play all four games (15 min)

Rendered, never PLAYED. The clearing and the size ceilings both changed.

- [ ] PKT CHASE, SIG MEMORY, FIREWALL, CIPHER: each one start to game over.
- [ ] No ghost text above the footer rule at any point, including the first
      frame after entering from the home screen.
- [ ] FIREWALL: ALLOW and BLOCK sit INSIDE their boxes with both borders
      visible, and do not merge into one word.
- [ ] Chips, tiles and the chase pet fill the panel rather than sitting small
      in the middle. Say whether the new proportions are right; they are a
      design change and your call, not a correctness one.
- [ ] GAME OVER text does not overlap itself.

## 6. The round header band (5 min)

Changed late, after every lane had finished, so it had the least review.

- [ ] Open MENU, DEN, CLOSET, KIT, QUESTS, GAMES, MISSIONS, JOURNAL, REPORT,
      ROAM, UPDATE in turn.
- [ ] Every title is drawn and centred; no leftover text beside or under it
      when moving between screens with different title lengths.

## 7. Flourishes (only if you evolve)

**Never run on hardware in any form.** All three gate at Gremlin or Sentinel,
so this is unreachable at Packet Pup. If you do not evolve today, this ships
untested on a real panel and that should be a conscious decision.

- [ ] CLOSET, FX row: hold cycles through what you own, then off.
- [ ] Home screen: motes orbit the pet, clear of the title and the stat lines.
- [ ] They do not leave holes in the pet or trails behind them.
- [ ] Leave the home screen and come back; nothing is left on the glass.

## 8. Power and soft sleep (15 min)

Until 2026-09-01 this section said "let it sleep and wake", which was not a
thing this board could do: the deep-sleep path was gated on
HEXHOUND_HAS_BUTTON_B and the T-RGB has one button. It has a real sleep now,
and so do the Waveshare 1.28 round and the Waveshare Touch 1.47.

- [ ] Unplug and run on battery. Screen stays lit.
- [ ] MENU -> CONFIG. The cursor starts on the TAB BAR, not the first entry.
      One short press lands on a red `[SLEEP] HOLD=OFF` row.
- [ ] Short-press again: the highlight moves PAST it without executing. A
      press must never sleep the board.
- [ ] Put the cursor back on `[SLEEP]` and HOLD for about 1 second.
      Expect: green "SLEEPING" for ~0.6 s, then the screen goes BLACK AND
      UNLIT. Check it in a dark room and shine a light at it - the panel
      should show nothing at all, not a faint image.
- [ ] Press the BOOT button once. It wakes into a normal boot, splash and all,
      with the pet intact.

The failure appearances, because two of them have been real false-hangs on
this project and they implicate different things:

- Still on the config screen: the hold never reached onLongPress(). Look for
  `[Button] Long press` on serial. If it is absent it is the gesture, not sleep.
- "SLEEPING" then BACKLIT BUT BLANK: the panel is powered with nothing driving
  it. This is the outcome that looks exactly like a crash, and it is what the
  pad holds exist to prevent. Report it as backlit, not as dark.
- Backlight off but a white or garbage image: the panel was not held in reset.
- Stuck on "SLEEPING" for seconds: I2C is timing out. Say how many seconds.
- Splash comes straight back instead of sleeping: the button was held too long
  and the wake source saw it still down. Hold about 1 s, not 5.

- [ ] **THE CRITICAL ONE. Sleep, wake, then power-cycle and boot TWICE MORE.**
      Every boot must reach the splash normally. A permanently black screen on
      the second or later boot means a pad hold was never released, which is
      the T-Display S3's shipped bug reproduced. An S3 pad hold SURVIVES the
      core reset a wake performs, so the release is a boot-path job; it lives
      in trgb::begin() and in hexhoundInitBoardPower().
- [ ] Reset once more and confirm everything above is still there.

On the Waveshare 1.28 round, do the sleep test ON BATTERY, UNPLUGGED. Its
USB-C is a CH343P bridge whose DTR/RTS drive EN and GPIO0, so a host asserting
them can wake or reset the board and a spurious wake reads as a sleep failure.

Deep-sleep CURRENT is unmeasured and deliberately not minimised: the 1.28's
QMI8658 and the Touch 1.47's touch controller are left in their default states.
The contract was "genuinely dark and reliably wakeable", not a current figure.

## 9. Touch tuning, never confirmed on glass (10 min, T-RGB only)

Carried since 2026-08-28. The touch nav layer has 13 native tests and its
constants have never been judged by a finger.

- [ ] Tapping a sub-menu row selects THAT row, not the next one down.
- [ ] The 1000 ms hold feels right: not so short that a slow tap opens things,
      not so long that it feels unresponsive.
- [ ] A swipe scrolls, and the 80 px threshold is neither hair-trigger nor
      unreachable.
- [ ] Tapping the footer strip (y >= 380) goes back from any leaf screen.
- [ ] Carry it in a pocket during a roam. Touch must stay suppressed: a pocket
      is a conductor, and a hold on the roam screen would end the expedition.

## 10. The HID modifier guard (20 min, T-RGB or T-Dongle S3, HID image only)

This is the fix for the 2026-09-01 incident. Read
`docs/bugfix_log.md` (2026-09-02) before running it.

**Set the host up so a stuck modifier cannot cost anything.** The failure being
guarded against launches pinned taskbar apps and leaves the keyboard unusable.

- [ ] Use a host with an EMPTY taskbar, or a Linux host. `Win`+digit is the
      dangerous combination and it needs pinned apps to do anything.
- [ ] Close anything with unsaved work. Have a second machine to read this on.
- [ ] Know the recovery before you start: **unplug the board.** A sleep/wake
      cycle also works and is what saved the last one, but unplugging is
      immediate and certain.
- [ ] Optional but worth it: a UART adapter on GPIO43/44 at 115200. The HID
      image is a keyboard, not a serial port, so without one the `[USB]` lines
      below are invisible and this whole section is judged by behaviour alone.

**The part a bench pass can actually judge: the happy path still works.**

- [ ] Mission 8 `Term Tip` types its five `echo` lines. No modifiers involved,
      so this is the control that says typing works at all.
- [ ] Mission 5 `Sec Checklist` types its multi-line payload. `print()` presses
      SHIFT for every capital letter, so this exercises the closing clear.
- [ ] Mission 6 `Map Link` opens the Run dialog and the map link resolves.
- [ ] **Immediately after each mission, press a letter key on the real
      keyboard.** It must type that letter. If it opens something instead, a
      modifier is held and the guard did not hold - unplug the board and record
      exactly which mission it was.
- [ ] Mission 4 `Lock Screen` types its reminder and then locks. Log back in and
      check Event Viewer for an Event ID 4625 in the last minute: there must not
      be one. That was a separate defect, already fixed, and this is the cheap
      standing check that it stays fixed.

**The part a bench pass CANNOT judge without help.** Everything above passes
whether or not the guard exists, because it only exercises the path where
reports are delivered. The guard is the ABORT path, and nothing at the bench
makes `SendReport()` fail on demand.

To prove the abort path, build a temporary instrumented image: make
`USBModule::clearAllKeys()` `return false;` on its first line, flash, and run
mission 6.

- [ ] Nothing is typed. No Run dialog, no URL, no pinned app opens.
- [ ] The panel shows `Mission failed. Unplug and replug, then retry.`
- [ ] No XP is awarded and no journal entry is written.
- [ ] Re-flash the real image afterwards. **Do not leave the instrumented build
      on a board**; it looks identical and refuses every mission.

This control is the only step here that can fail for the right reason. A run
without it confirms that missions work, not that the guard does.

**Re-flashing either HID image needs the ROM download mode**, because the image
has no serial port for esptool to reset. Hold BOOT while plugging in, then
`pio run -e <env> -t upload --upload-port <COM>`.

## 11. Why does the T-RGB cycle on USB? (20 min, T-RGB only)

The 2026-09-01 symptom: the board connected and immediately disconnected,
repeatedly, on every port and cable. `[env:lilygo-t-rgb-hid-diag]` exists to
answer it - a composite image that is a keyboard AND a serial console.

**Read this first: the premise is an inference, and step 1 tests it.**
"Repeated connect/disconnect means the chip is resetting" was never observed,
only inferred. A host failing enumeration and re-driving the port looks
identical with nothing resetting at all.

Flash (BOOT-hold needed to get OFF the current image; once the diag image is
on, re-flashing does not need it, because `USBCDC` honours the DTR/RTS knock
and the 1200 bps touch):

```
pio run -e lilygo-t-rgb-hid-diag -t upload --upload-port COMx
pio device monitor -p COMx -b 115200
```

**Open the monitor BEFORE resetting the board.** `USBCDC::write()` returns 0
when no host has the port open, so pre-attach output is dropped, not buffered.

- [ ] **Is it resetting at all?** Count `[BOOT] HexHound starting...` lines
      against the number of connect/disconnect events. One boot with a churning
      port means nothing is resetting and the fault is host-side - on Windows
      look for "Unknown USB Device (Device Descriptor Request Failed)" in
      Device Manager.
- [ ] **If it IS resetting, read the reason.** `[BOOT] Reset reason:` names the
      PREVIOUS boot's cause. A hang cannot produce a reset on this firmware
      (`loopTaskWDTEnabled = false`, CPU1 idle unwatched, RTC WDT disabled), so
      the answer is one of a short list:
      - `PANIC (crash)` - the backtrace is on the line before. Note which
        `[BOOT] Init <module>...` line was the LAST one printed.
      - `BROWNOUT (power!)` - power, not firmware. Note this board had zero
        brownouts in its first 84 recorded boots.
      - `DEEPSLEEP wake` - a sleep/wake cycle, not a fault. Wake is ext0 on
        GPIO0, which is also BOOT. Cheap physical control: hold the board so
        nothing touches BOOT and see whether the cycle stops.
      - `SW (ESP.restart)` - something asked for it. Check for
        `[OTA] This image is ON TRIAL`, which would mean stale otadata is
        arming the rollback deadline every boot.
- [ ] **The keyboard must still work.** Press a mission and confirm the host
      receives keystrokes. **Do not read a quiet run as "HID is fine"** - if the
      file-scope keyboard in `usb_module.cpp` were ever reverted, this image
      would enumerate perfectly and type nothing, and the only complaint would
      go to UART0 where you cannot see it.
- [ ] **Re-flash a real image afterwards.** This is a DIAGNOSTIC build. Do not
      leave it on a board.

**You may not need any of this.** The reset reason is journalled to SPIFFS on
every boot and survives reflashing, so the incident boots are probably already
recorded on the device. Read it without flashing anything:

```
esptool read_flash 0xc90000 0x360000 trgb-spiffs.bin
strings -n 8 trgb-spiffs.bin | grep -aE "RESET REASON|SLEEP"
```

## What is NOT covered here, and why

- **The C5 target.** `lilygo-t-dongle-c5-vendor-app` cannot be built on this
  machine (TLS interception) and is not in the seven-board set, so it can rot
  without anything noticing.
- **The other six boards.** All seven build; only the T-RGB gets this pass.
  The rectangular changes (patrol HUD pet, patrol materials, closet layout)
  are simulator-verified only.
- **Anything OTA.** Untouched today.
