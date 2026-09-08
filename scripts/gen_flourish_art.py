#!/usr/bin/env python3
"""Bake the flourish motes into src/ui/flourish_art.h.

A flourish is the embellishment that plays AROUND the pet: three of them exist
in the item table (PACKET FLURRY, SOLDER SPARKS, NOISE AURORA) and each is
drawn as a handful of identical MOTES riding a ring. This script bakes one mote
per flourish, at one size per panel family, the same way scripts/gen_worn_art.py
bakes one overlay per worn cosmetic.

One mote and not a strip of animation frames, deliberately. The motion is in
where the motes are put, not in what each one looks like: src/ui/ui_flourish.cpp
steps them between fixed stations on a ring, so the whole animation costs three
small images instead of three times as many frames as the loop is long. That
matters on the 480 round panel, which is the only board that can afford this at
all and is also the one whose flash is half full.

Each mote is authored inside a UNIT SQUARE and blitted at the size
ui_flourish.cpp computes from the pet's own box, so nothing in this file knows
about pixels on any particular board.

Colours are READ OUT OF src/content/item_defs.h rather than repeated here, for
the same reason gen_worn_art.py and gen_item_icons.py read them: the accent in
the table and the colour on the screen are one fact, and a second copy of a
fact is a copy that goes stale.

Transparency is the 0xF81F chroma key drawSprite() skips, at the same
ALPHA_CUTOFF the HD pet art and the worn overlays use.

Run:  python scripts/gen_flourish_art.py
"""

import os
import re
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEM_DEFS = os.path.join(PROJECT_DIR, "src", "content", "item_defs.h")
OUT_PATH = os.path.join(PROJECT_DIR, "src", "ui", "flourish_art.h")

TRANSPARENT_COLOR = 0xF81F
ALPHA_CUTOFF = 24
SS = 8                      # supersample factor before the LANCZOS reduction

# One size per panel family, the same split item_art.h and worn_art.h use.
# Roughly an ninth of the pet's box on each: the pet is drawn 176 px square on
# the 480 round panel, 88 or 64 on the middle family, and 24 on a 160x80. A
# mote much larger than this stops reading as a particle and starts reading as
# a second object orbiting the pet; much smaller and it disappears entirely on
# the small panel, where 4 px is already the floor.
SIZES = [20, 10, 4]

# Emission order. Also the order of the lookup tables at the end of the header.
ITEMS = ["cos.confetti", "cos.sparks", "cos.aurora"]


def read_colors():
    """Pull the RGB565 accent for every cosmetic out of the item table.

    Same pattern gen_worn_art.py uses, and deliberately NOT anchored on the end
    of the row: ItemDef has grown a trailing field once already (WearAnchor),
    and a pattern that requires the colour to be the last thing in the row
    silently stops matching when that happens. See the note on ROW_RE in
    scripts/gen_item_icons.py, where exactly that cost four icons.
    """
    text = open(ITEM_DEFS, encoding="utf-8").read()
    out = {}
    pattern = (r'\{\s*"(cos\.[a-z]+)"\s*,\s*"[^"]*"\s*,\s*ITEM_COSMETIC\s*,'
               r'\s*\w+\s*,\s*\w+\s*,\s*(0x[0-9A-Fa-f]{4})')
    for m in re.finditer(pattern, text):
        out[m.group(1)] = int(m.group(2), 16)
    return out


def rgb(c565):
    r = ((c565 >> 11) & 0x1F) * 255 // 31
    g = ((c565 >> 5) & 0x3F) * 255 // 63
    b = (c565 & 0x1F) * 255 // 31
    return (r, g, b)


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def tint(c, f):
    """Blend towards white by f.

    Not shade(c, >1). Every accent in the item table is already saturated, so
    multiplying it up either changes nothing or clamps a channel and shifts the
    hue. PACKET FLURRY is the case that proved it: its accent is 0xF81E, and
    shade(c, 1.6) came out (255, 0, 255) - which is not merely invisible
    against the body, it IS the 0xF81F chroma key, so the highlight was being
    rewritten to 0xF81E by the guard in render() and vanishing entirely.
    """
    return tuple(int(v + (255 - v) * f) for v in c)


# ── The three motes ───────────────────────────────────────────────────────
#
# Design rule, and it is the same one gen_item_icons.py states: silhouette
# first. The smallest panel bakes these at FOUR pixels, so anything that is not
# in the outline is gone there. Detail exists for the 20 px set and is expected
# to blur away on the way down.


def draw_confetti(d, N, col):
    """A packet: a small rectangle with a brighter header stripe.

    PACKET FLURRY is packets, so the mote is a packet - a body and a header,
    the way every diagram of one is drawn. Slightly wider than tall so the two
    parts stay distinguishable for one size longer on the way down.
    """
    d.rounded_rectangle([0.10 * N, 0.24 * N, 0.90 * N, 0.76 * N],
                        radius=0.10 * N, fill=shade(col, 0.72))
    d.rectangle([0.16 * N, 0.30 * N, 0.84 * N, 0.44 * N], fill=tint(col, 0.55))


def draw_sparks(d, N, col):
    """A four-point star: a hot core with four thin spikes.

    A spark is a point of light with rays, and the rays are what stop a ring of
    these reading as a ring of dots. They are thin on purpose: at 10 px they
    survive as single pixels reaching into the corners, which is exactly the
    silhouette a spark needs.
    """
    hot = shade(col, 1.8)
    for a, b in (((0.46, 0.00), (0.54, 1.00)),
                 ((0.00, 0.46), (1.00, 0.54))):
        d.rectangle([a[0] * N, a[1] * N, b[0] * N, b[1] * N], fill=col)
    d.ellipse([0.28 * N, 0.28 * N, 0.72 * N, 0.72 * N], fill=hot)
    d.ellipse([0.40 * N, 0.40 * N, 0.60 * N, 0.60 * N], fill=(255, 255, 255))


