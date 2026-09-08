"""Refuse to build the C5 targets in the shared PlatformIO home.

The C5 envs install their platform from pioarduino's git URL. pioarduino
publishes under the SAME package names as the stock PlatformIO platform, so
building it in the default `~/.platformio`:

  * overwrites the unpinned `espressif32` platform slot, and
  * strips `tools/sdk` out of the shared `framework-arduinoespressif32`,

after which EVERY ESP32-S3 target in this repo stops building with

    fatal error: freertos/FreeRTOS.h: No such file or directory

Nothing detects that at the time it happens - the C5 build succeeds and the
damage only surfaces the next time you build a different board. Repairing it
means a forced package reinstall.

PlatformIO's `core_dir` option is global, not per-env, so platformio.ini cannot
express "this one env gets its own package tree". The only lever is the
PLATFORMIO_CORE_DIR environment variable, which means the check has to live
here and fail loudly.

scripts/build_flashes.py sets this automatically; this guard is for anyone
running `pio run -e lilygo-t-dongle-c5-...` by hand.
"""

import os
import sys

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

if not env.subst("$PIOENV").startswith("lilygo-t-dongle-c5"):  # noqa: F821
    raise SystemExit

if not os.environ.get("PLATFORMIO_CORE_DIR"):
    # Suggest the SAME directory build_flashes.py uses, imported rather than
    # repeated. These two disagreed until 2026-08-03 - the guard said
    # ~/.platformio-c5 while the script used C:\c5 - so following the printed
    # advice built a SECOND ~4 GB package tree instead of reusing the one the
    # release script had already populated. See build_flashes.py for why the
    # Windows path is a drive-root directory (Windows MAX_PATH).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        from build_flashes import C5_CORE_DIR as suggested
    except Exception:
        suggested = "C:\\c5" if os.name == "nt" else os.path.join(
            os.path.expanduser("~"), ".platformio-c5")
    sys.stderr.write(f"""
================================================================================
REFUSING TO BUILD: the C5 target needs its own PlatformIO core directory.

This env installs the pioarduino platform, which publishes under the same
package names as the stock platform. Building it in the shared PlatformIO home
strips tools/sdk out of framework-arduinoespressif32, and every ESP32-S3 target
in this repo then fails with:

    fatal error: freertos/FreeRTOS.h: No such file or directory

Re-run with an isolated core directory (it is a large first download, ~4 GB,
and is reused after that):

    PLATFORMIO_CORE_DIR="{suggested}" pio run -e {env.subst("$PIOENV")}

  PowerShell:
    $env:PLATFORMIO_CORE_DIR="{suggested}"; pio run -e {env.subst("$PIOENV")}

Or build every board through the script that handles this for you:

    python scripts/build_flashes.py <version>

Keep the path SHORT and outside the project - a project-relative core dir blows
past Windows MAX_PATH while unpacking the RISC-V toolchain.
================================================================================
""")
    env.Exit(1)  # noqa: F821
