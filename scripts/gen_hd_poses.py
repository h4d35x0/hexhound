#!/usr/bin/env python3
"""Bake one HD "hungry" portrait per stage into src/ui/sprites_hd_hungry.h.

WHY THIS EXISTS
---------------
The home screen draws HD art only while the pet is RESTING; every other
animation state falls through to the 16x16 pixel sprite. That was fine when the
pixel sprite was the only art there is, and it stopped being fine for two
reasons at once:

1. Worn cosmetics are placed from an anchor table normalised to the HD canvas.
   The pixel sprites frame the figure 108 to 200 thousandths tighter and their
   silhouettes move between frames, so the table cannot follow them - measured,
   rendered and rejected twice; see the header of scripts/gen_wear_anchors.py.
   A pet on the pixel path is therefore drawn BARE.
2. ANIM_HUNGRY is not an override with a deadline. updateAnimState() in
   src/main.cpp reselects it every tick while hunger < 30, so it is a STEADY
   STATE. A hungry pet on any board sat on the pixel path indefinitely, blocky
   and wearing nothing, until it was fed.

Point 2 is what makes this art necessary rather than nice. The transient
animations (ANIM_HAPPY at 2 to 5 s, ANIM_ALERT at 4 s) are bounded and can stay
on the pixel path; a state with no time limit cannot.

WHAT A HUNGRY POSE IS
---------------------
Synthesised from the same master still the resting portrait comes from, so it
cannot drift off-model, and so no new source art is needed for five stages
across four panel families. Three transforms, in this order:

  * BODY_DIM   the whole creature loses brightness. This is what reads at 24 px
               on the compact panel, where the other two are sub-pixel.
  * GLOW_DIM   the neon accents lose much more than the body does. A hungry
               HexHound is a machine running low, so its lights go first; the
               same feathered accent mask the idle breath uses.
  * DROOP      a small vertical squash toward the feet. Deliberately small: it
               is about 3 px at 176 and rounds away entirely at 24, so it is a
               bonus on the big panels rather than the thing carrying the read.

Alpha is never touched, so the silhouette is identical to the resting portrait
and the worn-cosmetic anchors land in exactly the same places.

COST
----
Four panel families, one frame per stage. Each board compiles only its own:

    176 px (480 round)    5 x 176 x 176 x 2 B = 302 KB
     88 px (240 round)    5 x  88 x  88 x 2 B =  75 KB
     64 px (big rect)     5 x  64 x  64 x 2 B =  40 KB
     24 px (compact)      5 x  24 x  24 x 2 B =   6 KB

Against roughly 3.0 MB free in the 6,553,600 B app partition on the fullest
board. The 24 px set is smaller than this docstring.

Usage:
    python scripts/gen_hd_hungry.py
    python scripts/gen_hd_hungry.py --preview <dir>
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
OUT_PATH = os.path.join(PROJECT_DIR, "src", "ui", "sprites_hd_poses.h")

# Stage order must match PetStage in src/config.h.
STAGES = ["egg", "pup", "beast", "gremlin", "sentinel"]

SRC_PX = 400
# One per panel family. These MUST equal HD_HOME_SRC_PX for the same family in
# src/ui/animator.h, because the hungry portrait is drawn into the slot the
# resting portrait was laid out for. ui_home.cpp static_asserts the pair.
SIZES = [176, 88, 64, 24]

# One entry per pose: (body gain, accent gain, vertical squash).
#
# A gain below 1 dims, above 1 brightens; the squash is toward the feet, so a
# negative value LIFTS. Alpha is never touched by any of them, which is what
# keeps every silhouette - and therefore every worn-cosmetic anchor - identical
# to the resting portrait.
#
#   hungry  a machine running low: the whole body dims and the neon goes first.
#   happy   the inverse, and deliberately gentler. The accent CORES are at or
#           near RGB565 saturation in the master, so brightening them is mostly
#           clipped away (gen_hd_idle.py measured 22 of 30976 words moving for
#           a doubled up-swing); what actually reads is the bloom HALO lifting
#           and the little hop.
#   alert   not brighter and not dimmer. The body drops back so the accents
#           stand out against it, which reads as attention rather than as mood.
POSES = {
    "hungry": (0.72, 0.38, 0.018),
    # The lift does most of the work here, not the brightness. A Packet Pup is
    # mostly charcoal with small cyan eyes, so brightening its accents moved
    # only 14% of its pixels and the MIN_CHANGED_FRAC guard rejected the set -
    # correctly, because a pose that costs 300 KB and looks the same is worse
    # than no pose. 0.020 is just under the 0.023 ceiling gen_hd_idle.py
    # measured for the tallest stage's antenna against the canvas edge.
    "happy":  (1.12, 1.35, -0.020),
    "alert":  (0.80, 1.28, -0.006),
}

# Same accent detection as the idle breath: the bodies are charcoal (R~G~B) and
# the rim lights are near-white, so both stay out of the mask; the cyan eyes,
# amber vents, green grin, red chevrons and blue visor are all well above it.
GLOW_FEATHER = 7.0
SAT_FLOOR = 100
VAL_FLOOR = 110
ALPHA_FLOOR = 40

TRANSPARENT_COLOR = 0xF81F
BG = (0, 0, 0)

# A transform that quietly became a no-op would still bake, still link, still
# draw, and would cost real flash to show the same portrait the board already
# had. Every run reports how far the hungry pose actually moved from the
# resting one and refuses to write a set that did not move.
MIN_CHANGED_FRAC = 0.15


def source_png(stage):
    return os.path.join(ASSET_DIR, "asset_%s_%d.png" % (stage, SRC_PX))


def accent_feather(img):
    """Feathered mask of the creature's neon."""
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


