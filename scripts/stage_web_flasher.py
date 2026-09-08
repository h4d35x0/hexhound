#!/usr/bin/env python3
"""Stage the web flasher: copy merged firmware in, generate its manifests.

The page in web/ is committed, but the binaries are not - they are build output
(firmware/ is gitignored) and ~1.3 MB each. This script copies a built firmware
set into web/firmware/ and writes one ESP Web Tools manifest per board, so the
manifests can never drift from the binaries that are actually there.

Usage:
    python scripts/build_flashes.py 0.2.0-beta2      # build the images first
    python scripts/stage_web_flasher.py 0.2.0-beta2

Then serve it locally to test with real hardware:
    python -m http.server -d web 8000
    # open http://localhost:8000  (Web Serial needs HTTPS *or* localhost)
"""

import argparse
import hashlib
import json
import os
import shutil
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DIR = os.path.join(PROJECT_DIR, "web")

# basename in the firmware dir -> (board id, display name, chip family)
#
# chipFamily must match what esptool-js reports for the chip. ESP32-C5 support
# is recent; if the C5 button errors with an unsupported-chip message, that is
# the web tooling, not our image - fall back to esptool for that board.
#
# The display name is the ONLY thing that distinguishes one board from another
# in the picker: every ESP32-S3 image reports the same chipFamily, so ESP Web
# Tools cannot tell these boards apart and the picker is load-bearing rather
# than a convenience. A wrong pick writes a working image with another board's
# display pins, which boots perfectly and leaves the screen dark. Keep every
# name specific enough that a holder of the physical board cannot mis-pick -
# in particular "Waveshare 1.28 Round" and "LilyGo T-RGB 2.1in Round" are both
# round, so both carry their size and their vendor.
#
# KEEP THIS A 4-TUPLE. deploy_web_flasher.py imports BOARDS and unpacks it as
# `for _, board_id, _, _ in BOARDS`; widening it here breaks the deploy gate.
# Per-board extras go in POST_FLASH_NOTES below instead.
BOARDS = [
    ("t-dongle-s3-vendor-app", "t-dongle-s3", "LilyGo T-Dongle S3", "ESP32-S3"),
    ("t-dongle-s3-hid", "t-dongle-s3-hid", "LilyGo T-Dongle S3 (USB HID)", "ESP32-S3"),
    ("t-display-s3", "t-display-s3", "LilyGo T-Display S3", "ESP32-S3"),
    ("waveshare-esp32-s3-lcd-147b", "waveshare-147b", "Waveshare 1.47B", "ESP32-S3"),
    ("waveshare-esp32-s3-touch-lcd-147", "waveshare-touch-147", "Waveshare Touch 1.47", "ESP32-S3"),
    ("t-dongle-c5-vendor-app", "t-dongle-c5", "LilyGo T-Dongle C5", "ESP32-C5"),
    ("waveshare-esp32-s3-lcd-128", "waveshare-128-round", "Waveshare 1.28 Round", "ESP32-S3"),
    ("t-rgb", "t-rgb", "LilyGo T-RGB 2.1in Round", "ESP32-S3"),
]

# board id -> extra guidance the page shows for that board, before and after
# the flash. Optional; a board with no entry gets the page's generic text.
#
# Only add an entry where the board genuinely behaves differently, so that the
# presence of a note stays meaningful.
POST_FLASH_NOTES = {
    # The T-RGB parks in ROM download mode after every write and comes up with
    # a dark screen until it is reset by hand. That looks exactly like the
    # wrong-image failure this picker exists to prevent, so say so up front:
    # a tester who reads "dark screen" as "I picked wrong" will reflash, get
    # the same dark screen, and conclude the image is broken.
    "t-rgb": "The T-RGB stays in download mode after flashing and the screen "
             "stays dark until you reset it. That is normal for this board, "
             "not a failed flash. Press the RST button, or unplug and replug "
             "the USB cable, and it will boot.",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version", help="firmware version, e.g. 0.2.0-beta2")
    args = ap.parse_args()

    src_dir = os.path.join(PROJECT_DIR, "firmware", f"HexHound-Firmware-v{args.version}")
    if not os.path.isdir(src_dir):
        sys.exit(f"no such firmware build: {src_dir}\n"
                 f"run: python scripts/build_flashes.py {args.version}")

    fw_out = os.path.join(WEB_DIR, "firmware")
    man_out = os.path.join(WEB_DIR, "manifests")
    os.makedirs(fw_out, exist_ok=True)
    os.makedirs(man_out, exist_ok=True)

    staged, missing = [], []
    for basename, board_id, display, chip in BOARDS:
        src = os.path.join(src_dir, f"hexhound-{basename}-merged.bin")
        if not os.path.exists(src):
            missing.append(display)
            continue

        dst_name = f"hexhound-{board_id}.bin"
        shutil.copy2(src, os.path.join(fw_out, dst_name))

        # Our images are merged and flash at offset 0.
        #
        # Nothing here is sized or offset per board: one part, always offset 0,
        # whatever the image happens to be. That is what lets the T-RGB in -
        # its app partition is 6.25 MB (default_16MB.csv app0 = 0x640000) and
        # its merged image is over 4 MB, roughly three times the other boards'.
        # If you ever add a second part or a fixed offset table here, that
        # assumption stops holding and the big images break first.
        manifest = {
            "name": f"HexHound - {display}",
            "version": args.version,
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": chip,
                "parts": [{"path": f"../firmware/{dst_name}", "offset": 0}],
            }],
        }
        with open(os.path.join(man_out, f"{board_id}.json"), "w", newline="\n") as fh:
            json.dump(manifest, fh, indent=2)
            fh.write("\n")

        digest = hashlib.sha256(open(src, "rb").read()).hexdigest()
        entry = {
            "id": board_id, "name": display, "chip": chip,
            "bin": dst_name,
            "size": os.path.getsize(src),
            "sha256": digest,
        }
        if board_id in POST_FLASH_NOTES:
            entry["note"] = POST_FLASH_NOTES[board_id]
        staged.append(entry)
        print(f"  staged {display:32s} {os.path.getsize(src)/1024:.0f} KB  {digest[:12]}")

    # The page reads this to build its board picker, so adding a board to a
    # firmware build is enough - no HTML edit needed.
    with open(os.path.join(WEB_DIR, "boards.json"), "w", newline="\n") as fh:
        json.dump({"version": args.version, "boards": staged}, fh, indent=2)
        fh.write("\n")

    print(f"\n{len(staged)} board(s) staged into web/")
    for name in missing:
        print(f"  NOT STAGED (no image in this build): {name}")
    print("\nTest locally:  python -m http.server -d web 8000")
    print("Then open:     http://localhost:8000   (Chrome or Edge)")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
