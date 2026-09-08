# HexHound Troubleshooting

First-boot and bring-up problems, with the fix for each. For the full T-Dongle
S3 display failure analysis see
[t-dongle-s3-build-report.md](t-dongle-s3-build-report.md); for the bring-up
checklist see [hardware_checklist.md](hardware_checklist.md).

## Flashing the wrong board image

**The single most common cause of a dead-looking board.** Every ESP32-S3 image
reports the same chip family, so nothing stops you flashing a T-Dongle image
onto a Waveshare board. The firmware boots perfectly and the serial log reads
`SETUP COMPLETE - ALL OK`, but the panel stays black because the backlight and
SPI pins belong to a different board.

Identify the board before flashing rather than by eye:

```bash
espefuse.py --port COMn summary | grep PSRAM_CAP
```

| Result | Meaning |
|--------|---------|
| No PSRAM reported | LilyGo T-Dongle S3 |
| `PSRAM_CAP = 8M` | A PSRAM board: T-Display S3, Waveshare 1.47B, or Touch 1.47 |
| Enumerates via CH343P, not native USB | Waveshare 1.28 round |

A completely dark panel with no backlight points at a board whose backlight is
GPIO 46 (either Waveshare 1.47) being flashed with a GPIO 38 image.

## First-boot issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Black screen, no backlight at all | Wrong board image flashed | Confirm the board as above, reflash the matching env |
| Black screen, but vendor baseline color-cycle works | Old display backend or backlight control issue | Use `lilygo-t-dongle-s3-vendor-app` |
| Backlight flashes then turns off | GPIO 38 backlight control conflict | Use the current vendor-app build; see the build report |
| Blank/white screen | Wrong TFT pins or driver | Run `lilygo-t-dongle-s3-vendor-baseline` first |
| Mirrored display | Orientation flipped for the current USB direction | Use the `FLIP DISPLAY` menu item, then let config save |
| Boots as Egg after every reboot | No readable persistence backend, or only a fresh save exists | Boot normally once so SPIFFS can initialize, then do a patrol and wait 5 seconds before unplugging |
| SPIFFS init failed | Uninitialized or corrupt onboard filesystem | Reboot once on the current build so it can auto-format SPIFFS |
| SD init fails | Wrong SPI pins, incompatible card, or no card present | Core persistence still works from SPIFFS; for SD, check the card is FAT32 or try another |
| No LED | DATA/CLK swapped | Swap GPIO 39 and 40 in `config.h` |
| HID doesn't type | CDC mode active | Flash the `t-dongle-s3-hid` environment |
| Brownout resets | Insufficient USB power | Use a powered hub or a shorter cable |

## Stuck in download mode after flashing

`esptool`'s reset can leave an ESP32-S3 in `boot:0x0 (DOWNLOAD(USB/UART0))`.
A power cycle clears it. Over serial, hold DTR low and pulse RTS only:

```python
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)
```

Asserting DTR and RTS together is what selects download mode in the first
place. A correct reset shows `boot:0x8 (SPI_FAST_FLASH_BOOT)`.

### The T-RGB does this after every flash

On the LilyGo T-RGB this is not intermittent - the board is silent after every
upload and needs a **manual reset**. Measured: after a verified flash, neither
esptool's `--after hard_reset` nor a host DTR-low/RTS-pulse brought it back, and
six freshly opened serial handles over about 40 seconds returned zero bytes.
Pressing reset, or unplugging and replugging, brings it up immediately.

Treat the reset as part of the flash procedure on that board, not as a fault.

### A silent port right after a reset is not always a dead board

The T-RGB uses the ESP32-S3's **native USB-Serial/JTAG**, so resetting the chip
drops the USB device and re-enumerates it. Any serial handle opened before the
reset is now stale and will read zero bytes forever, or fail with
`ClearCommError failed`. Wait for the port to disappear and come back, then open
a fresh handle. A boot log that starts at `[TRGB] RGB panel running` means the
app is fine and only your connection was stale.

## Touch stops responding on the T-RGB

Symptom: nothing on screen reacts to a finger, and the serial log fills with

```
[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1
```

repeating at the touch poll cadence. `TouchModule::update()` bails to
`_pressed = false` on every failed read, so no gesture can form and touch is
completely dead.

**Fix: a full power cycle** - unplug and replug, not the reset button.
`trgb::begin()` pulses the touch reset line through the XL9535 expander at cold
start, which re-resets the FT3267.

Observed once, on 2026-08-28: the controller was healthy 15 seconds into a boot
and failing by 30 seconds, with `millis()` climbing straight through, so the
board had not reset in between. The address still ACKed - it was the
repeated-start read that failed. **The root cause was never established.** If
you see it again, that is a real bug rather than a one-off, and worth capturing
in detail.

Note there is no recovery path in firmware today: once the controller stops
answering, `update()` never re-probes and never re-pulses the reset line, so
touch stays dead until the device is power-cycled. The display is unaffected,
which is a useful signal - the XL9535 expander shares that I2C bus, so a working
screen means the bus is healthy and the fault is the touch controller itself.

## `pio test -e native` reports ERRORED with no error

On Windows the test runner swallows the compiler's stderr, so a genuine
compile error appears as a bare `ERRORED` line with nothing to read. A fresh
`cc1plus` cannot find its runtime DLLs. Export the toolchain path **in the same
command**:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH" && pio test -e native
```

This hides easily, because only the suite whose source actually changed gets
recompiled - every cached suite still passes.

## Web flasher says "No port selected"

The browser flasher needs a visible, focused tab. If the tab is in the
background, Chrome silently discards the serial port chooser and ESP Web Tools
reports "No port selected". That is not a device fault. Bring the tab to the
foreground and click Install again.

Web Serial is Chrome/Edge desktop only, and the port chooser requires a real
click; it cannot be automated.

## Benign boot messages

| Message | Meaning |
|---------|---------|
| `E psram: PSRAM ID read error ... PSRAM chip not found` | Expected on boards without PSRAM wired, and on any PSRAM board built for the wrong line mode. Cosmetic. |
| `[Storage] No SD card detected - using SPIFFS only` | Normal on every board without an SD slot or card |
| `[USB] HID disabled (CDC mode)` | Expected on the recommended builds; HID missions need the `t-dongle-s3-hid` env |
