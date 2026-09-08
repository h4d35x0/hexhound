#!/usr/bin/env python3
"""Convert the HD stage art in assets/hd/ into RGB565 arrays for the firmware.

HexHound has no PNG decoder and no guaranteed SD card, so the HD pet art is
baked into flash as PROGMEM arrays, exactly like the pixel sprites in
src/ui/sprites.h.

One header per panel family is produced, so a build only carries the sizes its
panel uses:

  src/ui/sprites_hd_small.h     24px + 48px    for the 160x80 panels
  src/ui/sprites_hd_big.h       64px + 112px   for the 320x172 / 320x170 panels
  src/ui/sprites_hd_round.h     88px + 160px   for the 240x240 round panels
  src/ui/sprites_hd_round480.h  176px + 320px  for the 480x480 round panel

Each variant names the source PNG it is cut from. The 480 round panel draws its
art at twice the size the 240 one does, and a 200px master cannot fill 320px of
glass with anything but a blur, so it is cut from the 400px masters instead.
Those are the SAME art from the SAME 960x960 supersampled canvas in the
sibling project's generator, downsampled once instead of twice, which is why the 480
panel gets real detail rather than an upscale.

The sizes are not arbitrary - they match the layout slots that already exist:
  home  32 / 64   ui_home.cpp draws the resting pet at 32px (corner, small
                  panels) and 64px (centred lower area, big panels)
  hero  48 / 112  ui_evolve.cpp heroScale() fills the screen for the evolution
                  reveal at those sizes

Transparency: the UI background is solid black, so semi-transparent edge pixels
are composited over black (which preserves the antialiasing and the neon bloom)
and only near-fully-transparent pixels become the magenta chroma key the
existing drawSprite() already skips.

Usage:
    pip install Pillow
    python scripts/gen_hd_sprites.py
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_DIR = os.path.join(PROJECT_DIR, "assets", "hd")
OUT_DIR = os.path.join(PROJECT_DIR, "src", "ui")

# Stage order must match PetStage in src/pet/pet_core.h.
STAGES = ["egg", "pup", "beast", "gremlin", "sentinel"]

# The master PNGs. 200 is the historic set every shipping panel is cut from;
# the 400 set exists only because the 480 round panel draws art bigger than that.
SRC_200 = 200
SRC_400 = 400

# (header filename, home size, hero size, guard comment, source master edge)
VARIANTS = [
    # 24px is what the 160x80 layout can actually spare: the free block beside
    # the stat bars is x=128..152, y=10..34. Bigger would collide with the
    # battery line above or the TRU/MIS/NRG bars below.
    ("sprites_hd_small.h", 24, 48, "160x80 panels (T-Dongle S3, T-Dongle C5)", SRC_200),
    ("sprites_hd_big.h", 64, 112, "320x172 / 320x170 panels (Waveshare, T-Display S3)", SRC_200),
    # On a 240x240 round panel the pet IS the screen - there is no side column
    # to compete with it - so the home portrait is much larger than on the
    # landscape boards. 160px is the largest hero that still clears the bezel:
    # the inscribed square of a 240 circle is 170px.
    ("sprites_hd_round.h", 88, 160, "240x240 round panels (Waveshare ESP32-S3-LCD-1.28)", SRC_200),
    # The 480x480 round panel (LilyGo T-RGB) runs the SAME layout at twice the
    # scale, so both slots are exactly double the 240 ones. Baking them means
    # the drawn size equals the baked size again, so drawHDArt() goes back to
    # its plain blit and the art stops being a 2x nearest-neighbour smear.
    # 1.3 MB of flash, against ~1.5 MB of app in a 6.25 MB partition.
    ("sprites_hd_round480.h", 176, 320, "480x480 round panel (LilyGo T-RGB)", SRC_400),
]

TRANSPARENT_COLOR = 0xF81F  # magenta chroma key, matches sprites.h
ALPHA_CUTOFF = 24           # below this the pixel is background, not art
BG = (0, 0, 0)              # UI background is black


def source_png(stage, src_px):
    """Master art for one stage at one master size.

    The 200px set keeps its historic filename; any other size is suffixed, so
    the two live side by side in assets/hd/ and adding a master cannot silently
    re-cut a shipping panel's art from a different source.
    """
    stem = f"asset_{stage}" if src_px == 200 else f"asset_{stage}_{src_px}"
    return os.path.join(ASSET_DIR, f"{stem}.png")


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Never emit the chroma key as a real colour, or that pixel goes invisible.
    return 0xF81E if value == TRANSPARENT_COLOR else value


def convert(png_path, size):
    img = Image.open(png_path).convert("RGBA")
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
                r = int(r * f + BG[0] * (1 - f))
                g = int(g * f + BG[1] * (1 - f))
                b = int(b * f + BG[2] * (1 - f))
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


def build_header(filename, home_size, hero_size, panels, src_px):
    src_name = "asset_<stage>.png" if src_px == 200 else f"asset_<stage>_{src_px}.png"
    parts = [
        "#pragma once",
        "// ── HexHound - HD Pet Art (RGB565) ───────────────────────────────",
        "//",
        "// GENERATED FILE - do not hand-edit.",
        f"//   source:    assets/hd/{src_name} ({src_px}x{src_px} transparent)",
        f"//   generator: scripts/gen_hd_sprites.py",
        f"//   panels:    {panels}",
        "//",
        "// Include from ONE .cpp only (animator.cpp), same rule as sprites.h.",
        "// Transparent pixels use the 0xF81F chroma key that drawSprite() skips.",
        "// ────────────────────────────────────────────────────────────────────────────",
        "",
        "#ifndef SIMULATOR_BUILD",
        "#include <pgmspace.h>",
        "#endif",
        "#include <cstdint>",
        "",
        f"#define HD_HOME_SIZE {home_size}",
        f"#define HD_HERO_SIZE {hero_size}",
        "",
    ]

    for stage in STAGES:
        png = source_png(stage, src_px)
        if not os.path.exists(png):
            sys.exit(f"missing source art: {png}")
        for label, size in (("home", home_size), ("hero", hero_size)):
            print(f"  {stage:9s} {label} {size}x{size}")
            parts.append(emit_array(f"hd_{stage}_{label}", convert(png, size), size))
            parts.append("")

    for label in ("home", "hero"):
        parts.append(f"static const uint16_t* const hd_{label}_by_stage[5] = {{")
        for stage in STAGES:
            parts.append(f"    hd_{stage}_{label},")
        parts.append("};")
        parts.append("")

    return "\n".join(parts)


def main():
    print("=== HexHound HD sprite generator ===")
    for filename, home_size, hero_size, panels, src_px in VARIANTS:
        print(f"\n{filename}  ({panels}, from the {src_px}px masters)")
        text = build_header(filename, home_size, hero_size, panels, src_px)
        out_path = os.path.join(OUT_DIR, filename)
        with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        kb = os.path.getsize(out_path) / 1024.0
        flash = 5 * (home_size ** 2 + hero_size ** 2) * 2 / 1024.0
        print(f"  wrote {out_path} ({kb:.0f} KB source, {flash:.0f} KB of flash)")
    print("\nDone. Rebuild the firmware targets to pick the new art up.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
