#!/usr/bin/env python3
"""Draw the inventory item icons and bake them into src/ui/item_art.h.

HexHound has no PNG decoder and no guaranteed SD card, so every image the
firmware shows is an RGB565 array in flash. This is the same pipeline as
scripts/gen_hd_sprites.py, with two differences that matter:

  * There is no source PNG. Twenty small icons drawn by hand would be twenty
    files to keep in sync with a table that already exists, so these are drawn
    procedurally here, supersampled at SUPER px and downscaled with LANCZOS,
    which is what gives them the soft neon edge the HD pet art has.
  * The palette is READ OUT OF src/content/item_defs.h rather than repeated
    here. The accent colour in the table and the colour of the icon are the
    same fact, and a second copy of a fact is a copy that goes stale. If the
    parse below stops matching the table this script fails loudly instead of
    quietly baking the wrong colours.

Three sizes are emitted and the header compiles exactly ONE of them, chosen by
panel family:

  8x8    160x80 panels, where a list row is 13 px tall
  16x16  320x172 and 240x240 round panels
  32x32  the 480x480 round panel (LilyGo T-RGB)

The 32 px set exists because the 480 panel was drawing the 16 px art at twice
its size, and unlike the den tiles these icons are NOT flat-colour block art:
they are supersampled and downscaled with LANCZOS, so their edges are graded.
Doubling a graded edge turns every antialiased pixel into a 2x2 block, which is
the one thing antialiasing exists to avoid. Baking at 32 was measured against a
2x enlargement of the 16 px set before it was adopted: on average 35% of the
1,024 texels differ, and up to 59% on cos.poster. That is real detail arriving,
not a re-encoding of detail that was already there.

Contrast with scripts/gen_den_art.py, where the same measurement came back 0 of
1,024 on all three tiles, because those features sit on a coarse unit grid. Do
not assume a bigger bake buys anything; measure it.

Flash cost is 2 bytes per pixel: 2,560 bytes for the 8 px set, 10,240 for the
16 px set, 40,960 for the 32 px set. Only the selected set is compiled in.

Do NOT extend this to the pet art. src/ui/sprites_hd_*.h and
scripts/gen_hd_sprites.py are a different pipeline with a different owner.

Usage:
    pip install Pillow
    python scripts/gen_item_icons.py
"""

import os
import re
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEM_DEFS = os.path.join(PROJECT_DIR, "src", "content", "item_defs.h")
OUT_PATH = os.path.join(PROJECT_DIR, "src", "ui", "item_art.h")

# Supersample canvas, downscaled to each icon size. Deliberately NOT raised when
# the 32 px set was added: 128 still gives 4x4 samples per output texel at 32,
# and any change here would resample the 8 and 16 px sets too, so the two
# shipping panel families would come out one LSB different for no reason.
SUPER = 128
SIZES = (8, 16, 32)
TRANSPARENT_COLOR = 0xF81F  # magenta chroma key, matches sprites.h
ALPHA_CUTOFF = 24           # below this the pixel is background, not art
BG = (0, 0, 0)              # the UI background is black


# ── Palette, read from the item table ─────────────────────────────────────

# The optional trailing `anchor` group is load-bearing, and its absence was a
# real defect: ItemDef grew a WearAnchor field when worn cosmetics were added,
# so the four COSMETIC_WORN rows end `..., 0x07FF, WEAR_HEAD }` and this
# pattern, which required `}` immediately after the colour, stopped matching
# them. Re-running the generator would then have emitted SIXTEEN icons instead
# of twenty, and because ITEM_ICONS is indexed positionally by item id, every
# cosmetic from id 8 up would have been drawn with the wrong picture. It did
# not fail loudly: the only guard downstream is
# `static_assert(ITEM_ICON_COUNT <= ITEM_TYPE_COUNT)` in ui_inventory.cpp,
# which 16 passes. The count check in load_items() below is the loud one now.
#
# So: parse the anchor if it is there, ignore it if it is not, and never again
# make the last field of the row the thing that anchors the match.
ROW_RE = re.compile(
    r'\{\s*"([a-z]+\.[a-z]+)"\s*,\s*"([^"]*)"\s*,\s*'
    r'(ITEM_MATERIAL|ITEM_COSMETIC)\s*,\s*(COSMETIC_\w+)\s*,\s*'
    r'([A-Za-z_0-9]+)\s*,\s*(0x[0-9A-Fa-f]{4})\s*(?:,\s*(\w+)\s*)?\}'
)


