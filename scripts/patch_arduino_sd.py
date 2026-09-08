"""
PlatformIO pre-build script: soften Arduino SD error spam for optional-card boots.

Some HexHound targets can run entirely from SPIFFS, with the SD slot acting as an
optional persistence backend. When no card is inserted, Arduino's sd_diskio.cpp
prints low-level log_e() messages before HexHound can report a friendly fallback.

Fix: patch those specific log_e() callsites so builds that define
HEXHOUND_QUIET_OPTIONAL_SD downgrade them to log_d() instead.
"""

Import("env")
import os


PATCH_SENTINEL = "HEXHOUND_QUIET_OPTIONAL_SD"


def patch_arduino_sd(*args, **kwargs):
    target_file = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"),
        "framework-arduinoespressif32",
        "libraries",
        "SD",
        "src",
        "sd_diskio.cpp",
    )

    if not os.path.exists(target_file):
        print("[patch_arduino_sd] File not found: " + target_file)
        return

    with open(target_file, "r", encoding="utf-8") as f:
        content = f.read()

    if PATCH_SENTINEL in content:
        print("[patch_arduino_sd] Already patched: " + target_file)
        return

    replacements = {
        '        log_e("Card Failed! cmd: 0x%02x", cmd);': (
            "        #ifdef HEXHOUND_QUIET_OPTIONAL_SD\n"
            '        log_d("Card Failed! cmd: 0x%02x", cmd);\n'
            "        #else\n"
            '        log_e("Card Failed! cmd: 0x%02x", cmd);\n'
            "        #endif"
        ),
        '        log_e("f_mount failed: %s", fferr2str[res]);': (
            "        #ifdef HEXHOUND_QUIET_OPTIONAL_SD\n"
            '        log_d("f_mount failed: %s", fferr2str[res]);\n'
            "        #else\n"
            '        log_e("f_mount failed: %s", fferr2str[res]);\n'
            "        #endif"
        ),
    }

    patched = content
    replaced_any = False
    for original, replacement in replacements.items():
        if original in patched:
            patched = patched.replace(original, replacement)
            replaced_any = True

    if not replaced_any:
        print("[patch_arduino_sd] WARNING: expected log lines not found in " + target_file)
        return

    with open(target_file, "w", encoding="utf-8") as f:
        f.write(patched)

    print("[patch_arduino_sd] PATCHED: " + target_file)


env.AddPreAction("buildprog", patch_arduino_sd)
env.AddPreAction("$BUILD_DIR/lib", patch_arduino_sd)
patch_arduino_sd()
