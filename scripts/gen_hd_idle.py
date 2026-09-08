#!/usr/bin/env python3
"""Bake the HD idle animation for the 480x480 round panel.

WHY THIS EXISTS
---------------
On the T-RGB the home screen shows a 176x176 HD portrait while the pet is at
rest, and the moment any animation is selected UIHome::drawRoundPet() hands the
slot to the pixel animator, which draws a 16x16 sprite at scale 8. That cliff
between the resting image and a moving one is the most visible quality drop on
the board. It exists because the HD set has exactly one image per stage: there
were no motion frames to play.

This produces them for the one animation the keeper actually spends their time
looking at - IDLE. The other seven animations still fall back to the pixel art;
all 36 frames at 176x176 would be 2.13 MB, and idle is where the eye lives.

HOW THE FRAMES ARE MADE
-----------------------
Not redrawn. Derived, by transform, from the same 400px stage masters that the
resting portrait is already cut from (assets/hd/asset_<stage>_400.png, out of
the 960x960 supersampled canvas in the sibling project's sprite generator). An
idle animation that drifts off-model is worse than a pixel one, so the frames
are guaranteed on-model by construction: the only things that change are the
creature's height and the intensity of its own neon.

Two frames, a breath, symmetric about the master pose:

  frame 0  EXHALE   vertical scale 1-BREATH about the base of the art,
                    neon accents dimmed by GLOW_DOWN
  frame 1  INHALE   vertical scale 1+BREATH about the base,
                    neon accents brightened by GLOW_UP

Anchoring the scale at the bottom of the alpha bounding box is what makes this
read as breathing rather than as a zoom: the feet stay planted and displacement
grows with height, so the head moves most. The anchor is taken from each
stage's own bbox, so nothing here is hand-tuned per creature and adding a sixth
stage needs no new numbers.

The neon pulse modulates brightness through a FEATHERED mask of the saturated
accent pixels, which covers the accents and the bloom halo the master already
carries. It touches RGB only and never alpha, so the silhouette of a frame is
bit-for-bit the silhouette the geometry gave it - the glow cannot grow the
sprite into the header text above it.

WHY THE FRAMES ARE OPAQUE
-------------------------
sprites_hd_round480.h uses the 0xF81F chroma key, because drawSprite() skips
those pixels and the resting portrait is drawn exactly once. An animation is
different: two frames whose silhouettes differ by a few pixels would leave a
fringe of the previous frame every time they alternate, and the only ways out
are to clear the slot between frames (which flashes, twice a second, forever)
or to write every pixel. So the idle frames composite the transparent
background to black instead of to the chroma key. The pet slot is cleared to
COL_BG - black - before anything is drawn in it, so an opaque black background
is the pixel that is already there; the frames simply overwrite each other with
no clear and no ghost.

COST
----
5 stages x 2 frames x 176 x 176 x 2 B = 619,520 B of flash, against ~4.0 MB
free in the 6,553,600 B app partition.

Usage:
    pip install Pillow
    python scripts/gen_hd_idle.py                  # bake the header
    python scripts/gen_hd_idle.py --preview <dir>  # also drop the pose PNGs there
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageChops, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_DIR = os.path.join(PROJECT_DIR, "assets", "hd")
OUT_DIR = os.path.join(PROJECT_DIR, "src", "ui")

# Stage order must match PetStage in src/pet/pet_core.h.
STAGES = ["egg", "pup", "beast", "gremlin", "sentinel"]

SRC_PX = 400   # master edge; the 480 panel's art is cut from the 400px set
OUT_PX = 176   # HD_HOME_SRC_PX on the 480 round panel

# Breath depth as a fraction of the creature's own height. The two frames sit
# either side of the master, so the head travels 2*BREATH of the art's height -
# measured at 5-6 px of the 176 px frame. Placing the frames either side of the
# master rather than running 0..2*BREATH from it is what buys that: the total
# travel is the same but neither frame reaches as far off the master, and the
# ceiling here is a hard one. Beacon Beast's antenna glow starts 8 px from the
# top of its 400 px master, so an inhale past about 1.023 pushes it off the
# canvas; breathe() refuses to write a frame that does. 0.020 is that ceiling
# with a margin, and it is the whole budget the layout allows - a bigger breath
# needs a taller slot, not a bigger number.
BREATH = 0.020
# Neon swing. Asymmetric on purpose, and this is measurement rather than taste:
# the accent CORES are already at or near RGB565 saturation in the master, so
# brightening them is mostly clipped away - pushing the up-swing from 0.12 to
# 0.26 moved 22 of 30976 words. The visible half of the pulse therefore has to
# come from the exhale being dimmer, which is why the frames dim by 0.30 and
# brighten by only 0.18. The up-swing still earns its place: it lifts the bloom
# HALO, which is not saturated and does have room to move.
GLOW_DOWN = 0.30
GLOW_UP = 0.18
# Feather radius (on the 400px master) of the accent mask. Roughly the bloom
# radius the master art already carries, so the pulse lifts the existing halo
# rather than drawing a hard-edged ring around each accent.
GLOW_FEATHER = 7.0

# An accent is a pixel that is both strongly coloured and bright. The bodies are
# charcoal (R~G~B) and the rim lights and fangs are near-white, so both fall
# below the saturation floor and stay out of the pulse; the cyan eyes, amber
# vents, green grin, red chevrons and blue visor are all far above it.
SAT_FLOOR = 100
VAL_FLOOR = 110
ALPHA_FLOOR = 40

TRANSPARENT_COLOR = 0xF81F  # only used to keep rgb565() honest; not emitted here
BG = (0, 0, 0)              # UI background, and what the frames composite onto


def source_png(stage):
    return os.path.join(ASSET_DIR, f"asset_{stage}_{SRC_PX}.png")


def accent_feather(img):
    """Feathered mask of the creature's neon, for the glow pulse."""
    _, s, v = img.convert("RGB").convert("HSV").split()
    alpha = img.split()[3]
    mask = ImageChops.multiply(
        s.point(lambda p: 255 if p >= SAT_FLOOR else 0),
        v.point(lambda p: 255 if p >= VAL_FLOOR else 0),
    )
    mask = ImageChops.multiply(
        mask, alpha.point(lambda p: 255 if p >= ALPHA_FLOOR else 0)
    )
    return mask.filter(ImageFilter.GaussianBlur(radius=GLOW_FEATHER))


