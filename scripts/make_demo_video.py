#!/usr/bin/env python3
"""Record a HexHound demo video from the desktop simulator.

Produces a social-ready MP4 (and optionally a GIF) by driving the 480 round
simulator through a scripted tour, one scene per run, and compositing the round
panel onto a branded canvas with a caption.

    python scripts/make_demo_video.py                 # 1080x1920, Reels/Stories
    python scripts/make_demo_video.py --square        # 1080x1080, feed
    python scripts/make_demo_video.py --both
    python scripts/make_demo_video.py --gif           # also an animated GIF

Requires the simulator to be built first:

    pio run -e desktop-sim-round480

and ffmpeg on PATH.

── How the frames are captured ──────────────────────────────────────────────

The simulator already takes headless screenshots at a list of wall-clock marks
(HEXHOUND_SIM_SHOT_MS). One process per scene, because the forced-screen hook
picks a screen at boot and there is no way to navigate between screens without
a real input device.

Boot takes about 4.3 s, so every scene's marks start after that. That is also
why each scene costs a fresh process: the alternative is one long run that can
only ever show one screen.

The pet is dressed for every scene (HEXHOUND_SIM_OWN), because a demo of a
customisation feature that shows a bare pet is a demo of nothing.

── What this deliberately does NOT do ───────────────────────────────────────

It does not photograph the real board, and it does not pretend to. Colours on
an ST7701S panel behind a round bezel are not the colours in an SDL window,
and the simulator's timing is not the board's. This is a faithful recording of
the FIRMWARE, which is the honest thing to publish for a software demo; a
product shot of the hardware is a camera job.
"""

import argparse
import os
import shutil
import subprocess
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(PROJECT, ".pio", "build", "desktop-sim-round480", "program.exe")
if not os.path.exists(SIM):
    SIM = SIM[:-4]          # non-Windows
WORK = os.path.join(PROJECT, ".pio", "videowork")
OUT_DIR = PROJECT

BOOT_MS = 4600              # the splash is gone by here
FPS = 15
PANEL = 960                 # the sim canvas is the 480 panel at 2x

# Materials, so the Kit shows recipes that can actually be made rather than a
# wall of "short". A demo of crafting that shows nothing craftable demos
# nothing.
MATS = ("mat.scrap*12,mat.copper*8,mat.signal*8,mat.shard*6,"
        "mat.ferrite*5,mat.circuit*5,mat.lens*4,mat.glitch*3")

# A leading '-' grants an item without putting it on, which is what lets the
# closet scene show the hat going ON rather than starting with it worn.
UNWORN = "-cos.antenna,-cos.scarf"
DRESSED = "cos.antenna,cos.scarf"

# The flourish is GREMLIN-GATED and appears only in scenes at that stage or
# later. It was in every scene in the first cut, including on a Packet Pup -
# a state the game cannot produce, so the video was showing something no player
# could ever see. It also went unexplained: reported back as "what are the pink
# things circling him", which is the right question to ask of an effect that
# turns up with no caption and no way to have earned it.
FLOURISH = "cos.confetti"
DEN_SET = "cos.lamp,cos.rug,cos.poster,cos.crate"

# Scene: (screen, stage, seconds, caption, extra env)
#
# Timings are in the caption's own terms: each scene has to be long enough to
# READ the caption and then watch the thing it describes happen. The first cut
# ran seven scenes in twenty seconds and was reported as too fast, with the
# captions not lining up - three seconds is not enough to read a line and take
# in a screen, so the two never landed together.
#
# Presses are scripted against the scene's own clock (BOOT_MS + offset), so a
# scene can show the device being USED instead of being a still life.
def press(*offsets_kinds):
    return ",".join("%d:%s" % (BOOT_MS + off, kind) for off, kind in offsets_kinds)


