#!/usr/bin/env python3
"""Draw the six behavioural-form badges and bake them into src/ui/form_art.h.

A form badge is the small mark that says which of the six PetForm identities a
pet has grown into. Form is DISPLAY ONLY - a title, an accent colour and this
badge - so the art is the whole of its footprint, and the footprint is the
design constraint.

Style follows assets/hd/README.md: procedural Pillow drawing, transparent PNG,
neon over a black UI background, supersampled then downscaled so the glow and
the antialiasing survive. Unlike the stage art, both halves live here - this
script DRAWS the PNGs and BAKES the header, because nothing upstream owns badge
art and a second generator would only be a place for the two to disagree.

  scripts/gen_hd_sprites.py and src/ui/sprites_hd_*.h belong to the stage art
  and are NOT touched by this script. Same pipeline, separate outputs.

SIZE IS THE POINT. RGB565 is two bytes per pixel with no compression, so a
badge costs 2 * size^2 bytes of flash EACH and there are six of them. At 20px
that is 4800 bytes total; at 64px it would be 49152, which is an absurd price
for a decoration in the corner of a 160x80 panel. Do not raise BADGE_SIZE
without recomputing what it buys.

Usage:
    pip install Pillow
    python scripts/gen_form_badges.py
"""

import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_DIR = os.path.join(PROJECT_DIR, "assets", "forms")
OUT_HEADER = os.path.join(PROJECT_DIR, "src", "ui", "form_art.h")

# Baked badge edge, in pixels. See the flash note in the docstring.
BADGE_SIZE = 20
# Supersample factor. 12 gives a 240px working canvas, which is enough for the
# glow blur to be smooth after the downscale without the draw taking any real
# time.
SS = 12
CANVAS = BADGE_SIZE * SS

TRANSPARENT_COLOR = 0xF81F   # magenta chroma key, matches sprites.h
ALPHA_CUTOFF = 24            # below this the pixel is background, not art
BG = (0, 0, 0)               # UI background is black

# Order matches PetForm - 1 in src/config.h. The colours are the same six
# accents as FORM_ACCENT in src/pet/pet_forms.cpp; if one changes, change both,
# or the badge and the title beside it will disagree.
FORMS = [
    ("pathfinder", (0x00, 0xE5, 0xC8)),
    ("guardian",   (0x3C, 0x8C, 0xFF)),
    ("archivist",  (0xB4, 0x78, 0xFF)),
    ("cipher",     (0xB4, 0xFF, 0x32)),
    ("gremlin",    (0xFF, 0x4F, 0xC8)),
    ("packmaster", (0xFF, 0x9B, 0x32)),
]


# ── Drawing ───────────────────────────────────────────────────────────────
#
# Every glyph is drawn into its own RGBA layer at canvas scale. Two copies of
# each layer are composited: a heavily blurred one for the neon bloom, and a
# crisp one on top for the edge. That is what makes a 20px badge still read as
# lit rather than as a smudge.


def hexagon(cx, cy, r):
    """Flat-top hexagon. HexHound's shape, so every badge shares a silhouette
    and the six read as one set rather than six unrelated icons."""
    import math
    pts = []
    for i in range(6):
        a = math.radians(60 * i)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def draw_pathfinder(d, cx, cy, r, colour):
    # A navigator's needle: tip out, notched tail. Travel, not speed.
    d.polygon([
        (cx, cy - r * 0.88),
        (cx + r * 0.60, cy + r * 0.66),
        (cx, cy + r * 0.24),
        (cx - r * 0.60, cy + r * 0.66),
    ], fill=colour)


def draw_guardian(d, cx, cy, r, colour):
    # A shield, drawn as an outline rather than a slab: a guard watches, it
    # does not fill the frame.
    d.polygon([
        (cx - r * 0.58, cy - r * 0.66),
        (cx + r * 0.58, cy - r * 0.66),
        (cx + r * 0.58, cy + r * 0.08),
        (cx, cy + r * 0.80),
        (cx - r * 0.58, cy + r * 0.08),
    ], outline=colour, width=int(r * 0.20))


def draw_archivist(d, cx, cy, r, colour):
    # Three filed layers, narrowing upward: a stack that someone keeps in
    # order, not a pile.
    widths = (0.70, 0.54, 0.38)
    for i, w in enumerate(widths):
        y = cy + r * (0.42 - i * 0.42)
        d.rounded_rectangle(
            [cx - r * w, y - r * 0.13, cx + r * w, y + r * 0.13],
            radius=r * 0.12, fill=colour)


def draw_cipher(d, cx, cy, r, colour):
    # A keyhole. Something that opens only once you have understood it.
    d.ellipse([cx - r * 0.38, cy - r * 0.70, cx + r * 0.38, cy + r * 0.06],
              outline=colour, width=int(r * 0.22))
    d.polygon([
        (cx - r * 0.16, cy + r * 0.02),
        (cx + r * 0.16, cy + r * 0.02),
        (cx + r * 0.30, cy + r * 0.76),
        (cx - r * 0.30, cy + r * 0.76),
    ], fill=colour)


def draw_gremlin(d, cx, cy, r, colour):
    # A bolt. Mischief that went somewhere.
    d.polygon([
        (cx + r * 0.30, cy - r * 0.84),
        (cx - r * 0.44, cy + r * 0.12),
        (cx - r * 0.04, cy + r * 0.12),
        (cx - r * 0.26, cy + r * 0.86),
        (cx + r * 0.48, cy - r * 0.14),
        (cx + r * 0.06, cy - r * 0.14),
    ], fill=colour)