def pulse_glow(img, gain):
    """Scale accent brightness by `gain`, weighted by the feathered mask.

    RGB only. Alpha is returned untouched, which is what guarantees the pulse
    cannot change the silhouette.
    """
    if gain == 1.0:
        return img
    rgb = img.convert("RGB")
    lit = rgb.point(lambda p: min(255, int(p * gain + 0.5)))
    out = Image.composite(lit, rgb, accent_feather(img))
    out = out.convert("RGBA")
    out.putalpha(img.split()[3])
    return out


def breathe(img, scale):
    """Vertical scale about the bottom of the art's alpha bounding box.

    PIL's AFFINE maps OUTPUT coordinates back to INPUT, so the inverse of
    `y -> base + (y - base) * scale` is what goes in the matrix.
    """
    if scale == 1.0:
        return img
    bbox = img.getbbox()
    if bbox is None:
        return img
    base = float(bbox[3])
    inv = 1.0 / scale
    out = img.transform(
        img.size, Image.AFFINE, (1, 0, 0, 0, inv, base * (1.0 - inv)),
        resample=Image.BICUBIC,
    )
    # A stretch that pushed the creature off the top of the master canvas would
    # silently crop its antenna or ears, and the crop would only ever show up on
    # glass. Beacon Beast's antenna already starts 8 px from the top edge, so
    # this is a live constraint and not a hypothetical one.
    grown = out.getbbox()
    if grown is None or grown[1] <= 0 or grown[3] >= img.size[1]:
        sys.exit(f"breath scale {scale} clips the master canvas: bbox {grown}")
    return out


def pose(img, frame):
    """frame 0 = exhale, frame 1 = inhale."""
    if frame == 0:
        return breathe(pulse_glow(img, 1.0 - GLOW_DOWN), 1.0 - BREATH)
    return breathe(pulse_glow(img, 1.0 + GLOW_UP), 1.0 + BREATH)


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Never emit the chroma key as a real colour, or that pixel goes invisible
    # if this art is ever handed to the chroma-key path.
    return 0xF81E if value == TRANSPARENT_COLOR else value


def convert_opaque(img, size):
    """Flatten onto black at `size` square. Every pixel is a real colour."""
    small = img.resize((size, size), Image.LANCZOS)
    flat = Image.new("RGB", (size, size), BG)
    flat.paste(small, (0, 0), small)
    px = flat.load()
    out = []
    for y in range(size):
        for x in range(size):
            r, g, b = px[x, y]
            out.append(rgb565(r, g, b))
    return out


