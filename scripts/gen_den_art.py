#!/usr/bin/env python3
"""Draw the den room tiles and bake them into src/ui/den_art.h.

The den is a ROOM, so it needs a backdrop. HexHound has no PNG decoder and no
guaranteed SD card, so every image the firmware shows is an RGB565 array in
flash - the same pipeline as scripts/gen_item_icons.py.

── Why tiles and not a backdrop image ────────────────────────────────────────

RGB565 is 2 bytes per pixel. A full-screen backdrop would cost 25,600 bytes on
the 160x80 T-Dongle, 110,080 on the 320x172 Waveshare, 115,200 on the 240x240
round panel and 460,800 on the 480x480 T-RGB, and it would have to be redrawn
for every new panel size. Three small SEAMLESS tiles cost 384 bytes (8 px set),
1,536 bytes (16 px set) or 6,144 bytes (32 px set), the firmware compiles
exactly one set, and the same three tiles fill a room of any size on any panel.
The room is therefore resolution-independent by construction rather than by
having four hand-made backdrops to keep in sync.

── The three tiles ───────────────────────────────────────────────────────────

  wall   the back wall: panelled, with a rivet, seams on the top/left edges so
         tiling forms a continuous grid
  floor  the floor: a darker lattice, seams on the top/left edges for the same
         reason
  rail   the wall/floor junction, ONE tile tall: wall above, a lit edge, a
         shadow, floor below. Drawing this as art rather than as two lines is
         what gives the room a horizon instead of a colour change.

Coarse features are drawn on a SUPER/8 unit grid so that they land on exact
pixel boundaries at EVERY output size; the 32 px set adds a finer shading pass
on a SUPER/32 grid, which is where its extra bytes actually go (see FINE below -
a plain re-run at 32 was measured to be a byte-identical enlargement of the 16
px set, so the coarse grid alone would have bought nothing). The downscale is
BOX (area average) rather
than LANCZOS: a Lanczos kernel reaches past the tile edge, which would leave a
visible seam every tile. The item icons use LANCZOS because they are lone
sprites on black and want the soft neon edge; a tile wants to be seamless.

── Palette lives here, and is exported ───────────────────────────────────────

ui_den.cpp draws a few things the tiles cannot: the empty-slot brackets, the
shelf a wall row of items stands on, the pet's grounding shadow. Those must be
the same colours as the tiles, so the palette is emitted into the header as
DEN_COL_* rather than retyped in the .cpp. One copy of a fact.

Do NOT extend this to the pet art or the item icons. src/ui/sprites_hd_*.h,
src/ui/form_art.h and src/ui/item_art.h are different pipelines with different
owners.

Usage:
    pip install Pillow
    python scripts/gen_den_art.py
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PATH = os.path.join(PROJECT_DIR, "src", "ui", "den_art.h")

SUPER = 128                 # supersample canvas; 128 = 16 units per output px
SIZES = (8, 16, 32)
UNIT = SUPER // 8           # one pixel of the SMALLEST output size

# The size at which a tile stops being an enlargement and starts being art.
#
# MEASURED, not assumed. Every coarse feature below sits on a multiple of UNIT,
# and UNIT is a whole number of output pixels at 8, 16 AND 32, so a plain re-run
# of the painters at 32 comes out byte-identical to a 2x nearest-neighbour
# enlargement of the 16 px set: 0 of 1,024 texels differ, on all three tiles.
# (The item icons are the opposite case - graded LANCZOS edges, ~35% of texels
# differ - which is why THEY needed a 32 px bake and these did not.)
#
# So a 32 px tile set is only worth its 6,144 bytes if it carries something a
# 16 px tile physically cannot hold. FINE is that something: one output texel at
# 32 px, which is HALF a texel at 16 and therefore unrepresentable there. It
# buys the panel joints a bevel and the horizon a lip.
#
# The detail is deliberately all shading, drawn in colours the palette already
# has and never brighter than what is already on the tile. The room's job is to
# recede behind the pet and the item icons standing on it; a 480 panel is a
# reason to make the surface finer, not louder.
FINE = SUPER // 32

# ── Palette ───────────────────────────────────────────────────────────────
# A dark equipment closet: cold navy walls, a colder floor, one lit edge where
# they meet. Deliberately low-contrast so the pet and the item icons (which are
# bright, saturated and lit) read as the subjects and the room reads as the
# background. A busy backdrop on a 160x80 panel makes an 8 px icon invisible.
#
# Authored as RGB565, not as 24-bit RGB. Down here in the near-black end of the
# range 565 quantisation is coarse enough that two colours a designer can tell
# apart in RGB can collapse onto the SAME 565 value: the first draft of this
# palette had the floor panel face and the wall base quantise together, which
# made the floor look like more wall. Picking the 565 value directly means what
# is chosen here is exactly what ships, and Pillow only ever sees the 8-bit
# expansion of a colour the panel can actually show.

def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # 0xF81F is the chroma key drawSprite() skips. A tile is opaque by
    # definition, so a tile pixel that happened to land on the key would punch
    # a transparent hole in the floor.
    return 0xF81E if value == 0xF81F else value


def rgb(c565):
    """8-bit expansion of an RGB565 value, for Pillow to draw with."""
    r = ((c565 >> 11) & 0x1F) * 255 // 31
    g = ((c565 >> 5) & 0x3F) * 255 // 63
    b = (c565 & 0x1F) * 255 // 31
    return (r, g, b)


WALL_BASE   = rgb(0x10A4)   # cold navy panel
WALL_SEAM   = rgb(0x18E6)   # one step up: the seam between panels
WALL_RIVET  = rgb(0x2969)   # two steps up: a stud catching the light
FLOOR_BASE  = rgb(0x0862)   # darker and greyer than the wall
FLOOR_FACE  = rgb(0x0883)   # the lifted face of a floor panel
# Dim teal lattice between floor panels. Deliberately darker than it "wants"
# to be: on a 160x80 the tile is 8 px, so the lattice repeats every 8 px right
# behind the 8 px item icons. The first draft was two steps brighter and the
# floor competed with the objects standing on it.
FLOOR_SEAM  = rgb(0x00C5)
RAIL_LIT    = rgb(0x330E)   # the one lit edge in the room
RAIL_SHADOW = rgb(0x0021)   # the shadow it casts

# Exported to the header for the primitives ui_den.cpp draws itself.
EXPORT = [
    ("DEN_COL_WALL",   WALL_BASE,   "wall base; text plate for a wall-row label"),
    ("DEN_COL_FLOOR",  FLOOR_FACE,  "floor panel face; text plate for a floor-row label"),
    ("DEN_COL_SEAM",   WALL_SEAM,   "panel seam; empty-slot bracket, unlit"),
    ("DEN_COL_RIVET",  WALL_RIVET,  "wall rivet; empty-slot bracket, cursor"),
    ("DEN_COL_RAIL",   RAIL_LIT,    "lit junction edge; shelf a wall row sits on"),
    ("DEN_COL_SHADOW", RAIL_SHADOW, "under-rail shadow; the pet's grounding shadow"),
]


# ── Tile painters ─────────────────────────────────────────────────────────
# Each paints one SUPER x SUPER canvas that tiles seamlessly with itself. Every
# coordinate is a multiple of UNIT so the feature survives both downscales.

def t_wall(d, fine):
    d.rectangle([0, 0, SUPER, SUPER], fill=WALL_BASE)
    # Seams on the top and left edges only: tiling supplies the other two, so
    # the lattice is one pixel wide rather than two.
    d.rectangle([0, 0, SUPER, UNIT - 1], fill=WALL_SEAM)
    d.rectangle([0, 0, UNIT - 1, SUPER], fill=WALL_SEAM)
    # One rivet, centred, so a wall of tiles has a regular stud pattern.
    d.rectangle([3 * UNIT, 3 * UNIT, 4 * UNIT - 1, 4 * UNIT - 1], fill=WALL_RIVET)
    if not fine:
        return
    # The seam is LIT on the top and left, so the opposite edges of the panel
    # face are where its shadow falls. One texel of it turns a flat fill with
    # lines on it into a plate sitting in a frame - and because the next tile's
    # lit seam butts straight onto this shadow, the pair reads as a real groove.
    d.rectangle([UNIT, SUPER - fine, SUPER, SUPER], fill=RAIL_SHADOW)
    d.rectangle([SUPER - fine, UNIT, SUPER, SUPER], fill=RAIL_SHADOW)
    # Same light on the rivet: a drop shadow below and right makes it a stud
    # standing off the wall instead of a bright square painted on it.
    d.rectangle([3 * UNIT + fine, 4 * UNIT, 4 * UNIT + fine - 1, 4 * UNIT + fine - 1],
                fill=RAIL_SHADOW)
    d.rectangle([4 * UNIT, 3 * UNIT + fine, 4 * UNIT + fine - 1, 4 * UNIT + fine - 1],
                fill=RAIL_SHADOW)


def t_floor(d, fine):
    d.rectangle([0, 0, SUPER, SUPER], fill=FLOOR_BASE)
    # A lifted face inside the lattice reads as a floor panel rather than as a
    # flat fill with lines on it.
    d.rectangle([UNIT, UNIT, SUPER - UNIT - 1, SUPER - UNIT - 1], fill=FLOOR_FACE)
    d.rectangle([0, 0, SUPER, UNIT - 1], fill=FLOOR_SEAM)
    d.rectangle([0, 0, UNIT - 1, SUPER], fill=FLOOR_SEAM)
    if not fine:
        return
    # The floor face is LIFTED, so its far edges fall away into the lattice.
    # One texel of the seam colour tucked inside the bottom and right edges is
    # what gives it a thickness; without it the face and the base are two flat
    # greys meeting at a hard line, which reads as a hole rather than a panel.
    d.rectangle([UNIT, SUPER - UNIT - fine, SUPER - UNIT - 1, SUPER - UNIT - 1],
                fill=FLOOR_SEAM)
    d.rectangle([SUPER - UNIT - fine, UNIT, SUPER - UNIT - 1, SUPER - UNIT - 1],
                fill=FLOOR_SEAM)


def t_rail(d, fine):
    # The horizon, one tile tall. Half wall, a lit edge, a shadow, then floor.
    d.rectangle([0, 0, SUPER, 4 * UNIT - 1], fill=WALL_BASE)
    d.rectangle([0, 0, UNIT - 1, 4 * UNIT - 1], fill=WALL_SEAM)
    d.rectangle([0, 4 * UNIT, SUPER, 5 * UNIT - 1], fill=RAIL_LIT)
    d.rectangle([0, 5 * UNIT, SUPER, 6 * UNIT - 1], fill=RAIL_SHADOW)
    d.rectangle([0, 6 * UNIT, SUPER, SUPER], fill=FLOOR_BASE)
    d.rectangle([0, 6 * UNIT, UNIT - 1, SUPER], fill=FLOOR_SEAM)
    if not fine:
        return
    # The only lit edge in the room went straight from navy to teal in one step.
    # A single texel of the rivet tone above it is the lip the light is catching
    # on, and it is the difference between a horizon and a stripe.
    d.rectangle([0, 4 * UNIT - fine, SUPER, 4 * UNIT - 1], fill=WALL_RIVET)
    # And one texel of the floor lattice under the shadow, so the floor starts
    # at a joint rather than at a colour change.
    d.rectangle([0, 6 * UNIT, SUPER, 6 * UNIT + fine - 1], fill=FLOOR_SEAM)


TILES = [
    ("den_tile_wall",  "back wall: panelled, rivet centred, seams top and left",
     t_wall),
    ("den_tile_floor", "floor: lattice with a lifted panel face", t_floor),
    ("den_tile_rail",  "wall/floor junction: the room's horizon", t_rail),
]


def render(painter, size):
    """Paint one tile canvas for a given OUTPUT size.

    The canvas is rendered per size rather than once and downscaled three ways,
    because the fine detail is a function of the output size: below 32 px it is
    not drawn at all. Passing fine=0 for the 8 and 16 px sets makes the painters
    take exactly the code path they took before this existed, which is what
    keeps those two arrays byte-identical.
    """
    img = Image.new("RGB", (SUPER, SUPER), (0, 0, 0))
    painter(ImageDraw.Draw(img), FINE if size >= 32 else 0)
    return img


def quantize(img, size):
    # BOX, not LANCZOS: an area average never reaches past the tile edge, which
    # is what keeps the tile seamless against a copy of itself.
    small = img.resize((size, size), Image.BOX)
    out = []
    for y in range(size):
        for x in range(size):
            out.append(rgb565(*small.getpixel((x, y))))
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


def build():
    p = [
        "#pragma once",
        "// ── HexHound - Den Room Tiles (RGB565) ──────────────────────",
        "//",
        "// GENERATED FILE - do not hand-edit.",
        "//   generator: scripts/gen_den_art.py  (draws them, no source PNG)",
        "//",
        "// Include from ONE .cpp only (ui_den.cpp), same rule as item_art.h and",
        "// sprites.h: these are `static const`, so a second translation unit that",
        "// included them would bake a second copy of every tile into flash.",
        "//",
        "// Three SEAMLESS tiles, not a backdrop image. RGB565 is 2 bytes per pixel,",
        "// so a full-screen backdrop would be 25,600 bytes on a 160x80 panel and",
        "// 115,200 on the round one, and it would need redrawing for every new panel.",
        "// A tile set fills a room of any size on any panel for a few hundred bytes.",
        "//",
        "// Only ONE size is compiled, chosen by panel family, matching item_art.h:",
        "// 8 px on a 160x80, 16 px on the big and 240 round panels, 32 px on the",
        "// 480 round one. Every panel bakes its tiles at the size it DRAWS them, so",
        "// DEN_TILE_PX is both the source and the destination size and ui_den.cpp",
        "// no longer enlarges anything.",
        "//",
        "// The 32 px tiles are NOT a re-encoding of the 16 px ones: a plain re-run",
        "// of the painters at 32 was measured against a 2x enlargement and came out",
        "// byte-identical, 0 of 1024 texels, because every coarse feature sits on a",
        "// whole number of output pixels at both sizes. The 32 px set earns its",
        "// flash by carrying a one-texel bevel on the panel joints and a lip on the",
        "// horizon - half a texel at 16 px, so physically unrepresentable there.",
        "//",
        "//   8 px set:    384 bytes of flash",
        "//   16 px set:  1536 bytes of flash",
        "//   32 px set:  6144 bytes of flash",
        "//",
        "// The tiles are OPAQUE - no chroma key. They are a backdrop, so ui_den.cpp",
        "// blits them with its own clipping tiler rather than drawSprite().",
        "// ─────────────────────────────────────────────────────────────────",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        "",
        '#include "../board/board_profile.h"   // SCREEN_W/H, HEXHOUND_PANEL_ROUND',
        "",
        "#if HEXHOUND_PANEL_ROUND && (SCREEN_W >= 480)",
        "#define DEN_TILE_PX 32",
        "#elif HEXHOUND_PANEL_ROUND || (SCREEN_H > 100)",
        "#define DEN_TILE_PX 16",
        "#else",
        "#define DEN_TILE_PX 8",
        "#endif",
        "",
        "// Palette, exported so the primitives ui_den.cpp draws itself (empty-slot",
        "// brackets, the wall shelf, the pet's shadow) are the same colours as the",
        "// tiles instead of a second copy that goes stale.",
    ]
    for name, colour, note in EXPORT:
        p.append(f"#define {name:<15s} 0x{rgb565(*colour):04X}   // {note}")
    p.append("")

    for size in SIZES:
        p.append(f"#if DEN_TILE_PX == {size}")
        p.append("")
        for name, note, painter in TILES:
            print(f"  {name:16s} {size}x{size}")
            p.append(f"// {note}")
            p.append(emit_array(name, quantize(render(painter, size), size), size))
            p.append("")
        p.append("#endif")
        p.append("")

    return "\n".join(p)


def main():
    print("=== HexHound den art generator ===")
    text = build()
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    for size in SIZES:
        print(f"  {size:2d}px set: {len(TILES) * size * size * 2} bytes of flash")
    print(f"\nwrote {OUT_PATH} ({os.path.getsize(OUT_PATH) / 1024.0:.1f} KB source)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