def draw_packmaster(d, cx, cy, r, colour):
    # Three nodes, linked. A pack, not a crowd.
    nodes = [
        (cx, cy - r * 0.60),
        (cx - r * 0.52, cy + r * 0.40),
        (cx + r * 0.52, cy + r * 0.40),
    ]
    for a in range(3):
        d.line([nodes[a], nodes[(a + 1) % 3]], fill=colour,
               width=int(r * 0.14))
    for (nx, ny) in nodes:
        d.ellipse([nx - r * 0.22, ny - r * 0.22, nx + r * 0.22, ny + r * 0.22],
                  fill=colour)


GLYPHS = {
    "pathfinder": draw_pathfinder,
    "guardian":   draw_guardian,
    "archivist":  draw_archivist,
    "cipher":     draw_cipher,
    "gremlin":    draw_gremlin,
    "packmaster": draw_packmaster,
}


def render(name, rgb):
    """Draw one badge as a transparent RGBA image at CANVAS resolution."""
    cx = cy = CANVAS / 2.0
    accent = rgb + (255,)
    # The ring is dimmer than the glyph so the glyph is what the eye lands on
    # at 20px, where the ring is barely three pixels thick.
    ring = tuple(int(c * 0.62) for c in rgb) + (255,)

    layer = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    d.polygon(hexagon(cx, cy, CANVAS * 0.46), outline=ring,
              width=int(CANVAS * 0.055))
    GLYPHS[name](d, cx, cy, CANVAS * 0.34, accent)

    # Neon bloom: a blurred copy of the same art underneath itself. Blurring a
    # copy rather than the art means the crisp edge survives the downscale,
    # which is the whole reason a 20px badge still looks lit.
    glow = layer.filter(ImageFilter.GaussianBlur(CANVAS * 0.045))
    glow.putalpha(glow.getchannel("A").point(lambda a: int(a * 0.75)))

    out = Image.alpha_composite(glow, layer)
    return out


# ── Baking ────────────────────────────────────────────────────────────────


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Never emit the chroma key as a real colour, or that pixel goes invisible.
    return 0xF81E if value == TRANSPARENT_COLOR else value


def convert(img, size):
    img = img.convert("RGBA").resize((size, size), Image.LANCZOS)
    out = []
    for y in range(size):
        for x in range(size):
            r, g, b, a = img.getpixel((x, y))
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
        for i in range(0, size, 10):
            lines.append("    " + ",".join(f"0x{v:04X}"
                                           for v in chunk[i:i + 10]) + ",")
    lines.append("};")
    return "\n".join(lines)


def build_header(baked):
    flash = len(FORMS) * BADGE_SIZE * BADGE_SIZE * 2
    parts = [
        "#pragma once",
        "// ── HexHound - Form Badges (RGB565) ──────────────────────────────",
        "//",
        "// GENERATED FILE - do not hand-edit.",
        "//   source:    assets/forms/badge_<form>.png (transparent, procedural)",
        "//   generator: scripts/gen_form_badges.py",
        "//",
        "// One small badge per PetForm, indexed by (PetForm - 1), which is the same",
        "// index PetForms::formIndex() returns. FORM_UNSET has no badge on purpose:",
        "// a pet that has not become anything yet should not be wearing a mark that",
        "// says so, and there is deliberately no progress art toward one.",
        "//",
        "// This header is NOT related to sprites_hd_*.h. Those are the stage art and",
        "// are generated by scripts/gen_hd_sprites.py; these are the form art. Stage",
        "// and form are independent axes and their art pipelines stay that way.",
        "//",
        f"// Cost: {len(FORMS)} badges x {BADGE_SIZE}x{BADGE_SIZE} x 2 bytes = "
        f"{flash} bytes of flash, and only in a",
        "// build that includes this header. RGB565 is uncompressed, so the size is",
        "// exactly 2 * edge^2 each - raising the edge is quadratic, not free.",
        "//",
        "// Include from ONE .cpp only, the same rule as sprites.h and sprites_hd_*.h:",
        "// these are file-scope arrays and every extra translation unit that includes",
        "// them pays for a whole extra copy.",
        "//",
        "// Transparent pixels use the 0xF81F chroma key that drawSprite() skips, so a",
        "// badge composites over whatever is already on screen with no mask.",
        "// ────────────────────────────────────────────────────────────────────────────",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        "",
        f"#define FORM_BADGE_SIZE {BADGE_SIZE}",
        f"#define FORM_BADGE_COUNT {len(FORMS)}",
        "",
    ]

    for name, values in baked:
        parts.append(emit_array(f"form_badge_{name}", values, BADGE_SIZE))
        parts.append("")

    parts.append("// Indexed by PetForms::formIndex(), i.e. (PetForm - 1).")
    parts.append("static const uint16_t* const "
                 "form_badge_by_index[FORM_BADGE_COUNT] = {")
    for name, _ in baked:
        parts.append(f"    form_badge_{name},")
    parts.append("};")
    parts.append("")

    return "\n".join(parts)


def main():
    print("=== HexHound form badge generator ===")
    os.makedirs(ASSET_DIR, exist_ok=True)

    baked = []
    for name, rgb in FORMS:
        img = render(name, rgb)
        png = os.path.join(ASSET_DIR, f"badge_{name}.png")
        img.save(png)
        baked.append((name, convert(img, BADGE_SIZE)))
        print(f"  {name:11s} {png}  -> {BADGE_SIZE}x{BADGE_SIZE}")

    text = build_header(baked)
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)

    kb = os.path.getsize(OUT_HEADER) / 1024.0
    flash = len(FORMS) * BADGE_SIZE * BADGE_SIZE * 2
    print(f"\nwrote {OUT_HEADER} ({kb:.0f} KB source, {flash} bytes of flash)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
