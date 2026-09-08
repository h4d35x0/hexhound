# HexHound Tester Guide

**Thank you for testing HexHound!**

HexHound is a gamified cybersecurity recon pet that runs on compact ESP32-S3 boards with screens, including the LilyGo T-Dongle S3 and the Waveshare 1.47-inch variants. The pet evolves by performing real Wi-Fi and BLE reconnaissance and can deliver security awareness "missions" via USB HID keyboard emulation.

This guide helps you test the experience quickly and give useful feedback.

---

## Quick Start (No Hardware - 5 Minutes)

You can experience almost the entire product using the desktop simulator.

### 1. Get the Code

**Option A (preferred):** Clone the repo
```bash
git clone https://github.com/h4d35x0/hexhound.git
cd hexhound
```

**Option B:** Use the provided tester ZIP and extract it.

### 2. Install PlatformIO (if you don't have it)

- Recommended: [VS Code](https://code.visualstudio.com/) + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Or install the CLI: `pip install platformio`

### 3. Run the Unit Tests First (No Extra Dependencies)

This verifies the core pet logic, rules, storage, and event system:

```bash
pio test -e native
```

All tests should pass. If they don't, note the failures.

### 4. Build and Run the Simulator (or use prebuilt on Windows)

If you received the **HexHound-Tester-Package** ZIP:

- On **Windows**: Look inside `prebuilt/windows/`. 
  - Run `HexHound-Simulator.exe` (keep `SDL2.dll` next to it).
  - See `prebuilt/windows/README.txt` for quick notes.

- On **Linux**: Look inside `prebuilt/linux/` (packages built after 2026-07-28;
  the v0.2.0-beta1 ZIP is Windows-only).
  - Run `./run-simulator.sh` (it points the loader at the bundled `lib/`).
  - See `prebuilt/linux/README.txt` for quick notes.

- On all platforms: You can also build from source (recommended for latest code).

See the **Operating System Specific Instructions** section below for full details.

**Simulator Controls**

| Key       | Action                                      |
|-----------|---------------------------------------------|
| `SPACE`   | Short press (start patrol, scroll, dismiss) |
| `ENTER`   | Long press (open menu, select item, back)   |
| `S`       | Save screenshot (to current dir or /tmp)    |
| `Q`       | Quit the simulator                          |

The simulator automatically injects realistic fake scan data (8 Wi-Fi networks with duplicates/open APs + 5 BLE devices including a tracker candidate).

---

## Operating System Specific Instructions

### Windows

Windows requires MinGW (via MSYS2) + SDL2 because the simulator uses native compilation.

**One-time setup:**

1. Download and install **MSYS2** from https://www.msys2.org/
2. Open the **MSYS2 MINGW64** terminal (important - not the default MSYS one).
3. Update and install the required packages:
   ```bash
   pacman -Syu
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2
   ```
4. Add the MinGW bin folder to your Windows **PATH**:
   - `C:\msys64\mingw64\bin`
   - You may need to restart your terminal / VS Code after changing PATH.
5. Verify in a new terminal:
   ```powershell
   gcc --version
   # Should show MinGW, not MSVC
   ```

**Build and run the simulator:**

From the project directory (in PowerShell or the MINGW64 terminal):

```powershell
pio run -e desktop-sim
# Then launch the built program
.pio\build\desktop-sim\program.exe
# or sometimes just:
.pio\build\desktop-sim\program
```

**Tips for Windows:**
- If `pio` complains about no compiler, make sure `gcc` from MSYS2 is first in PATH.
- Line ending warnings (CRLF) are normal on Windows and do not affect the build.
- The first build will download dependencies and can take 1–3 minutes.

### macOS

```bash
# Install SDL2 and pkg-config
brew install sdl2 pkg-config

# Build (the Linux env works here too: it resolves SDL2 via pkg-config,
# which is the same on macOS. Untested on macOS as of v0.2.0-beta1.)
pio run -e desktop-sim-linux

# Run
.pio/build/desktop-sim-linux/program
```

If you don't have Homebrew: https://brew.sh/

### Linux (Kali / Ubuntu / Debian / Pop!_OS etc.)

Use the `desktop-sim-linux` env, not `desktop-sim`. The Windows env hardcodes
MSYS2 paths; the Linux env finds SDL2 through `pkg-config`.

```bash
# Install the toolchain and SDL2 development headers
sudo apt update
sudo apt install build-essential libsdl2-dev pkg-config

# Build
pio run -e desktop-sim-linux

# Run
.pio/build/desktop-sim-linux/program
```

Other distros: install the equivalent `libsdl2-dev` / `SDL2-devel` package plus
`pkgconf`. Verify with `pkg-config --modversion sdl2` before building.

For the 320x172 Waveshare panel layout instead of the 160x80 T-Dongle one:

```bash
pio run -e desktop-sim-wave-linux
.pio/build/desktop-sim-wave-linux/program
```

For the 240x240 round Waveshare 1.28 panel (the keychain form factor). The sim
blanks everything outside the circle, so what you see is what the bezel shows:

```bash
pio run -e desktop-sim-round-linux
.pio/build/desktop-sim-round-linux/program
```

**Making a copyable package (no toolchain needed on the target machine):**

```bash
./scripts/package_sim_linux.sh
# -> dist/HexHound-Simulator-Linux/ and dist/HexHound-Simulator-Linux.tar.gz
```

The package bundles SDL2 and a `run-simulator.sh` launcher. libstdc++ and libgcc
are linked statically, so the only requirements on the target are glibc and a
graphical session. Build it on the oldest glibc you need to support: a binary
built on a newer glibc will not start on an older one.

**Headless check (no window, e.g. over SSH):**

```bash
SDL_VIDEODRIVER=dummy HEXHOUND_SIM_QUIT_MS=3000 \
  .pio/build/desktop-sim-linux/program
```

---

## What to Test & Play With

Try to exercise these areas and note your impressions:

1. **Onboarding / Hatching**
   - Start as Egg
   - Perform several short-press patrols + interactions
   - Watch for the evolution cinematic when it hatches to Packet Pup

2. **Patrol Experience**
   - Short press on home screen
   - Live HUD (if visible) then results screen
   - XP, hunger, mood, trust changes
   - Duplicate SSIDs and open networks flagged

3. **Menu & Stats**
   - Long press to open menu
   - View stats, journal, config
   - Note any confusing labels or flows

4. **Missions (Gremlin Mode)**
   - Reach Stage 4 (or force via enough activity)
   - Try the different missions
   - In simulator they output to console instead of real HID

5. **Journal**
   - Does it record patrols nicely?
   - Scrolling and readability

6. **Evolution & Personality**
   - Does the cutscene feel good?
   - Do traits make sense?

7. **Decay & Care**
   - Leave it idle for a bit - do stats decay reasonably?
   - Does it feel like it "needs" interaction?

8. **General Polish**
   - Button response feel (simulated)
   - Text legibility
   - Any crashes, hangs, or weird graphics?

**Pro tip:** Use the `S` key to capture screenshots of interesting states and attach them in feedback.

---

## What the Simulator Does NOT Cover

- Real Wi-Fi or BLE scanning (hardware radio behavior)
- Actual USB HID typing on a host machine
- Physical button feel, LED behavior, battery, SD card
- Display timing / backlight / rotation issues on real T-Dongle S3
- Power-on diagnostics (hold button at boot)
- Persistence across real power cycles + SD mirroring

For full validation of the recon + HID demo value, real hardware is required.

---

## Testing on Real Hardware (Optional but Valuable)

### Hardware Needed
- LilyGo T-Dongle S3 (~$15-22)
  - AliExpress (cheapest): https://www.aliexpress.com/item/1005004459426498.html
  - Amazon or lilygo.cc (faster)
- Or a supported Waveshare 1.47-inch ESP32-S3 board
- Optional: FAT32 MicroSD card (8GB+), mainly for T-Dongle SD-path validation

### Recommended Flash Commands

**Full application (current known-good path):**
```bash
pio run -e lilygo-t-dongle-s3-vendor-app -t upload
```

**Waveshare standard LCD:**
```bash
pio run -e waveshare-esp32-s3-lcd-147b -t upload
```

**Waveshare capacitive touch LCD:**
```bash
pio run -e waveshare-esp32-s3-touch-lcd-147 -t upload
```

**Waveshare 1.28-inch round LCD (non-touch):**
```bash
pio run -e waveshare-esp32-s3-lcd-128 -t upload
```
Serial on this board comes out of the CH343P USB-UART bridge, not a native USB
CDC port. Input is the BOOT button plus IMU gestures: tilt to scroll, shake to
select. Do not flash this image to the ESP32-S3-**Touch**-LCD-1.28, which is a
different board with different display pins.

**LilyGo T-RGB (480x480 round, capacitive touch):**
```bash
pio run -e lilygo-t-rgb -t upload
```
This board **parks in the ROM download mode after every flash** and is silent
until you press reset or replug it. That is expected, not a failed upload - the
flash itself verifies its hash before that point.

It is the only round board with native USB, so it is the only one that can reach
the USB HID missions screen. Its SD slot is SDMMC and is not implemented.

Touch works as an input source, so the button still does everything it always
did. On a touch panel: tap a menu row to open it; inside a list, tap a row to
put the cursor there and hold it to open it; tap the bottom strip to go back.
A tap on a list row deliberately does not open it, because those rows craft
items and spend the day's only reroll.

**Board validation only (color cycle test):**
```bash
pio run -e lilygo-t-dongle-s3-vendor-baseline -t upload
```

**For real HID missions (disables serial console):**
```bash
pio run -e t-dongle-s3-hid -t upload
```

**Diagnostics:** Hold the button while powering on the board.

See `docs/hardware_checklist.md` and `docs/t-dongle-s3-build-report.md` for detailed bring-up notes.

**Warning:** HID missions type directly on the connected computer. Only plug the dongle into machines you own or have explicit authorization to test.

---

## Responsible Use

HexHound is an **educational and security awareness tool**.

- Only scan networks and devices you own or have permission to test.
- Only use USB HID missions on systems you control.
- Unauthorized use may violate computer crime laws (CFAA, etc.).

Full policy: see `SECURITY.md` in the repository.

---

## Giving Feedback

Please tell us:

- What felt great or surprising
- What was confusing or frustrating
- Any crashes or visual bugs (attach simulator screenshots)
- Specific feature thoughts (missions, evolution pacing, patrol results clarity, menu organization)
- Hardware-specific observations if you tested on a real T-Dongle S3

**Preferred formats:**
1. GitHub Issue (use the Bug Report or Hardware Variant template in `.github/ISSUE_TEMPLATE/`)
2. Open a GitHub Issue with:
   - OS + how you tested (sim vs hardware)
   - Steps to reproduce
   - Expected vs actual behavior
   - Screenshots / console output

Even "it feels a bit slow to hatch" or "the mission text is too long" is valuable feedback.

---

## Current Known Areas of Interest

- Evolution thresholds and "hatch" requirements
- BLE tracker detection heuristics
- Mission text content and mischief impact
- Long-term stat decay balance
- Display readability on the tiny 80x160 screen
- First-boot SPIFFS / persistence behavior

Thank you again - your time helps make this a better security awareness toy.

- The HexHound team
