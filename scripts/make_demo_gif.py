#!/usr/bin/env python3
"""Build the README hero GIF from the desktop simulator.

Captures the real patrol loop headlessly and stitches it into one animation:
    scan (radar HUD) -> tracker alert -> patrol results + XP -> evolution

Every frame is a genuine simulator render, cropped to the real 320x172 panel
exactly the way scripts/capture_screens.py crops the doc screenshots.

Two constraints drive the shape of this script:
  * sim_autoCapture() in hal_sim.cpp holds a fixed shotTimes[16], so a run can
    request at most 16 marks. Longer sequences are chunked across runs, which
    is safe because the forced screens and the forced cutscene are
    deterministic from boot.
  * Boot takes ~1.6 s, so any mark before that captures the boot splash.
"""
import os, subprocess, glob, shutil, sys
from PIL import Image

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(PROJECT, ".pio", "build", "desktop-sim-wave", "program.exe" if sys.platform == "win32" else "program")
WORK = os.environ.get("HEXHOUND_GIF_WORK", os.path.join(PROJECT, ".pio", "gifwork"))
PANEL_W, PANEL_H, SCALE = 320, 172, 3   # matches PANELS in capture_screens.py
MAX_MARKS = 16                          # hal_sim.cpp shotTimes[16]
BOOT_MS = 1650                          # measured: home renders from ~1.6 s

def marks(start, stop, step):
    return list(range(start, stop, step))

# The cutscene is frame-paced (halTime().delay(16) per frame), not wall-clock,
# so its eight phases stretch to roughly 1.65 s -> 12.4 s of elapsed sim time
# rather than the nominal 6.4 s. Measured end points:
#   ~1.8s flash/dissolve   ~4-6s energy rings   ~6-8s name typewriter
#   ~8s sprite reveal      ~9-10s ability fanfare   ~11-12s LEVEL UP hold
# Past ~12.5 s it returns to HOME, which renders the pet as an Egg again and
# would contradict the evolution the GIF just showed. Stop before that.
# (label, env, marks, per-frame ms in the final GIF)
SEGMENTS = [
    ("hud",    {"HEXHOUND_SIM_FORCE_SCREEN": "hud",
                "HEXHOUND_SIM_HUD_STAGE": "4"},      marks(BOOT_MS, 3700, 250), 130),
    ("alert",  {"HEXHOUND_SIM_FORCE_SCREEN": "alert"}, [BOOT_MS, BOOT_MS+250],  700),
    ("patrol", {"HEXHOUND_SIM_FORCE_SCREEN": "patrol"},[BOOT_MS, BOOT_MS+250], 1000),
    ("evolve", {"HEXHOUND_SIM_FORCE_EVOLVE": "4,5"},  marks(BOOT_MS, 12450, 250), 100),
]

def chunks(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i:i+n]

def run_segment(label, extra, all_marks):
    # Wipe the whole tree, not just top-level *.bmp: frames land in per-chunk
    # subdirs, and leftovers from a previous run would silently pad the GIF
    # with stale frames (seen: 44 requested, 80 collected).
    outdir = os.path.join(WORK, "frames_" + label)
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)
    captured = []
    for ci, chunk in enumerate(chunks(all_marks, MAX_MARKS)):
        env = dict(os.environ)
        env.update(extra)
        sub = os.path.join(outdir, f"c{ci}")
        os.makedirs(sub, exist_ok=True)
        env["HEXHOUND_SIM_SHOT_DIR"] = sub
        env["HEXHOUND_SIM_SHOT_MS"] = ",".join(str(m) for m in chunk)
        env["HEXHOUND_SIM_QUIT_MS"] = str(max(chunk) + 300)
        subprocess.run([BIN], cwd=PROJECT, env=env,
                       capture_output=True, text=True, timeout=120)
        got = glob.glob(os.path.join(sub, "*.bmp"))
        captured += [(int(''.join(c for c in os.path.basename(p) if c.isdigit())), p)
                     for p in got]
    captured.sort()
    print(f"  {label:7s} requested {len(all_marks):3d}  captured {len(captured):3d}")
    return [p for _, p in captured]

def load(path):
    img = Image.open(path).convert("RGB")
    if img.width >= PANEL_W * SCALE and img.height >= PANEL_H * SCALE:
        img = img.crop((0, 0, PANEL_W * SCALE, PANEL_H * SCALE))
    return img.resize((PANEL_W * 2, PANEL_H * 2), Image.NEAREST)

def main():
    frames, durations = [], []
    for label, extra, m, per in SEGMENTS:
        paths = run_segment(label, extra, m)
        if not paths:
            print(f"  ABORT: segment '{label}' produced no frames")
            return 1
        for p in paths:
            frames.append(load(p))
            durations.append(per)
    frames.append(frames[-1]); durations.append(1100)   # hold the evolved pet

    out = os.path.join(PROJECT, "docs", "screenshots", "hexhound-demo.gif")
    pal = [f.quantize(colors=128, method=Image.MEDIANCUT, dither=Image.NONE)
           for f in frames]
    pal[0].save(out, save_all=True, append_images=pal[1:],
                duration=durations, loop=0, optimize=True, disposal=2)
    kb = os.path.getsize(out) / 1024
    total = sum(durations) / 1000
    print(f"\n  {len(frames)} frames -> {out}")
    print(f"  {frames[0].width}x{frames[0].height}  {kb:.0f} KB  {total:.1f}s loop")
    return 0

sys.exit(main())