def load_items():
    with open(ITEM_DEFS, "r", encoding="utf-8") as fh:
        text = fh.read()

    start = text.index("static const ItemDef ITEM_DEFS[]")
    end = text.index("};", start)
    table = text[start:end]
    rows = ROW_RE.findall(table)
    if not rows:
        sys.exit("could not parse ITEM_DEFS out of src/content/item_defs.h")

    # A row is a line that OPENS with `{ "`, which is how every row in the
    # table is written and how no comment in it is. Counting them independently
    # of ROW_RE is the whole point: a row this generator cannot parse is far
    # worse than a row it cannot find, because ITEM_ICONS is indexed by item id
    # and a silently dropped row shifts every icon after it onto the wrong
    # item. Fail here instead, naming the count, and go fix the pattern.
    declared = sum(1 for line in table.splitlines()
                   if line.lstrip().startswith('{ "'))
    if declared != len(rows):
        sys.exit("ITEM_DEFS has %d rows but ROW_RE matched %d. A row shape "
                 "changed and the icon table would be MISALIGNED with the "
                 "item ids. Fix ROW_RE in scripts/gen_item_icons.py."
                 % (declared, len(rows)))

    return [(m[0], m[1], int(m[5], 16)) for m in rows]


def rgb(color565):
    r = ((color565 >> 11) & 0x1F) * 255 // 31
    g = ((color565 >> 5) & 0x3F) * 255 // 63
    b = (color565 & 0x1F) * 255 // 31
    return (r, g, b)


def dim(c, f):
    return (int(c[0] * f), int(c[1] * f), int(c[2] * f))


# ── Glyphs ────────────────────────────────────────────────────────────────
#
# Every glyph is drawn on a SUPER x SUPER transparent canvas in the item's own
# accent colour. They are read at 8 px on the smallest panel, so the design rule
# is silhouette first: a shape that survives being four pixels wide. Detail is
# there for the 16 px set and is expected to blur away on the small one.

S = SUPER
W = S // 10          # a comfortable stroke at this scale


