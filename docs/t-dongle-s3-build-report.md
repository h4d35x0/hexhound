# HexHound T-Dongle S3 Build Report

## Purpose

This document records the complete LilyGo T-Dongle S3 bring-up and recovery path for HexHound:

- what failed
- what worked
- what assumptions were wrong
- what was changed in the codebase
- what the final known-good deployment path is

This is the reference document for anyone building or debugging HexHound on a LilyGo T-Dongle S3.

## Final Outcome

HexHound is now running on the LilyGo T-Dongle S3 using a vendor-backed display path based on LilyGo's `esp_lcd` ST7735 implementation.

The currently verified persistence path for the tested board is:

- onboard `SPIFFS` for pet state, journal, and config
- optional SD mirror when an SD card mounts successfully

This matters because the tested hardware repeatedly failed SD initialization, but the final firmware still preserved patrol/evolution progress across unplug/replug by using onboard flash storage.

Known-good full firmware target:

```powershell
pio run -e lilygo-t-dongle-s3-vendor-app -t upload
```

Known-good board validation target:

```powershell
pio run -e lilygo-t-dongle-s3-vendor-baseline -t upload
```

## Hardware Confirmed

- Board: LilyGo T-Dongle S3
- MCU: ESP32-S3
- Display: ST7735 80x160
- Backlight pin: GPIO 38
- TFT pins:
  - `MOSI = 3`
  - `SCLK = 5`
  - `CS = 4`
  - `DC = 2`
  - `RST = 1`

## Original Symptoms

The original firmware behavior on the user's board looked like this:

- flashing succeeded
- the serial port worked
- the full app loop was alive
- the screen either stayed black or briefly glitched/flashed and then went dark

User-visible symptoms during debugging included:

- "screen just flashes like a glitch"
- "just black, like it is dead"
- brief unreadable text flash, then black
- blue/yellow streak when interrupted
- backlight turning on and then off

## What Went Right Early

Several things were healthy from the beginning:

1. The board flashed reliably.
2. USB serial was stable.
3. The ESP32 target was correctly identified as an S3.
4. The display hardware was not dead.

That mattered because it ruled out:

- bad USB flashing
- wrong CPU family
- total panel failure
- general boot crashes

## What Was Tested

### 1. Serial-Only Test

`serial-test` proved the board could boot and remain stable for long periods.

Result:

- success
- stable serial heartbeat

Meaning:

- board power, flash, and base firmware execution were fine

### 2. Raw TFT Diagnostic

`tft-test` bypassed the normal app stack and drove the panel directly.

Result:

- visible color flashes

Meaning:

- panel wiring and basic display communication were working

### 3. TFT_eSPI Isolation Test

`tft-espi-test` used the same library class shape as the full app without the rest of the app complexity.

Result:

- serial output was alive
- screen remained black

Meaning:

- the black-screen problem was not "the whole app"
- it was specifically the display stack used by the app

### 4. Custom Soft-TFT Experiments

A local ST7735-compatible wrapper was added to avoid TFT_eSPI's ESP32-S3 path.

This included:

- bit-banged writes
- slower SPI timing
- software rotation
- post-setup debug hold screens
- repeated color fills

Result:

- some brief flashes
- still not stable
- app stayed alive while the panel went black

Meaning:

- these experiments narrowed the problem but did not produce a reliable full-app solution

### 5. LilyGo Vendor Baseline

At that point the strategy changed completely:

- stop adapting the failing stack
- move to the official LilyGo display path as the reference truth

LilyGo's official repo was cloned locally, and a minimal vendor baseline was built inside this repo using:

- `espressif32@6.12.0`
- `esp_lcd`
- LilyGo's own `esp_lcd_st7735.c/.h`
- LilyGo's orientation and gap settings
- LilyGo-style backlight behavior

Result:

- full-screen color cycling worked reliably

Meaning:

- the board was healthy
- the panel was healthy
- the root problem was in HexHound's original display/backlight stack, not the board itself

## What Was Wrong

### Wrong Assumption 1: "If raw SPI works, the app's display library should be close enough"

This turned out to be false on this board.

The board could clearly display content, but not through the original full app path.

### Wrong Assumption 2: "Backlight is a simple active-high GPIO"