def dim(img, gain, mask=None):
    """Scale RGB by `gain`, optionally only where `mask` is lit.

    Alpha is returned untouched, which is what guarantees the silhouette - and
    therefore every worn-cosmetic anchor - is unchanged.
    """
    rgb = img.convert("RGB")
    darker = rgb.point(lambda p: max(0, int(p * gain + 0.5)))
    out = darker if mask is None else Image.composite(darker, rgb, mask)
    out = out.convert("RGBA")
    out.putalpha(img.split()[3])
    return out


def droop(img, amount):
    """Vertical squash about the bottom of the alpha bounding box.

    PIL's AFFINE maps OUTPUT coordinates back to INPUT, so the matrix holds the
    inverse of `y -> base + (y - base) * scale`. Squashing only ever moves the
    figure DOWN into space it already occupied, so unlike the idle breath's
    stretch there is no canvas-clipping check to make.
    """
    scale = 1.0 - amount
    if abs(scale - 1.0) < 1e-6:
        return img
    # A LIFT (negative amount) stretches instead of squashing, so unlike the
    # squash it can push a tall stage off the top of the master canvas. The
    # lifts here are a fraction of a percent and gen_hd_idle.py already
    # establishes that anything under about 2.3% is safe on the tallest stage,
    # but the check is cheap and a silently cropped antenna would only ever
    # show up on glass.
    if scale > 1.0:
        grown = img.transform(
            img.size, Image.AFFINE,
            (1, 0, 0, 0, 1.0 / scale, float(img.getbbox()[3]) * (1.0 - 1.0 / scale)),
            resample=Image.BICUBIC).getbbox()
        if grown is None or grown[1] <= 0 or grown[3] >= img.size[1]:
            sys.exit("lift %.4f clips the master canvas: bbox %s" % (amount, grown))
    bbox = img.getbbox()
    if bbox is None:
        return img
    base = float(bbox[3])
    inv = 1.0 / scale
    return img.transform(
        img.size, Image.AFFINE, (1, 0, 0, 0, inv, base * (1.0 - inv)),
        resample=Image.BICUBIC,
    )


def make_pose(img, name):
    body, accent, squash = POSES[name]
    lit = accent_feather(img)
    out = dim(img, body)
    out = dim(out, accent, lit)
    return droop(out, squash)


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Never emit the chroma key as a real colour, or that pixel goes invisible
    # if this art is ever handed to the chroma-key path.
    return 0xF81E if value == TRANSPARENT_COLOR else value