SCENES = [
    ("home", 2, 4.5, "A cyber-pet that lives on the badge",
     {"HEXHOUND_SIM_OWN": ""}),

    ("inventory", 2, 5.0, "Patrol for parts, then craft",
     {"HEXHOUND_SIM_OWN": MATS}),

    ("closet", 2, 3.5, "Pick a slot",
     {"HEXHOUND_SIM_OWN": UNWORN,
      "HEXHOUND_SIM_PRESS": press((900, "s"))}),

    ("closet", 2, 4.0, "Put it on",
     {"HEXHOUND_SIM_OWN": DRESSED,
      "HEXHOUND_SIM_PRESS": press((900, "s"))}),

    ("den", 2, 4.5, "Furnish its den",
     {"HEXHOUND_SIM_OWN": DEN_SET + "," + DRESSED}),

    ("hud", 2, 5.5, "It scans the air around you",
     {"HEXHOUND_SIM_OWN": DRESSED}),

    ("patrol", 2, 5.0, "Real Wi-Fi and Bluetooth, real finds",
     {"HEXHOUND_SIM_OWN": DRESSED}),

    ("game", 3, 5.5, "Four minigames",
     {"HEXHOUND_SIM_OWN": DRESSED, "HEXHOUND_SIM_GAME": "0"}),

    ("game", 3, 5.5, "Allow it or block it",
     {"HEXHOUND_SIM_OWN": DRESSED, "HEXHOUND_SIM_GAME": "2"}),

    # 3 -> 4 so the finale is a Gremlin, which is the earliest stage that can
    # actually own the flourish shown in the last scene.
    (None, 3, 7.5, "Feed it, play with it, watch it evolve",
     {"HEXHOUND_SIM_OWN": DRESSED, "HEXHOUND_SIM_FORCE_EVOLVE": "3,4"}),

    ("home", 4, 5.0, "New forms unlock new effects",
     {"HEXHOUND_SIM_OWN": DRESSED + "," + FLOURISH}),

    # Missions are gated on Gremlin Mode, so these scenes MUST come after the
    # evolution scene above: at stage 3 menuItemVisible() hides the row and the
    # forced screen would render a list nobody can reach in that state. Stage 4
    # here is the same stage the cutscene just delivered, so the tour stays
    # honest about the order a real owner sees things in.
    ("missions", 4, 5.0, "Gremlin Mode unlocks security missions",
     {"HEXHOUND_SIM_OWN": DRESSED + "," + FLOURISH,
      "HEXHOUND_SIM_PRESS": press((900, "s"), (2100, "s"))}),

    ("missions", 4, 4.5, "The badge types them into a real computer",
     {"HEXHOUND_SIM_OWN": DRESSED + "," + FLOURISH,
      "HEXHOUND_SIM_PRESS": press((1000, "s"))}),

    ("home", 4, 4.0, "HexHound",
     {"HEXHOUND_SIM_OWN": DRESSED + "," + FLOURISH}),
]

BG = (10, 10, 14)
ACCENT = (0, 229, 255)
DIM = (150, 155, 165)


def run_scene(idx, screen, stage, seconds, env_extra):
    """One simulator process; returns the captured BMP paths in order."""
    shot_dir = os.path.join(WORK, "scene%02d" % idx)
    if os.path.isdir(shot_dir):
        shutil.rmtree(shot_dir)
    os.makedirs(shot_dir)

    n = int(seconds * FPS)
    step = 1000.0 / FPS
    marks = [int(BOOT_MS + i * step) for i in range(n)]

    env = dict(os.environ)
    for k in ("HEXHOUND_SIM_FORCE_SCREEN", "HEXHOUND_SIM_FORCE_EVOLVE",
              "HEXHOUND_SIM_GAME", "HEXHOUND_SIM_PRESS", "HEXHOUND_SIM_OWN"):
        env.pop(k, None)
    env.update({
        "HEXHOUND_SIM_STAGE": str(stage),
        "HEXHOUND_SIM_SHOT_DIR": shot_dir,
        "HEXHOUND_SIM_SHOT_MS": ",".join(str(m) for m in marks),
        "HEXHOUND_SIM_QUIT_MS": str(marks[-1] + 400),
    })
    if screen:
        env["HEXHOUND_SIM_FORCE_SCREEN"] = screen
    for k, v in env_extra.items():
        if v == "":
            env.pop(k, None)
        else:
            env[k] = v

    subprocess.run([SIM], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)

    shots = sorted(
        (f for f in os.listdir(shot_dir) if f.endswith(".bmp")),
        key=lambda f: int("".join(c for c in f if c.isdigit()) or 0))
    return [os.path.join(shot_dir, f) for f in shots]