This was one of the most important failures.

LilyGo's own factory code uses the backlight differently:

- GPIO 38 is driven through PWM
- the effective logic for visible full brightness is active-low in the tested path

The original and intermediate experiments repeatedly treated the backlight as a simple active-high line.

### Wrong Assumption 3: "GPIO 38 can be reset freely during boot"

This also turned out to be false in the working vendor path.

Once the backlight control had been attached correctly, resetting GPIO 38 later in boot could turn the backlight off again, even while the application itself was still running correctly.

This matched the observed symptom:

- backlight on briefly
- then off

## Root Cause

The failure was a combination of two issues:

1. The original display backend for the app was not the right low-level path for this T-Dongle S3 board/revision.
2. Backlight control on GPIO 38 was being handled incorrectly, especially during later boot/reset phases.

The app could be alive and drawing, while the screen still appeared dead because the backlight path had been clobbered.

## Final Fix

### Display Backend Migration

HexHound's display wrapper was migrated to support a vendor-backed path using:

- `src/esp_lcd_st7735.h`
- `src/esp_lcd_st7735.c`
- `src/hal/tft_compat.h`
- `src/hal/tft_compat.cpp`

The UI code still talks to a `TFT_eSPI`-shaped API, but under the hood the T-Dongle S3 now uses LilyGo's working `esp_lcd` ST7735 stack.

### Backlight Control Fix

Backlight handling was centralized into:

- `src/hal/backlight.h`
- `src/hal/backlight.cpp`

That module now:

- handles vendor-mode backlight setup cleanly
- applies the active-low behavior needed by the tested board
- avoids letting random code paths drive GPIO 38 directly

### Boot Sequence Fix

The vendor-app boot path was updated so GPIO 38 is not reset after the backlight control path is already active.

That was the last major bug before the full app finally stayed visible.

## Persistence Failure and Final Fix

Once the display path was stable, a second user-visible failure remained:

- level-ups and patrol progress worked during a session
- unplug/replug returned the device to Egg
- Windows could still see the device and the app kept running

### What Was Actually Happening

Live boot logs on the tested board showed:

- `SPIFFS init FAILED`
- `SD card init FAILED`
- `No persistence backend available`
- `No readable pet save found - starting fresh`

So the problem was not "save logic never ran." The problem was that the device had nowhere valid to save or load from on boot.

### Wrong Assumption 4: "LittleFS is the right onboard flash filesystem for this board"

That turned out to be false for the current HexHound T-Dongle S3 setup.

The local board definition uses:

- `default_16MB.csv`

That partition table exposes a `spiffs` data partition, not a LittleFS partition. The earlier onboard-flash fallback therefore could not mount on the real device.

### Wrong Assumption 5: "If SD fails, persistence is effectively unavailable"

That also turned out to be unnecessary.

The board's SD path was unreliable during testing:

- mount failures
- card select failures
- no stable pet persistence from SD

The final fix made onboard flash the reliable core persistence path so HexHound no longer depends on a working SD card for basic pet progression.

### Persistence Fixes Applied

1. Switched the onboard flash backend from `LittleFS` to `SPIFFS`
2. Added auto-format on first SPIFFS mount failure
3. Changed save writes to overwrite old files instead of appending JSON
4. Added recovery of the last valid JSON object from previously appended/corrupted files
5. Added `saveSeq` and newest-valid-save selection across SPIFFS and SD
6. Added immediate pet-state saves after patrols, missions, and evolution events
7. Fixed the evolution return-to-Home flow so the newly evolved stage is shown immediately after the cutscene

### Final Known-Good Persistence Result

On the tested board, the final boot log showed:

- `SPIFFS ready`
- `SD card init FAILED`
- `No readable pet save found - starting fresh`
- later, after runtime activity, `Pet state saved to SPIFFS`

After a new patrol on the fixed firmware, progress persisted across unplug/replug even with SD still failing.

## New and Important Build Environments

### `lilygo-t-dongle-s3-vendor-baseline`

Purpose:

- board validation
- color-cycle confirmation
- display/backlight sanity test

Use this when:

- a screen appears dead
- a new board arrives
- you need to separate hardware faults from app faults

### `lilygo-t-dongle-s3-vendor-app`