def convert_opaque(img, size):
    """Flatten onto black at `size` square. Every pixel is a real colour.

    Opaque, like the idle frames and the resting portrait, because the home
    screen swaps between images of the same footprint and clearing between them
    would flash. See the note in animator.h.
    """
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
    lines = ["static const uint16_t %s[%d * %d] PROGMEM = {" % (name, size, size)]
    for row in range(size):
        chunk = values[row * size:(row + 1) * size]
        for i in range(0, size, 12):
            lines.append("    " + ",".join("0x%04X" % v for v in chunk[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", metavar="DIR",
                    help="also write the posed PNGs here for eyeballing")
    args = ap.parse_args()

    masters = {}
    for stage in STAGES:
        path = source_png(stage)
        if not os.path.exists(path):
            sys.exit("missing master art: " + path)
        masters[stage] = Image.open(path).convert("RGBA")

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)

    bar = "─" * 10
    out = [
        "#pragma once",
        "",
        "// " + "─" * 4 + " HexHound - HD Hungry Portrait (RGB565) " + bar,
        "//",
        "// GENERATED by scripts/gen_hd_hungry.py. Do not edit by hand.",
        "//",
        "// One portrait per stage for the pet's HUNGRY state, synthesised from the",
        "// same master still the resting portrait comes from.",
        "//",
        "// It exists because ANIM_HUNGRY is a STEADY state, not a timed override:",
        "// updateAnimState() reselects it every tick while hunger < 30. Before this,",
        "// a hungry pet fell through to the 16x16 pixel sprite and stayed there,",
        "// blocky and - because worn cosmetics can only be placed on the HD canvas -",
        "// WEARING NOTHING, until it was fed. The transient animations are bounded",
        "// and can still use the pixel path; a state with no time limit cannot.",
        "//",
        "// The alpha channel is untouched by every transform, so the silhouette is",
        "// identical to the resting portrait and the worn anchors land unchanged.",
        "//",
        "// Opaque, like the idle frames: the home screen swaps between images of the",
        "// same footprint and clearing between them would flash.",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        '#include "../board/board_profile.h"',
        '#include "../config.h"   // PetStage',
        "",
        "// One size per panel family. Must equal HD_HOME_SRC_PX for the same family",
        "// in animator.h; ui_home.cpp static_asserts the pair, because this art is",
        "// drawn into the slot the resting portrait was laid out for.",
        "#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)",
        "#define HD_POSE_PX 176",
        "#elif HEXHOUND_PANEL_ROUND",
        "#define HD_POSE_PX 88",
        "#elif SCREEN_H > 100",
        "#define HD_POSE_PX 64",
        "#else",
        "#define HD_POSE_PX 24",
        "#endif",
        "",
    ]

    names = sorted(POSES.keys())
    stats = []
    for size in SIZES:
        out.append("#if HD_POSE_PX == %d" % size)
        out.append("")
        for pose in names:
            for stage in STAGES:
                master = masters[stage]
                posed = make_pose(master, pose)
                baked = convert_opaque(posed, size)
                rest = convert_opaque(master, size)
                changed = sum(1 for a, b in zip(baked, rest) if a != b)
                stats.append((size, pose, stage, changed / float(size * size)))
                if args.preview and size == SIZES[0]:
                    posed.resize((size, size), Image.LANCZOS).save(
                        os.path.join(args.preview, "%s_%s.png" % (pose, stage)))
                out.append(emit_array("hd_%s_%s" % (stage, pose), baked, size))
                out.append("")
        out.append("#endif  // HD_POSE_PX == %d" % size)
        out.append("")

    out.append("// The poses, in the order of the PetPose enum below.")
    out.append("enum PetPose : uint8_t {")
    for i, pose in enumerate(names):
        out.append("    POSE_%s = %d," % (pose.upper(), i))
    out.append("    POSE_COUNT")
    out.append("};")
    out.append("")
    out.append("static const uint16_t* const hd_pose_by_stage[POSE_COUNT][5] = {")
    for pose in names:
        out.append("    { " + ", ".join("hd_%s_%s" % (st, pose) for st in STAGES) + " },")
    out.append("};")
    out.append("")
    out.append("// Never null, and both indices clamped rather than trusted: this is read")
    out.append("// on every home-screen repaint, and a stage byte from a corrupt save must")
    out.append("// not index off the front of the table. Same shape as getStageHomeHD().")
    out.append("inline const uint16_t* getStagePoseHD(PetStage stage, PetPose pose) {")
    out.append("    int s = (int)stage - (int)STAGE_EGG;")
    out.append("    if (s < 0) s = 0;")
    out.append("    if (s > 4) s = 4;")
    out.append("    int p = (int)pose;")
    out.append("    if (p < 0 || p >= (int)POSE_COUNT) p = 0;")
    out.append("    return hd_pose_by_stage[p][s];")
    out.append("}")
    out.append("")

    worst = min(f for _, _, _, f in stats)
    if worst < MIN_CHANGED_FRAC:
        for size, pose, stage, f in stats:
            if f < MIN_CHANGED_FRAC:
                print("  %3dpx %-7s %-9s only %.1f%% of pixels differ from resting"
                      % (size, pose, stage, f * 100.0), file=sys.stderr)
        sys.exit("a pose is not visibly different from the resting portrait; "
                 "its POSES entry is doing nothing")

    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote " + OUT_PATH)
    for size in SIZES:
        rows = [f for sz, _, _, f in stats if sz == size]
        kb = len(STAGES) * len(names) * size * size * 2 / 1024.0
        print("  %3dpx: %d portraits, %6.1f KB, %.0f-%.0f%% of pixels differ from resting"
              % (size, len(STAGES) * len(names), kb, min(rows) * 100, max(rows) * 100))


if __name__ == "__main__":
    main()
