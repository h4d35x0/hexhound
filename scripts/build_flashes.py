#!/usr/bin/env python3
"""Build a flashable merged image for every board model, in one pass.

Produces firmware/HexHound-Firmware-<version>/ containing one merged .bin per
board, SHA256SUMS.txt and FLASHING.md - the artifact set that gets attached to
a GitHub release. `firmware/` is gitignored, so these never land in the repo.

Usage:
    python scripts/build_flashes.py 0.2.0-beta2
    python scripts/build_flashes.py 0.2.0-beta2 --only waveshare-esp32-s3-lcd-147b

Every model is built from current source, so the art, UI and fixes in the tree
are what ships. A model that fails to build is reported and does NOT silently
drop out of the package: the script exits non-zero and names it.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The C5 pulls its platform from pioarduino's git repo, which publishes under
# the same package names as the stock platform. Building it in the shared
# PlatformIO home strips tools/sdk out of framework-arduinoespressif32, and
# every ESP32-S3 env then dies with "fatal error: freertos/FreeRTOS.h: No such
# file or directory". Give that one model its own core directory so its
# packages cannot touch anyone else's.
# It lives next to the real PlatformIO home, NOT inside the project: the
# RISC-V toolchain nests deeply enough that a project-relative core dir blows
# past Windows' 260-char MAX_PATH and the install dies with
# "[WinError 3] The system cannot find the path specified: ...riscv32-esp-elf".
#
# UPDATE 2026-08-03: ~/.platformio-c5 is NO LONGER SHORT ENOUGH on Windows.
# pioarduino's develop branch now pulls esp_matter/connectedhomeip, whose
# headers nest to about 288 characters, e.g.
#
#   <core>/.cache/tmp/pkg-installing-XXXXXXXX/esp32-arduino-libs/esp32c6/
#   include/espressif__esp_matter/connectedhomeip/connectedhomeip/src/app/
#   clusters/camera-av-settings-user-level-management-server/
#   camera-av-settings-user-level-management-server.h
#
# A home-directory prefix such as C:\Users\<you>\.platformio-c5 is around 40
# characters and pushes that past MAX_PATH, so the install dies with a bare
# FileNotFoundError naming a path that is plainly right there. Windows long
# paths are off by default (LongPathsEnabled = 0) and turning them on
# needs admin, so the fix is to spend fewer characters on the prefix. A
# drive-root directory costs 5 instead of 40 and buys back 35.
#
# This is ugly and it is deliberate. Do not "tidy" it back under the home
# directory without checking MAX_PATH first.
if os.name == "nt":
    C5_CORE_DIR = "C:\\c5"
else:
    C5_CORE_DIR = os.path.join(os.path.expanduser("~"), ".platformio-c5")

# (pio env, output basename, human name, isolated core dir or None).
MODELS = [
    ("lilygo-t-dongle-s3-vendor-app", "t-dongle-s3-vendor-app", "LilyGo T-Dongle S3", None),
    ("t-dongle-s3-hid", "t-dongle-s3-hid", "LilyGo T-Dongle S3 (USB HID mode)", None),
    ("lilygo-t-dongle-c5-vendor-app", "t-dongle-c5-vendor-app", "LilyGo T-Dongle C5", C5_CORE_DIR),
    ("lilygo-t-display-s3", "t-display-s3", "LilyGo T-Display S3", None),
    ("waveshare-esp32-s3-lcd-147b", "waveshare-esp32-s3-lcd-147b", "Waveshare ESP32-S3 LCD 1.47B", None),
    ("waveshare-esp32-s3-touch-lcd-147", "waveshare-esp32-s3-touch-lcd-147", "Waveshare ESP32-S3 Touch LCD 1.47", None),
    ("waveshare-esp32-s3-lcd-128", "waveshare-esp32-s3-lcd-128", "Waveshare ESP32-S3 LCD 1.28 (round)", None),
    ("lilygo-t-rgb", "t-rgb", "LilyGo T-RGB 2.1in Round", None),
]


def env_for(core_dir):
    """Environment for a pio invocation, isolating the core dir when asked."""
    environ = dict(os.environ)
    if core_dir:
        environ["PLATFORMIO_CORE_DIR"] = core_dir
    # pioarduino's idf_tools.py refuses to run when it thinks it is inside
    # MSys/Mingw ("ERROR: MSys/Mingw is not supported"). Its check is literally
    # `if "MSYSTEM" in os.environ`, so removing the variable is the fix.
    #
    # BUT THIS DOES NOT SAVE YOU FROM GIT BASH, and an earlier version of this
    # comment claimed it did. Measured 2026-09-04: the MSys runtime re-injects
    # MSYSTEM into every child process it spawns, so it comes back regardless.
    # Even `env -u MSYSTEM python -c "import os; print(os.environ['MSYSTEM'])"`
    # prints MINGW64. Popping it here only works when this script was started
    # from a NATIVE Windows shell, cmd or PowerShell.
    #
    # Run the C5 build from PowerShell. From Git Bash the failure surfaces as a
    # compile error on an unrelated framework file (FS.cpp.o), three layers away
    # from the cause.
    environ.pop("MSYSTEM", None)

    # Use the OS certificate store rather than uv's bundled one.
    #
    # The C5 platform installs its Python dependencies with `uv`, which ships
    # its own CA bundle and therefore fails behind any TLS-intercepting proxy
    # or inspecting AV with "invalid peer certificate: UnknownIssuer". The
    # platform runs that install with stdout and stderr sent to DEVNULL, so all
    # you see is "Failed to install Python dependencies into penv" with no
    # cause, which is a genuinely miserable thing to debug.
    #
    # Harmless when nothing is intercepting: it just reads the OS trust store.
    #
    # Note this does NOT cover Python's own requests calls, which use certifi.
    # If those fail the same way, append the OS root store to the certifi
    # bundle in the relevant penv; see docs/build-environments.md.
    environ.setdefault("UV_NATIVE_TLS", "1")
    return environ


def run(cmd, core_dir=None, **kw):
    print("  $ " + " ".join(cmd))
    return subprocess.run(cmd, cwd=PROJECT_DIR, env=env_for(core_dir), **kw)


def build(env, core_dir=None):
    return run(["pio", "run", "-e", env], core_dir=core_dir).returncode == 0


def flash_layout(env, core_dir=None):
    """Ask PlatformIO for this env's flash layout.

    There is no `mergebin` target on espressif32 6.x, and the offsets are not
    the same on every chip (the C5 bootloader does not live at 0x0 like the
    S3's), so the layout is read from the build environment rather than
    hardcoded. Returns (mcu, [(offset, path), ...]) or (None, None).
    """
    result = run(["pio", "run", "-e", env, "-t", "envdump"],
                 core_dir=core_dir, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        return None, None

    dump = result.stdout
    mcu_match = re.search(r"'BOARD_MCU': '([^']+)'", dump)
    app_match = re.search(r"'ESP32_APP_OFFSET': '([^']+)'", dump)
    extra_match = re.search(r"'FLASH_EXTRA_IMAGES': \[(.*?)\]", dump, re.S)
    if not (mcu_match and app_match and extra_match):
        print("  could not read flash layout from envdump")
        return None, None

    images = []
    for offset, path in re.findall(r"\(\s*'(0x[0-9a-fA-F]+)',\s*'(.*?)'\s*\)",
                                   extra_match.group(1), re.S):
        images.append((offset, path.replace("\\\\", "\\")))

    app = os.path.join(PROJECT_DIR, ".pio", "build", env, "firmware.bin")
    images.append((app_match.group(1), app))
    images.sort(key=lambda item: int(item[0], 16))
    return mcu_match.group(1), images


def esptool_path(core_dir=None):
    home = core_dir or os.path.join(os.path.expanduser("~"), ".platformio")
    candidate = os.path.join(home, "packages", "tool-esptoolpy", "esptool.py")
    return candidate if os.path.exists(candidate) else None


def merge(env, out_path, core_dir=None):
    """Produce one image flashable at offset 0.

    The pioarduino platform (C5) already writes a combined `firmware.factory.bin`
    with the right per-chip offsets - the C5 bootloader sits at 0x2000, not 0x0 -
    so prefer that over re-deriving the layout. Its envdump also does not expose
    FLASH_EXTRA_IMAGES in the stock format, so the fallback path cannot read it.
    """
    factory = os.path.join(PROJECT_DIR, ".pio", "build", env, "firmware.factory.bin")
    if os.path.exists(factory):
        shutil.copy2(factory, out_path)
        print("  used the platform's own firmware.factory.bin")
        return True

    mcu, images = flash_layout(env, core_dir)
    if not images:
        return False
    for _, path in images:
        if not os.path.exists(path):
            print(f"  missing image component: {path}")
            return False

    tool = esptool_path(core_dir)
    if not tool:
        print("  esptool.py not found under ~/.platformio/packages/tool-esptoolpy")
        return False

    # esptool 5.x renamed the subcommand and flags, and defaults merge-bin to
    # Intel hex output - which would produce a file that is not flashable as a
    # raw image. The C5's isolated core ships esptool 5.3.0 while the shared one
    # is on 4.9, so try the modern spelling first and fall back to the old one.
    attempts = [
        [sys.executable, tool, "--chip", mcu, "merge-bin", "--format", "raw",
         "-o", out_path, "--flash-size", "keep"],
        [sys.executable, tool, "--chip", mcu, "merge_bin",
         "-o", out_path, "--flash_size", "keep"],
    ]
    last = None
    for cmd in attempts:
        full = list(cmd)
        for offset, path in images:
            full += [offset, path]
        last = run(full, core_dir=core_dir, capture_output=True, text=True)
        if last.returncode == 0 and os.path.exists(out_path):
            return True
    if last is not None:
        print(last.stdout[-2000:])
        print(last.stderr[-2000:])
    return False


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version", help="release version, e.g. 0.2.0-beta2")
    ap.add_argument("--only", action="append", default=None,
                    help="build just this pio env (repeatable)")
    args = ap.parse_args()

    models = MODELS
    if args.only:
        models = [m for m in MODELS if m[0] in args.only]
        if not models:
            sys.exit(f"no model matches {args.only}")

    out_dir = os.path.join(PROJECT_DIR, "firmware", f"HexHound-Firmware-v{args.version}")
    os.makedirs(out_dir, exist_ok=True)

    built, failed = [], []
    for env, basename, human, core_dir in models:
        print(f"\n=== {human}  [{env}] ===")
        if core_dir:
            print(f"  (isolated PlatformIO core dir: {core_dir})")
        out_path = os.path.join(out_dir, f"hexhound-{basename}-merged.bin")
        if build(env, core_dir) and merge(env, out_path, core_dir):
            size = os.path.getsize(out_path) / 1024.0
            print(f"  OK  {os.path.basename(out_path)}  ({size:.0f} KB)")
            built.append((basename, human, out_path))
        else:
            print(f"  FAILED  {env}")
            failed.append((env, human))

    if built:
        with open(os.path.join(out_dir, "SHA256SUMS.txt"), "w", newline="\n") as fh:
            for _, _, path in sorted(built, key=lambda b: b[0]):
                fh.write(f"{sha256(path)}  {os.path.basename(path)}\n")

        with open(os.path.join(out_dir, "FLASHING.md"), "w", newline="\n") as fh:
            fh.write(f"# Flashing HexHound v{args.version}\n\n")
            fh.write("These are merged images. Flash them at offset `0x0`.\n\n")
            fh.write("```bash\n")
            fh.write("esptool.py --chip esp32s3 --port <PORT> --baud 921600 "
                     "write_flash 0x0 <IMAGE>.bin\n")
            fh.write("```\n\n")
            fh.write("The T-Dongle C5 is an ESP32-C5, so use `--chip esp32c5` for that "
                     "image.\n\nBoard images:\n\n")
            for basename, human, path in built:
                fh.write(f"- `{os.path.basename(path)}` - {human}\n")
            fh.write("\nAfter flashing, power-cycle the board (unplug and replug USB). "
                     "An esptool reset alone can leave it in download mode.\n")

    print(f"\n=== {len(built)} built, {len(failed)} failed ===")
    print(f"Output: {out_dir}")
    for env, human in failed:
        print(f"  FAILED: {human} [{env}]")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