Purpose:

- full HexHound firmware on the known-good LilyGo display path
- onboard SPIFFS persistence with optional SD mirror
- newest-valid-save selection across available backends

Use this as the default S3 deployment target going forward.

## Recommended Bring-Up Order

For a new T-Dongle S3, use this order:

1. Build and flash vendor baseline

```powershell
pio run -e lilygo-t-dongle-s3-vendor-baseline -t upload
```

2. Confirm stable full-screen color cycling

3. Build and flash full HexHound vendor app

```powershell
pio run -e lilygo-t-dongle-s3-vendor-app -t upload
```

4. Confirm HexHound home screen appears and stays visible

If step 2 fails, stop treating it as a HexHound problem.

## Commands Used During Recovery

Serial-only validation:

```powershell
pio run -e serial-test -t upload
pio device monitor -b 115200
```

Raw display diagnostic:

```powershell
pio run -e tft-test -t upload
pio device monitor -b 115200
```

Legacy TFT_eSPI diagnostic:

```powershell
pio run -e tft-espi-test -t upload
pio device monitor -b 115200
```

Vendor baseline:

```powershell
pio run -e lilygo-t-dongle-s3-vendor-baseline -t upload
```

Final full app:

```powershell
pio run -e lilygo-t-dongle-s3-vendor-app -t upload
```

Boot log capture during recovery:

```powershell
pio device monitor -b 115200 --port <PORT>
```

## Files Added or Changed for the Fix

### Added

- `src/esp_lcd_st7735.c`
- `src/esp_lcd_st7735.h`
- `src/hal/backlight.cpp`
- `src/hal/backlight.h`
- `src/hal/tft_compat.cpp`
- `src/lilygo_vendor_baseline.cpp`
- `docs/t-dongle-s3-build-report.md`

### Updated

- `platformio.ini`
- `src/main.cpp`
- `src/hal/tft_compat.h`
- `src/hal/hal_hardware.cpp`
- `src/modules/notif_module.cpp`
- `src/modules/storage_module.h`
- `src/modules/storage_module.cpp`
- `src/pet/pet_core.h`
- `src/pet/pet_core.cpp`
- `src/ui/ui_home.h`
- `src/ui/ui_home.cpp`
- `test/test_pet_core/test_pet_core.cpp`
- `test/test_storage/test_storage.cpp`
- `README.md`
- `docs/hardware_checklist.md`
- `docs/architecture.md`
- `docs/bugfix_log.md`

## What to Avoid Going Forward

1. Do not assume the old TFT_eSPI-based S3 path is the best default for this board.
2. Do not reset GPIO 38 casually after backlight setup.
3. Do not treat a black screen as proof the whole app is dead without first checking:
   - serial output
   - vendor baseline color-cycle
   - backlight state
4. Do not debug future screen failures only inside the full app. Always verify the vendor baseline first.

## What This Means for Future Work

### For S3

The T-Dongle S3 now has a credible deployment path in this repo.

Future S3 work should build from the vendor-app path, not the original display experiments.

### For C5

If a C5 port is attempted later, this S3 recovery should be used as the model:

- validate with vendor baseline first
- identify the official panel path
- only then layer HexHound on top

### For Cleanup

The repo still contains some experimental bring-up environments that were useful during recovery.

Those can be pruned later, but they should be kept until the vendor-app path has survived a little more real-world use.

## Bottom Line

What went right:

- the board was healthy
- serial and raw diagnostics isolated the failure correctly
- LilyGo's official path gave a trustworthy baseline

What went wrong:

- the original app display path was not reliable on the tested board
- backlight behavior was misunderstood
- GPIO 38 was being reset after backlight setup
- persistence originally had no working real-device backend when both LittleFS and SD assumptions failed

How it was fixed:

- moved the display stack to LilyGo's working `esp_lcd` ST7735 backend
- centralized backlight control
- matched the board's real backlight behavior
- fixed the boot sequence so the backlight stayed alive
- switched onboard persistence to SPIFFS
- auto-formatted flash storage on first mount failure
- selected the newest valid save across SPIFFS and SD
- verified real progress survived unplug/replug on hardware

That is the complete reason HexHound now works on the LilyGo T-Dongle S3.
