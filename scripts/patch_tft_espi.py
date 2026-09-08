"""
PlatformIO pre-build script: Auto-patch TFT_eSPI for ESP32-S3 REG_SPI_BASE bug.

The ESP-IDF SDK's REG_SPI_BASE(i) macro returns 0 for i<2, but the Arduino HAL
uses FSPI=0 / HSPI=1. This causes TFT_eSPI to dereference a null SPI register
base address, crashing with StoreProhibited on tft->init().

Fix: Insert '#undef REG_SPI_BASE' before TFT_eSPI's own definition so it can
replace the broken SDK macro with a correct one.

This script runs automatically before every build. It's idempotent - if the
patch is already present, it does nothing.
"""

Import("env")
import glob
import os


PATCH = (
    "  // [HexHound patch] SDK REG_SPI_BASE(i) returns 0 for i<2;\n"
    "  // Arduino HAL uses FSPI=0/HSPI=1. Undefine so TFT_eSPI can fix it.\n"
    "  #undef REG_SPI_BASE\n"
)
MARKER = "  #ifndef REG_SPI_BASE"


def patch_target_file(target_file):
    if not os.path.exists(target_file):
        return False

    with open(target_file, "r", encoding="utf-8") as f:
        content = f.read()

    if "#undef REG_SPI_BASE" in content:
        print("[patch_tft_espi] Already patched: " + target_file)
        return True

    marker = "  #ifndef REG_SPI_BASE"
    if marker not in content:
        print("[patch_tft_espi] WARNING: Could not find target line in " + target_file)
        print("[patch_tft_espi] The TFT_eSPI library version may have changed.")
        return False

    patched = content.replace(marker, PATCH + marker, 1)

    with open(target_file, "w", encoding="utf-8") as f:
        f.write(patched)

    print("[patch_tft_espi] PATCHED: " + target_file)
    return True


def patch_reg_spi_base(*args, **kwargs):
    project_libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    active_env = env.subst("$PIOENV")
    active_target = os.path.join(
        project_libdeps_dir, active_env, "TFT_eSPI", "Processors", "TFT_eSPI_ESP32_S3.h"
    )

    patched_any = patch_target_file(active_target)

    if patched_any:
        return

    pattern = os.path.join(
        project_libdeps_dir, "*", "TFT_eSPI", "Processors", "TFT_eSPI_ESP32_S3.h"
    )
    for target_file in glob.glob(pattern):
        patched_any = patch_target_file(target_file) or patched_any

    if not patched_any:
        print("[patch_tft_espi] File not found yet under: " + pattern)


# Register to run before library compilation and before linking
env.AddPreAction("buildprog", patch_reg_spi_base)
env.AddPreAction("$BUILD_DIR/lib", patch_reg_spi_base)
patch_reg_spi_base()