def emit_array(name, values, size):
    lines = [f"static const uint16_t {name}[{size} * {size}] PROGMEM = {{"]
    for row in range(size):
        chunk = values[row * size:(row + 1) * size]
        for i in range(0, size, 12):
            lines.append("    " + ",".join(f"0x{v:04X}" for v in chunk[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


# The whole point of these frames is that they MOVE. A transform that quietly
# became a no-op - a BREATH rounded away by the downsample, a GLOW that missed
# every accent - would still bake, still link, still draw, and would cost 605 KB
# of flash to show the same frozen pet the board already had. So every run
# reports how much actually changed, and refuses to write frames that do not.
MIN_CHANGED_PX = 200   # of 30976; below this the animation is not visible


def report(stage, frames):
    a, b = frames
    changed = sum(1 for x, y in zip(a, b) if x != y)
    total = OUT_PX * OUT_PX
    if changed < MIN_CHANGED_PX:
        sys.exit(f"{stage}: idle frames differ in only {changed} of {total} "
                 f"pixels - that is not an animation. Check BREATH and GLOW.")
    ink = [sum(1 for v in f if v != 0x0000) for f in frames]
    print(f"  {stage:9s} {OUT_PX}x{OUT_PX} x2   "
          f"{changed:5d}/{total} px change   ink {ink[0]}/{ink[1]}")


def build_header(preview_dir):
    parts = [
        "#pragma once",
        "// -- HexHound - HD Idle Animation (RGB565) -----------------------",
        "//",
        "// GENERATED FILE - do not hand-edit.",
        f"//   source:    assets/hd/asset_<stage>_{SRC_PX}.png",
        "//   generator: scripts/gen_hd_idle.py",
        "//   panel:     480x480 round only (LilyGo T-RGB)",
        "//",
        "// Two frames per stage, a breath derived from the same master the resting",
        "// portrait is cut from: the body scales about its own base and the neon",
        "// pulses with it. See the generator for why, and for the cost.",
        "//",
        "// These frames are OPAQUE - the background is black, not the 0xF81F chroma",
        "// key the rest of the HD art uses. Two frames with slightly different",
        "// silhouettes alternating through a chroma key would leave a fringe of the",
        "// previous frame unless the slot were cleared between them, and clearing it",
        "// twice a second is a visible flash. Writing every pixel costs nothing here",
        "// because the pet slot is already black.",
        "//",
        "// Include from ONE .cpp only (animator.cpp), same rule as sprites.h.",
        "// --------------------------------------------------------------------------",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        "",
        f"#define HD_IDLE_SIZE {OUT_PX}",
        "#define HD_IDLE_COUNT 2",
        "",
    ]

    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)

    for stage in STAGES:
        png = source_png(stage)
        if not os.path.exists(png):
            sys.exit(f"missing source art: {png}")
        master = Image.open(png).convert("RGBA")
        if master.size != (SRC_PX, SRC_PX):
            sys.exit(f"{png} is {master.size}, expected {SRC_PX}x{SRC_PX}")
        baked = []
        for frame in range(2):
            posed = pose(master, frame)
            if preview_dir:
                posed.resize((OUT_PX, OUT_PX), Image.LANCZOS).save(
                    os.path.join(preview_dir, f"idle_{stage}_{frame}.png"))
            baked.append(convert_opaque(posed, OUT_PX))
            parts.append(emit_array(f"hd_{stage}_idle_{frame}", baked[-1], OUT_PX))
            parts.append("")
        report(stage, baked)

    parts.append("static const uint16_t* const hd_idle_by_stage[5][HD_IDLE_COUNT] = {")
    for stage in STAGES:
        parts.append(f"    {{ hd_{stage}_idle_0, hd_{stage}_idle_1 }},")
    parts.append("};")
    parts.append("")

    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", metavar="DIR", default=None,
                    help="also write the posed frames as PNGs into DIR so they "
                         "can be eyeballed; nothing is written into the repo")
    args = ap.parse_args()

    print("=== HexHound HD idle generator (480 round panel) ===")
    text = build_header(args.preview)
    out_path = os.path.join(OUT_DIR, "sprites_hd_idle_round480.h")
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    kb = os.path.getsize(out_path) / 1024.0
    flash = len(STAGES) * 2 * OUT_PX * OUT_PX * 2 / 1024.0
    print(f"\nwrote {out_path} ({kb:.0f} KB source, {flash:.0f} KB of flash)")
    if args.preview:
        print(f"preview PNGs in {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
