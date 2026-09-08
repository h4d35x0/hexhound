#!/usr/bin/env python3
"""Build deterministic documentation screenshots from the desktop simulator."""

import glob
import os
import shutil
import subprocess
import sys


PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# desktop-sim is the Windows/MinGW env (hardcoded MSYS2 paths); everywhere else
# needs desktop-sim-linux, which resolves SDL2 via pkg-config. Override with
# HEXHOUND_SIM_ENV to capture from a different panel, e.g. desktop-sim-wave-linux.
SIM_ENV = os.environ.get(
    "HEXHOUND_SIM_ENV",
    "desktop-sim" if sys.platform == "win32" else "desktop-sim-linux",
)
BUILD_DIR = os.path.join(PROJECT_DIR, ".pio", "build", SIM_ENV)
BINARY = os.path.join(BUILD_DIR, "program")
BINARY_EXE = os.path.join(BUILD_DIR, "program.exe")
OUTPUT_DIR = os.environ.get(
    "HEXHOUND_SHOT_DIR", os.path.join(PROJECT_DIR, "docs", "screenshots"))

# Panel geometry per sim env: (width, height, window scale) - see SIM_SCALE in
# src/hal/hal_sim.cpp. Used to crop the square framebuffer down to the panel.
PANELS = {
    "desktop-sim": (160, 80, 4),
    "desktop-sim-linux": (160, 80, 4),
    "desktop-sim-wave": (320, 172, 3),
    "desktop-sim-wave-linux": (320, 172, 3),
    # The round panel is already square, so the crop is a no-op - but the entry
    # still matters: a missing key silently falls back to (None, None, 1) and
    # skips the crop, which would hide a real geometry mistake. The circular
    # bezel mask is applied by the simulator itself (hal_sim.cpp), not here, so
    # the PNGs show exactly what the glass shows.
    "desktop-sim-round": (240, 240, 3),
    "desktop-sim-round-linux": (240, 240, 3),
    # The 480 panel drops to 2x in hal_sim.cpp so the window still fits on a
    # laptop screen. The scale here MUST match SIM_SCALE there.
    "desktop-sim-round480": (480, 480, 2),
    "desktop-sim-round480-linux": (480, 480, 2),
}
EXPECTED_SHOTS = [
    "01_home_sentinel",
    "02_stats",
    "03_patrol_results",
    "04_menu",
    "05_journal",
    "06_evolution",
]


def clear_output_dir():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    for pattern in ("*.bmp", "*.png"):
        for path in glob.glob(os.path.join(OUTPUT_DIR, pattern)):
            os.remove(path)


def convert_to_png(bmp_path, png_path):
    try:
        from PIL import Image
    except ImportError:
        print("  [WARN] Pillow not installed - cannot convert screenshots")
        return False

    try:
        img = Image.open(bmp_path)
        # The simulator framebuffer is square (side = the panel's long edge) so
        # portrait screens fit too, which leaves dead black padding below a
        # landscape panel. Crop it off so the docs show the panel, not the
        # letterbox.
        panel_w, panel_h, scale = PANELS.get(SIM_ENV, (None, None, 1))
        if panel_w and img.width >= panel_w * scale and img.height >= panel_h * scale:
            img = img.crop((0, 0, panel_w * scale, panel_h * scale))
        img.save(png_path, "PNG")
        return True
    except Exception as exc:
        print(f"  [WARN] PNG conversion failed for {bmp_path}: {exc}")
        return False


def main():
    print("=== HexHound Screenshot Suite ===\n")
    binary_path = BINARY_EXE if os.path.exists(BINARY_EXE) else BINARY

    print(f"[1/4] Building {SIM_ENV}...")
    result = subprocess.run(
        ["pio", "run", "-e", SIM_ENV],
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return result.returncode
    print("  Build successful.")

    if not os.path.exists(binary_path):
        for alt in (BINARY_EXE, BINARY, os.path.join(BUILD_DIR, SIM_ENV)):
            if os.path.exists(alt):
                binary_path = alt
                break
        else:
            print(f"  Binary not found at {binary_path}")
            return 1

    print("\n[2/4] Rendering documentation screenshots...")
    clear_output_dir()
    result = subprocess.run(
        [binary_path, "--capture-docs", OUTPUT_DIR],
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return result.returncode
    if result.stdout.strip():
        print(result.stdout.strip())

    print("\n[3/4] Converting BMP captures to PNG...")
    converted = 0
    for bmp_path in sorted(glob.glob(os.path.join(OUTPUT_DIR, "*.bmp"))):
        name, _ = os.path.splitext(os.path.basename(bmp_path))
        png_path = os.path.join(OUTPUT_DIR, f"{name}.png")
        if convert_to_png(bmp_path, png_path):
            converted += 1
            os.remove(bmp_path)
            print(f"  {name}.png")
        else:
            shutil.copy2(bmp_path, os.path.join(OUTPUT_DIR, f"{name}.bmp"))

    print("\n[4/4] Validating expected screenshot set...")
    missing = [
        name for name in EXPECTED_SHOTS
        if not os.path.exists(os.path.join(OUTPUT_DIR, f"{name}.png"))
    ]
    if missing:
        print("  Missing:", ", ".join(missing))
        return 1

    print(f"\nScreenshot suite complete - {converted}/{len(EXPECTED_SHOTS)} rendered")
    print(f"PNG files saved to {OUTPUT_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