def g_scrap(d, c):
    d.polygon([(20, 78), (34, 30), (72, 22), (98, 52), (84, 96), (44, 100)],
              fill=dim(c, 0.55), outline=c, width=W // 2)
    d.line([(40, 40), (72, 60)], fill=dim(c, 1.4), width=W // 2)


def g_signal(d, c):
    d.ellipse([54, 88, 74, 108], fill=c)
    for i, r in enumerate((30, 52, 74)):
        d.arc([64 - r, 98 - r, 64 + r, 98 + r], 200, 340,
              fill=dim(c, 1.0 - i * 0.22), width=W)


def g_circuit(d, c):
    d.rectangle([34, 34, 94, 94], fill=dim(c, 0.35), outline=c, width=W // 2)
    for i in range(3):
        y = 46 + i * 18
        d.line([(14, y), (34, y)], fill=c, width=W // 2)
        d.line([(94, y), (114, y)], fill=c, width=W // 2)
    d.rectangle([52, 52, 76, 76], fill=dim(c, 1.3))


def g_shard(d, c):
    d.polygon([(64, 10), (96, 58), (64, 118), (32, 58)],
              fill=dim(c, 0.5), outline=c, width=W // 2)
    d.line([(64, 18), (64, 110)], fill=dim(c, 1.5), width=W // 2)


def g_glitch(d, c):
    d.polygon([(74, 8), (36, 62), (60, 62), (46, 120), (94, 56), (68, 56)],
              fill=c)


def g_copper(d, c):
    for i in range(4):
        x = 22 + i * 22
        d.arc([x - 14, 34, x + 14, 94], 270, 90, fill=c, width=W)
    d.line([(8, 64), (22, 64)], fill=c, width=W // 2)
    d.line([(106, 64), (120, 64)], fill=c, width=W // 2)


def g_lens(d, c):
    d.ellipse([16, 16, 112, 112], fill=dim(c, 0.3), outline=c, width=W)
    d.line([(40, 30), (66, 70), (52, 78), (88, 104)],
           fill=dim(c, 1.6), width=W // 2)


def g_ferrite(d, c):
    d.ellipse([14, 30, 114, 98], fill=dim(c, 0.7), outline=c, width=W // 2)
    d.ellipse([48, 50, 80, 78], fill=(0, 0, 0, 0))


def g_antenna(d, c):
    d.polygon([(20, 88), (64, 34), (108, 88)], fill=dim(c, 0.6), outline=c,
              width=W // 2)
    d.rectangle([14, 88, 114, 104], fill=c)
    d.line([(64, 34), (92, 6)], fill=c, width=W // 2)
    d.ellipse([84, 0, 102, 18], fill=dim(c, 1.5))


def g_scarf(d, c):
    d.line([(10, 46), (40, 66), (70, 40), (100, 62), (120, 46)],
           fill=c, width=W * 2)
    d.polygon([(86, 62), (118, 74), (96, 112), (76, 90)], fill=dim(c, 0.7))


def g_goggles(d, c):
    d.rectangle([8, 56, 120, 72], fill=dim(c, 0.6))
    d.ellipse([10, 40, 58, 88], fill=dim(c, 0.35), outline=c, width=W // 2)
    d.ellipse([70, 40, 118, 88], fill=dim(c, 0.35), outline=c, width=W // 2)
    d.line([(22, 52), (34, 52)], fill=dim(c, 1.6), width=W // 2)


def g_collar(d, c):
    d.ellipse([18, 18, 110, 110], outline=c, width=W + 2)
    for x, y in ((64, 14), (110, 64), (64, 114), (14, 64)):
        d.ellipse([x - 9, y - 9, x + 9, y + 9], fill=dim(c, 1.6))


def g_lamp(d, c):
    d.polygon([(36, 20), (92, 20), (108, 62), (20, 62)], fill=c)
    d.line([(64, 62), (64, 104)], fill=dim(c, 0.7), width=W)
    d.rectangle([34, 104, 94, 116], fill=dim(c, 0.7))


def g_rug(d, c):
    d.rounded_rectangle([10, 34, 118, 96], radius=16, fill=dim(c, 0.55),
                        outline=c, width=W // 2)
    for i in range(4):
        y = 46 + i * 14
        d.line([(20, y), (108, y)], fill=dim(c, 1.4), width=W // 3)


def g_poster(d, c):
    d.rectangle([24, 12, 104, 116], fill=dim(c, 0.3), outline=c, width=W // 2)
    for i in range(4):
        d.line([(24, 34 + i * 22), (104, 34 + i * 22)], fill=c, width=W // 3)
    for i in range(3):
        d.line([(44 + i * 20, 12), (44 + i * 20, 116)], fill=c, width=W // 3)


def g_fern(d, c):
    d.line([(64, 120), (64, 40)], fill=dim(c, 0.8), width=W)
    for i in range(4):
        y = 46 + i * 20
        d.line([(64, y), (24, y - 14)], fill=c, width=W // 2)
        d.line([(64, y), (104, y - 14)], fill=c, width=W // 2)
    d.rectangle([44, 108, 84, 124], fill=dim(c, 0.6))


def g_lantern(d, c):
    d.polygon([(38, 34), (90, 34), (100, 100), (28, 100)],
              fill=dim(c, 0.55), outline=c, width=W // 2)
    d.ellipse([50, 54, 78, 82], fill=dim(c, 1.7))
    d.line([(48, 34), (64, 8), (80, 34)], fill=c, width=W // 2)


def g_confetti(d, c):
    for x, y, s in ((16, 20, 18), (54, 8, 14), (92, 30, 20), (24, 66, 16),
                    (66, 58, 22), (100, 78, 14), (40, 100, 18), (84, 108, 16)):
        d.rectangle([x, y, x + s, y + s], fill=dim(c, 0.6 + (s % 7) * 0.12))


def g_sparks(d, c):
    d.ellipse([50, 50, 78, 78], fill=dim(c, 1.6))
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0),
                   (1, 1), (1, -1), (-1, 1), (-1, -1)):
        d.line([(64 + dx * 20, 64 + dy * 20), (64 + dx * 58, 64 + dy * 58)],
               fill=c, width=W // 2)


def g_aurora(d, c):
    for i in range(3):
        y = 34 + i * 26
        d.line([(6, y), (34, y - 16), (64, y + 12), (94, y - 16), (122, y)],
               fill=dim(c, 1.4 - i * 0.35), width=W + 2)


def g_crate(d, c):
    """A crate with a coil of cable spilling over the front lip.

    Silhouette first: at 8 px this has to read as a BOX, so the crate walls are
    the only thing drawn at full strength and the coil sits inside them rather
    than looping outside, where it would just fur up the outline.
    """
    d.rectangle([16, 44, 112, 116], fill=dim(c, 0.35), outline=c, width=W // 2)
    d.line([(16, 62), (112, 62)], fill=c, width=W // 3)          # top rail
    d.line([(64, 44), (64, 62)], fill=c, width=W // 3)           # lid seam
    for r in (14, 24):                                           # coiled cable
        d.arc([64 - r, 88 - r, 64 + r, 88 + r], 0, 360,
              fill=dim(c, 1.4), width=W // 3)


def g_rack(d, c):
    """A patch panel: two rows of ports with jumper leads looping between them.

    The ports are the detail and are expected to blur out at 8 px; what has to
    survive is a wide BRIGHT panel with a dark row of holes across it.

    The first cut had it the other way round, a dark faceplate with lit ports,
    and at 8 px the two averaged into an even grey smear that read as nothing
    at all next to the fern and the lamp. A real patch panel is a light metal
    face with black holes punched in it, so drawing it that way is both more
    honest and the version that survives the reduction.
    """
    d.rectangle([10, 30, 118, 98], fill=dim(c, 0.75), outline=dim(c, 1.5),
                width=W // 2)
    for row in (46, 82):
        for i in range(5):
            x = 22 + i * 21
            d.rectangle([x - 7, row - 7, x + 7, row + 7], fill=dim(c, 0.12))
    # Two jumpers. They sag, because a patch lead always does, and the sag is
    # what makes this read as cabling rather than as a third row of ports.
    d.arc([22, 40, 106, 104], 20, 160, fill=dim(c, 1.9), width=W // 2)
    d.arc([40, 44, 88, 100], 20, 160, fill=dim(c, 1.6), width=W // 2)


def g_dish(d, c):
    """A parabolic dish on a mast, aimed up and to the right.

    The Sentinel's trophy, and the only den piece that points at something
    outside the room. Drawn as an open ellipse rather than a filled one so the
    dish reads as a hollow reflector at 16 px; at 8 px it collapses to a blob
    on a stick, which is still unmistakably a dish.
    """
    d.line([(58, 122), (58, 70)], fill=dim(c, 0.7), width=W)      # mast
    d.line([(34, 122), (82, 122)], fill=dim(c, 0.7), width=W // 2)  # foot
    d.ellipse([18, 10, 98, 82], fill=dim(c, 0.40), outline=c, width=W // 2)
    d.ellipse([34, 24, 82, 68], fill=dim(c, 0.65))                # inner cup
    d.line([(58, 46), (104, 22)], fill=c, width=W // 3)           # feed arm
    d.ellipse([98, 12, 116, 30], fill=dim(c, 1.6))                # feed horn


def g_generic(d, c):
    d.ellipse([28, 28, 100, 100], fill=dim(c, 0.5), outline=c, width=W // 2)


GLYPHS = {
    "mat.scrap": g_scrap,
    "mat.signal": g_signal,
    "mat.circuit": g_circuit,
    "mat.shard": g_shard,
    "mat.glitch": g_glitch,
    "mat.copper": g_copper,
    "mat.lens": g_lens,
    "mat.ferrite": g_ferrite,
    "cos.antenna": g_antenna,
    "cos.scarf": g_scarf,
    "cos.goggles": g_goggles,
    "cos.collar": g_collar,
    "cos.lamp": g_lamp,
    "cos.rug": g_rug,
    "cos.poster": g_poster,
    "cos.fern": g_fern,
    "cos.lantern": g_lantern,
    "cos.confetti": g_confetti,
    "cos.sparks": g_sparks,
    "cos.aurora": g_aurora,
    "cos.crate": g_crate,
    "cos.rack": g_rack,
    "cos.dish": g_dish,
}


def render(item_id, color565):
    img = Image.new("RGBA", (SUPER, SUPER), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    GLYPHS.get(item_id, g_generic)(d, rgb(color565))
    return img


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Never emit the chroma key as a real colour, or that pixel goes invisible.
    return 0xF81E if value == TRANSPARENT_COLOR else value


def quantize(img, size):
    small = img.resize((size, size), Image.LANCZOS)
    out = []
    for y in range(size):
        for x in range(size):
            r, g, b, a = small.getpixel((x, y))
            if a < ALPHA_CUTOFF:
                out.append(TRANSPARENT_COLOR)
                continue
            if a < 255:
                f = a / 255.0
                r = int(r * f + BG[0] * (1 - f))
                g = int(g * f + BG[1] * (1 - f))
                b = int(b * f + BG[2] * (1 - f))
            out.append(rgb565(r, g, b))
    return out


def emit_array(name, values, size):
    lines = [f"static const uint16_t {name}[{size} * {size}] PROGMEM = {{"]
    for row in range(size):
        chunk = values[row * size:(row + 1) * size]
        for i in range(0, size, 8):
            lines.append("    " + ",".join(f"0x{v:04X}"
                                           for v in chunk[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def sym(item_id):
    return "item_icon_" + item_id.replace(".", "_")


def build(items):
    p = [
        "#pragma once",
        "// ── HexHound - Item Icons (RGB565) ───────────────────────────────",
        "//",
        "// GENERATED FILE - do not hand-edit.",
        "//   generator: scripts/gen_item_icons.py  (draws them, no source PNG)",
        "//   palette:   read from src/content/item_defs.h, never duplicated",
        "//",
        "// Include from ONE .cpp only (ui_inventory.cpp), same rule as sprites.h.",
        "// Transparent pixels use the 0xF81F chroma key that drawSprite() skips.",
        "//",
        "// Icon order matches ITEM_DEFS exactly: index == item id == the index",
        "// into PetState::items[]. ui_inventory.cpp static_asserts the count.",
        "//",
        "// Only ONE size is compiled, chosen by panel family. 8 px on a 160x80,",
        "// whose list rows are 13 px tall; 16 px on the big and 240 round panels;",
        "// 32 px on the 480 round one, which was drawing the 16 px art at double",
        "// size. Every panel now bakes its icons at the size it DRAWS them, so",
        "// ITEM_ICON_PX is both the source and the destination size everywhere and",
        "// no screen enlarges an icon any more.",
        "// ────────────────────────────────────────────────────────────────────────────",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        "",
        '#include "../board/board_profile.h"   // SCREEN_W/H, HEXHOUND_PANEL_ROUND',
        "",
        "#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)",
        "#define ITEM_ICON_PX 32",
        "#elif HEXHOUND_PANEL_ROUND || (SCREEN_H > 100)",
        "#define ITEM_ICON_PX 16",
        "#else",
        "#define ITEM_ICON_PX 8",
        "#endif",
        "",
        f"#define ITEM_ICON_COUNT {len(items)}",
        "",
    ]

    for size in SIZES:
        p.append(f"#if ITEM_ICON_PX == {size}")
        p.append("")
        for item_id, name, color in items:
            print(f"  {item_id:14s} {name:15s} {size}x{size}")
            p.append(f"// {name}")
            p.append(emit_array(sym(item_id),
                                quantize(render(item_id, color), size), size))
            p.append("")
        p.append("#endif")
        p.append("")

    p.append("// Index == item id. Never null for id < ITEM_ICON_COUNT.")
    p.append("static const uint16_t* const ITEM_ICONS[ITEM_ICON_COUNT] = {")
    for item_id, _, _ in items:
        p.append(f"    {sym(item_id)},")
    p.append("};")
    p.append("")
    return "\n".join(p)


def main():
    print("=== HexHound item icon generator ===")
    items = load_items()
    print(f"{len(items)} items parsed from src/content/item_defs.h\n")
    # g_generic is a real fallback and not an error, but an item that reaches it
    # by ACCIDENT ships as an anonymous blob nobody notices until it is on a
    # board. Say which ones out loud.
    fallback = [i for i, _, _ in items if i not in GLYPHS]
    if fallback:
        print("  NOTE: no glyph, drawing g_generic for: "
              + ", ".join(fallback) + "\n")
    text = build(items)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    kb = os.path.getsize(OUT_PATH) / 1024.0
    for size in SIZES:
        print(f"  {size:2d}px set: {len(items) * size * size * 2} bytes of flash")
    print(f"\nwrote {OUT_PATH} ({kb:.0f} KB source)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