def load_font(size):
    for name in ("consola.ttf", "cour.ttf", "DejaVuSansMono.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except Exception:
            continue
    return ImageFont.load_default()


def compose(src_bmp, size, caption, font, small):
    """Round panel centred on the canvas, caption under it."""
    w, h = size
    canvas = Image.new("RGB", size, BG)
    d = ImageDraw.Draw(canvas)

    panel_px = int(w * 0.82)
    panel = Image.open(src_bmp).convert("RGB").resize(
        (panel_px, panel_px), Image.LANCZOS)

    # The sim draws the round panel on a square canvas with the corners already
    # dark, so a circular mask only tidies the very edge - but it is what makes
    # the device read as round rather than as a square screenshot of a circle.
    mask = Image.new("L", (panel_px, panel_px), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, panel_px - 1, panel_px - 1], fill=255)

    # Centre the PANEL AND ITS CAPTION as one block, not the panel alone.
    # Centring the panel on its own left a third of a 9:16 frame as dead black
    # under the caption, which reads as a mistake rather than as space.
    cap_h = int(h * 0.10)
    block_h = panel_px + cap_h
    px = (w - panel_px) // 2
    py = (h - block_h) // 2
    if py < int(h * 0.06):
        py = int(h * 0.06)

    # A thin bezel ring, so the panel has an edge instead of floating.
    d.ellipse([px - 6, py - 6, px + panel_px + 5, py + panel_px + 5],
              outline=(46, 50, 58), width=6)
    canvas.paste(panel, (px, py), mask)

    cap_y = py + panel_px + int(cap_h * 0.35)
    tw = d.textlength(caption, font=font)
    d.text(((w - tw) / 2, cap_y), caption, font=font, fill=(235, 240, 245))

    # A wordmark at the top of a tall frame, where a Reel has room for one and
    # a square post does not.
    if h > w * 1.2:
        title = "HEXHOUND"
        tf = load_font(int(w * 0.075))
        tw3 = d.textlength(title, font=tf)
        d.text(((w - tw3) / 2, int(h * 0.055)), title, font=tf, fill=ACCENT)

    tag = "HexHound"
    tw2 = d.textlength(tag, font=small)
    d.text(((w - tw2) / 2, h - int(h * 0.055)), tag, font=small, fill=DIM)

    d.rectangle([0, h - 8, w, h], fill=ACCENT)
    return canvas


def build(size, label, frames_by_scene, gif=False):
    w, h = size
    font = load_font(int(w * 0.045))
    small = load_font(int(w * 0.026))

    stage_dir = os.path.join(WORK, "frames_%s" % label)
    if os.path.isdir(stage_dir):
        shutil.rmtree(stage_dir)
    os.makedirs(stage_dir)

    n = 0
    for (scene, caption) in frames_by_scene:
        for src in scene:
            compose(src, size, caption, font, small).save(
                os.path.join(stage_dir, "f%05d.png" % n))
            n += 1
    if n == 0:
        sys.exit("no frames captured; is the simulator built?")

    out = os.path.join(OUT_DIR, "hexhound-demo-%s.mp4" % label)
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", str(FPS),
        "-i", os.path.join(stage_dir, "f%05d.png"),
        "-c:v", "libx264", "-preset", "slow", "-crf", "20",
        # yuv420p and even dimensions, or half the players in the world show
        # a green frame or refuse the file outright.
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-r", "30",
        out,
    ], check=True)
    print("wrote %s  (%d frames, %.1f s)" % (out, n, n / float(FPS)))

    if gif:
        gif_out = os.path.join(OUT_DIR, "hexhound-demo-%s.gif" % label)
        pal = os.path.join(stage_dir, "palette.png")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate",
                        str(FPS), "-i", os.path.join(stage_dir, "f%05d.png"),
                        "-vf", "scale=640:-1:flags=lanczos,palettegen", pal],
                       check=True)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate",
                        str(FPS), "-i", os.path.join(stage_dir, "f%05d.png"),
                        "-i", pal, "-lavfi",
                        "scale=640:-1:flags=lanczos[x];[x][1:v]paletteuse",
                        gif_out], check=True)
        print("wrote " + gif_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--square", action="store_true", help="1080x1080 feed post")
    ap.add_argument("--both", action="store_true")
    ap.add_argument("--gif", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(SIM):
        sys.exit("simulator not built: pio run -e desktop-sim-round480")
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found on PATH")

    os.makedirs(WORK, exist_ok=True)

    frames_by_scene = []
    for i, (screen, stage, secs, caption, extra) in enumerate(SCENES):
        shots = run_scene(i, screen, stage, secs, extra)
        print("  scene %d %-10s %3d frames  %s"
              % (i, screen or "evolve", len(shots), caption))
        if shots:
            frames_by_scene.append((shots, caption))

    sizes = []
    if args.both:
        sizes = [((1080, 1920), "reels"), ((1080, 1080), "square")]
    elif args.square:
        sizes = [((1080, 1080), "square")]
    else:
        sizes = [((1080, 1920), "reels")]

    for size, label in sizes:
        build(size, label, frames_by_scene, gif=args.gif)


if __name__ == "__main__":
    main()
