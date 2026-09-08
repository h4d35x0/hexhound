#!/usr/bin/env python3
"""Bake the worn-cosmetic overlays into src/ui/worn_art.h.

These are the four COSMETIC_WORN items drawn ON the pet: two head pieces and
two neck pieces. They are generated rather than drawn by hand for the reason
every other art set in this project is - one script, one palette, one place to
re-run when a size changes - and because they have to exist at three sizes for
three panel families.

Each overlay is authored inside a UNIT SQUARE and blitted into the square that
src/ui/wear_anchors.h defines for its anchor, so the composition here is in
fractions of that square and nothing in this file knows about pixels on any
particular board. The square is deliberately bigger than the body part: a hat
needs room above the skull for its antenna, so the cap itself occupies only the
lower middle of its square.

Colours are READ OUT OF src/content/item_defs.h rather than repeated here, so
an item whose accent changes cannot end up with an icon in the Kit and an
overlay on the pet in two different colours.

Transparency is the 0xF81F chroma key drawSprite() skips, at the same
ALPHA_CUTOFF the HD pet art uses, so an overlay composites onto the pet exactly
the way the pet composites onto the screen.

Run:  python scripts/gen_worn_art.py
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
OUT_PATH = os.path.join(PROJECT_DIR, "src", "ui", "worn_art.h")

TRANSPARENT_COLOR = 0xF81F
ALPHA_CUTOFF = 24
SS = 8                      # supersample factor before the LANCZOS reduction

# One size per panel family, matching the split item_art.h already uses.
SIZES = [96, 48, 24]

# Emission order. Also the order of the lookup tables at the end of the header.
ITEMS = ["cos.antenna", "cos.goggles", "cos.scarf", "cos.collar"]


def read_colors():
    """Pull the RGB565 accent for every cosmetic out of the item table."""
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


def draw_antenna(d, N, col):
    """A cap with an antenna, sitting in the UPPER part of its square.

    The square is centred on the middle of the head and is wider than the head
    is, so 0.32 down it is roughly the crown. Everything above that is clear air
    for the antenna, which is the whole reason the square is oversized.
    """
    dark = shade(col, 0.45)
    d.ellipse([0.40 * N, 0.02 * N, 0.60 * N, 0.16 * N], fill=col)          # ball
    d.rectangle([0.47 * N, 0.11 * N, 0.53 * N, 0.28 * N], fill=dark)       # rod
    d.ellipse([0.18 * N, 0.16 * N, 0.82 * N, 0.44 * N], fill=col)          # dome
    d.rectangle([0.18 * N, 0.28 * N, 0.82 * N, 0.33 * N], fill=col)
    d.rectangle([0.10 * N, 0.29 * N, 0.90 * N, 0.35 * N], fill=dark)       # brim


def draw_goggles(d, N, col):
    """A strap with two lenses, across the middle of the square, on the eyes."""
    dark = shade(col, 0.40)
    d.rectangle([0.02 * N, 0.44 * N, 0.98 * N, 0.55 * N], fill=dark)       # strap
    for cx in (0.31, 0.69):
        d.ellipse([(cx - 0.17) * N, 0.38 * N, (cx + 0.17) * N, 0.62 * N], fill=dark)
        d.ellipse([(cx - 0.11) * N, 0.43 * N, (cx + 0.11) * N, 0.57 * N], fill=col)
    d.rectangle([0.45 * N, 0.45 * N, 0.55 * N, 0.54 * N], fill=dark)       # bridge


def draw_scarf(d, N, col):
    """A wrap round the neck with one short tail."""
    dark = shade(col, 0.55)
    d.rounded_rectangle([0.12 * N, 0.40 * N, 0.88 * N, 0.58 * N],
                        radius=0.08 * N, fill=col)
    d.ellipse([0.41 * N, 0.45 * N, 0.60 * N, 0.65 * N], fill=dark)         # knot
    # A short tail. The first cut hung to 0.98 of the square and read as a bib
    # rather than a scarf once it was composited over a body.
    d.polygon([(0.53 * N, 0.58 * N), (0.68 * N, 0.60 * N),
               (0.64 * N, 0.82 * N), (0.51 * N, 0.79 * N)], fill=col)


def draw_collar(d, N, col):
    """A band with three lit cells."""
    dark = shade(col, 0.35)
    d.rounded_rectangle([0.06 * N, 0.40 * N, 0.94 * N, 0.60 * N],
                        radius=0.07 * N, fill=dark)
    for cx in (0.27, 0.50, 0.73):
        d.ellipse([(cx - 0.075) * N, 0.435 * N, (cx + 0.075) * N, 0.565 * N],
                  fill=col)


DRAW = {
    "cos.antenna": draw_antenna,
    "cos.goggles": draw_goggles,
    "cos.scarf": draw_scarf,
    "cos.collar": draw_collar,
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
        for i in range(0, size, 12):
            cells = ",".join("0x%04X" % v for v in chunk[i:i + 12])
            lines.append("    " + cells + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    colors = read_colors()
    missing = [i for i in ITEMS if i not in colors]
    if missing:
        sys.exit("no colour found in item_defs.h for: " + ", ".join(missing))

    bar = "─" * 12
    out = [
        "#pragma once",
        "",
        "// " + "─" * 4 + " HexHound - Worn Cosmetic Overlays (RGB565) " + bar,
        "//",
        "// GENERATED by scripts/gen_worn_art.py. Do not edit by hand.",
        "//",
        "// The four COSMETIC_WORN items, drawn ON the pet. Each is authored inside a",
        "// unit square and blitted into the square src/ui/wear_anchors.h defines for",
        "// its anchor, so one set serves every pet size on every panel: a 24 px pet on",
        "// a T-Dongle and a 320 px hero shot on the T-RGB.",
        "//",
        "// Transparent pixels use the 0xF81F chroma key that drawSprite() skips, so an",
        "// overlay composites onto the pet the same way the pet composites onto the",
        "// screen. Colours come from the accent in src/content/item_defs.h, so the",
        "// icon in the Kit and the overlay on the pet can never disagree.",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        '#include "../board/board_profile.h"',
        "",
        "// Source size for this panel family. The same split item_art.h uses.",
        "#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)",
        "#define WORN_ART_PX %d" % SIZES[0],
        "#elif HEXHOUND_PANEL_ROUND || (SCREEN_H > 100)",
        "#define WORN_ART_PX %d" % SIZES[1],
        "#else",
        "#define WORN_ART_PX %d" % SIZES[2],
        "#endif",
        "",
    ]

    for size in SIZES:
        out.append("#if WORN_ART_PX == %d" % size)
        out.append("")
        for item in ITEMS:
            name = "worn_art_" + item.replace(".", "_")
            out.append(emit_array(name, render(item, size, colors), size))
            out.append("")
        out.append("#endif  // WORN_ART_PX == %d" % size)
        out.append("")

    out.append("// Lookup by ITEM STRING ID, resolved to numeric ids at run time. A table")
    out.append("// rather than a switch so adding an overlay is one line in")
    out.append("// scripts/gen_worn_art.py and nothing at all in the renderer.")
    out.append("static const char* const WORN_ART_IDS[] = {")
    for item in ITEMS:
        out.append('    "%s",' % item)
    out.append("};")
    out.append("")
    out.append("static const uint16_t* const WORN_ART_DATA[] = {")
    for item in ITEMS:
        out.append("    worn_art_" + item.replace(".", "_") + ",")
    out.append("};")
    out.append("")
    out.append("static const uint8_t WORN_ART_COUNT =")
    out.append("    (uint8_t)(sizeof(WORN_ART_IDS) / sizeof(WORN_ART_IDS[0]));")
    out.append("")
    out.append("static_assert(sizeof(WORN_ART_IDS) / sizeof(WORN_ART_IDS[0]) ==")
    out.append("                  sizeof(WORN_ART_DATA) / sizeof(WORN_ART_DATA[0]),")
    out.append('              "worn art id and data tables are different lengths; "')
    out.append('              "re-run scripts/gen_worn_art.py");')
    out.append("")

    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote " + OUT_PATH)
    for size in SIZES:
        kb = len(ITEMS) * size * size * 2 / 1024.0
        print("  %dx%d: %d overlays, %.1f KB" % (size, size, len(ITEMS), kb))


if __name__ == "__main__":
    main()