def draw_aurora(d, N, col):
    """A soft graded blob, brightest in the middle.

    NOISE AURORA is a glow rather than an object, so this is the one mote with
    no edge: concentric ellipses stepping from dim to bright, which the LANCZOS
    reduction then smooths into a gradient. Drawn as a stack rather than with a
    blur filter so the result is deterministic across Pillow versions.
    """
    steps = 6
    for i in range(steps):
        f = i / float(steps - 1)              # 0 at the outside, 1 at the core
        r = 0.50 - 0.38 * f
        d.ellipse([(0.50 - r) * N, (0.50 - r * 1.15) * N,
                   (0.50 + r) * N, (0.50 + r * 1.15) * N],
                  fill=shade(col, 0.30 + 1.30 * f))


DRAW = {
    "cos.confetti": draw_confetti,
    "cos.sparks": draw_sparks,
    "cos.aurora": draw_aurora,
}


def render(item, size, colors):
    N = size * SS
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    DRAW[item](ImageDraw.Draw(img), N, rgb(colors[item]))
    img = img.resize((size, size), Image.LANCZOS)

    out = []
    for y in range(size):
        for x in range(size):
            r, g, b, a = img.getpixel((x, y))
            if a < ALPHA_CUTOFF:
                out.append(TRANSPARENT_COLOR)
                continue
            if a < 255:
                f = a / 255.0
                r, g, b = int(r * f), int(g * f), int(b * f)
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out.append(0xF81E if v == TRANSPARENT_COLOR else v)
    return out


def emit_array(name, values, size):
    lines = ["static const uint16_t %s[%d * %d] PROGMEM = {" % (name, size, size)]
    for row in range(size):
        chunk = values[row * size:(row + 1) * size]
        for i in range(0, size, 10):
            cells = ",".join("0x%04X" % v for v in chunk[i:i + 10])
            lines.append("    " + cells + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    colors = read_colors()
    missing = [i for i in ITEMS if i not in colors]
    if missing:
        sys.exit("no colour found in item_defs.h for: " + ", ".join(missing))

    bar = "─" * 12
    dash = "─"
    out = [
        "#pragma once",
        "",
        "// " + dash * 4 + " HexHound - Flourish Motes (RGB565) " + bar,
        "//",
        "// GENERATED by scripts/gen_flourish_art.py. Do not edit by hand.",
        "//",
        "// One MOTE per COSMETIC_FLOURISH item. A flourish is drawn as several",
        "// copies of its mote riding a ring around the pet, so the animation lives",
        "// in ui_flourish.cpp and this header holds three small images rather than",
        "// three animation strips.",
        "//",
        "// Transparent pixels use the 0xF81F chroma key that drawSprite() skips.",
        "// Colours come from the accent in src/content/item_defs.h, so the icon in",
        "// the Kit and the motes round the pet can never disagree.",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        '#include "../board/board_profile.h"',
        "",
        "// Source size for this panel family. The same split item_art.h uses.",
        "#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)",
        "#define FLOURISH_ART_PX %d" % SIZES[0],
        "#elif HEXHOUND_PANEL_ROUND || (SCREEN_H > 100)",
        "#define FLOURISH_ART_PX %d" % SIZES[1],
        "#else",
        "#define FLOURISH_ART_PX %d" % SIZES[2],
        "#endif",
        "",
    ]

    for size in SIZES:
        out.append("#if FLOURISH_ART_PX == %d" % size)
        out.append("")
        for item in ITEMS:
            name = "flourish_art_" + item.replace(".", "_")
            out.append(emit_array(name, render(item, size, colors), size))
            out.append("")
        out.append("#endif  // FLOURISH_ART_PX == %d" % size)
        out.append("")

    out.append("// Lookup by ITEM STRING ID, resolved to numeric ids at run time. Same")
    out.append("// arrangement worn_art.h uses, and for the same reason: item ids are")
    out.append("// indices into ITEM_DEFS, so a generated file holding numeric ones would")
    out.append("// be a second place to regenerate every time a row is appended.")
    out.append("static const char* const FLOURISH_ART_IDS[] = {")
    for item in ITEMS:
        out.append('    "%s",' % item)
    out.append("};")
    out.append("")
    out.append("static const uint16_t* const FLOURISH_ART_DATA[] = {")
    for item in ITEMS:
        out.append("    flourish_art_" + item.replace(".", "_") + ",")
    out.append("};")
    out.append("")
    out.append("static const uint8_t FLOURISH_ART_COUNT =")
    out.append("    (uint8_t)(sizeof(FLOURISH_ART_IDS) / sizeof(FLOURISH_ART_IDS[0]));")
    out.append("")
    out.append("static_assert(sizeof(FLOURISH_ART_IDS) / sizeof(FLOURISH_ART_IDS[0]) ==")
    out.append("                  sizeof(FLOURISH_ART_DATA) / sizeof(FLOURISH_ART_DATA[0]),")
    out.append('              "flourish art id and data tables are different lengths; "')
    out.append('              "re-run scripts/gen_flourish_art.py");')
    out.append("")

    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote " + OUT_PATH)
    for size in SIZES:
        b = len(ITEMS) * size * size * 2
        print("  %dx%d: %d motes, %d bytes of flash" % (size, size, len(ITEMS), b))


if __name__ == "__main__":
    main()
