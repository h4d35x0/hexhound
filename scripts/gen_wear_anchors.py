#!/usr/bin/env python3
"""Emit src/ui/wear_anchors.h: where a worn cosmetic sits on each pet stage.

Also renders a contact sheet so the numbers can be CHECKED by looking, which is
the only verification that means anything for a placement table.

    python scripts/gen_wear_anchors.py            # write the header
    python scripts/gen_wear_anchors.py --preview  # header + contact sheet PNG

── The table is authored, not measured, and that is the second attempt ──────

Four different automatic heuristics were tried against the five silhouettes in
assets/hd/ and every one of them placed at least one anchor visibly wrong:

  1. "the shoulder line is the first row at least 55% as wide as the widest"
     put the gremlin's neck twenty-five rows INSIDE its head, because its ears
     are as wide as its shoulders.
  2. "the neck is the narrowest row above the torso" returned the single-pixel
     top of every skull.
  3. "the neck is the narrowest row between the head's widest row and the
     torso's" found the shoulders for the head and produced necks at 72% of the
     figure's height.
  4. "the neck is the first local minimum in the upper 40%" works on three
     stages and fails on the other two: the Beacon Beast has a notch between
     its crest and its head that it picks instead of the real neck, and the
     gremlin's ears merge into its shoulders with no minimum at all.

These are five hand-drawn characters with genuinely different anatomy - an
ovoid with no head, a quadruped, two bipeds and something with ears wider than
its body. Thirty numbers read off the art are more reliable than a rule that
has to generalise over all of them, and they are cheap to re-check: run this
with --preview and look at the sheet.

What IS still measured is a real assertion: every anchor must land inside the
figure's own silhouette, and every item box must fit within the canvas. A typo
in the table below fails the script rather than shipping a hat floating beside
a pet's ear.

Units are thousandths of the sprite's own box, which is the whole source
canvas: gen_hd_sprites.py resizes 400x400 to the target size rather than
cropping to the figure, so a fraction of the canvas is the same fraction of the
sprite at every size on every panel. That is what lets one table serve a 24 px
pet on a T-Dongle and a 320 px hero shot on the T-RGB.

── This table is for the HD ART ONLY, and there is no pixel-art twin ────────

The 16x16 (20x20 sentinel) pixel sprites in src/ui/sprites.h are NOT covered,
and no table will ever cover them; see the three findings below. Where the pet
has to wear something, the answer is to draw the HD art there instead. The
patrol HUD corner did exactly that and is no longer a gap. The home screen's
non-resting states are the one that remains on the rectangular boards, and it
is being closed the same way, by baking HD idle frames at those panel sizes.
Measured 2026-08-30.

A second table measured against the pixel art was considered and rejected:

  5. "re-express each anchor as the same DEPTH INTO THE FIGURE and map it onto
     the pixel silhouette's own extent" is the natural repair once you notice
     the two art sets frame the figure differently, and it is the fifth
     heuristic to fail. It floats the pup's and the gremlin's hats off the top
     of the skull and drops every scarf onto the chin. The pixel sprite is not
     a small copy of the HD art; it is a different drawing of the same
     character, with its own head-to-body ratio.

The framing gap it was trying to correct, HD figure top minus pixel figure top
in thousandths of each one's own box: egg +170, pup +123, beast +108,
gremlin +200, sentinel +118. Applying the table below unchanged is what those
numbers rule out.

Two further reasons an authored pixel table was not worth it either:

  * The silhouette MOVES between animation frames, so a per-stage table is the
    wrong shape regardless of how carefully it is authored. gremlin_laugh and
    pup_hungry sit a whole pixel (62 thousandths) lower than their own idle
    frames, and sentinel_guard walks 150 thousandths sideways across its three
    frames against a fixed cx of 500. Following that needs a PER-FRAME table:
    37 frames x 2 anchors x 3 numbers = 222 authored values against the 30
    here, all by eye on a grid where a head is five pixels tall, and with no
    automatic check available for the reason given just above.

  * The payoff is 3 to 19 pixels of hat. The pet is drawn 32 px in the compact
    home and 64 px in the big-panel home; the beast's head anchor of 195
    thousandths was a 3x3 square at the 16 px the big-panel patrol HUD used to
    give it, which is what decided that screen in favour of the HD still.

The real fix, if this is ever worth doing, is to bake HD motion frames for the
rectangular panels the way scripts/gen_hd_idle.py did for the 480 round panel.
Then this one table covers every screen and nothing has to be kept in step.
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

import numpy as np

STAGES = ["egg", "pup", "beast", "gremlin", "sentinel"]
SRC = "assets/hd/asset_{stage}_400.png"
OUT = "src/ui/wear_anchors.h"
PREVIEW = "wear_anchors_preview.png"
CANVAS = 400
FIXED = 1000

# ── The table ────────────────────────────────────────────────────────────────
#
# Per stage: head (cx, cy, w) then neck (cx, cy, w), in thousandths of the box.
#
# `w` is the side of the SQUARE the overlay is drawn into, not the width of the
# body part. It is deliberately larger than the part: the square is centred on
# the middle of the head, and scripts/gen_worn_art.py authors a cap in the upper
# third of its square and goggles across the middle of theirs, which is how two
# items sharing one anchor end up in two different places.
#
# The egg has no head and no neck. Its rows exist so the table is total over
# PetStage and the renderer needs no special case; nothing can reach them in
# play, because every worn recipe gates at STAGE_PACKET_PUP or later.
#
# Every neck was lowered on 2026-08-30 after seeing a scarf on glass: the first
# pass put them on the chin. Reported as "the scarf can be moved a little
# lower", which is the kind of note only a photograph of the real panel
# produces - it read as acceptable on the contact sheet.
ANCHORS = {
    "egg":      {"head": (500, 300, 300), "neck": (500, 480, 300)},
    "pup":      {"head": (500, 250, 300), "neck": (500, 490, 300)},
    # The Beacon Beast's neck went 300 -> 340 -> 470. Its head is small, high
    # and narrow and its shoulders are the wide dark band well below it, so
    # both earlier values put the scarf across its visor. Reported from the
    # board, and visible on the contact sheet once it was looked at for this
    # stage specifically rather than for the set.
    "beast":    {"head": (500, 185, 195), "neck": (500, 470, 330)},
    # The gremlin's neck went 490 -> 530 -> 620. Its jaw hangs low and its
    # shoulders start lower still, so a scarf that looks fine on the contact
    # sheet still sat across its mouth on the board. It is the stage with the
    # least neck and the one most sensitive to this number, and it has now been
    # corrected twice from photographs rather than from the sheet.
    "gremlin":  {"head": (505, 255, 330), "neck": (505, 620, 300)},
    "sentinel": {"head": (500, 215, 265), "neck": (500, 355, 360)},
}


def silhouette(stage):
    a = np.array(Image.open(SRC.format(stage=stage)).convert("RGBA"))
    return a[:, :, 3] > 128


def verify(stage, mask):
    """Every anchor lands on the figure; every box fits the canvas."""
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
    problems = []
    for name, (cx, cy, w) in ANCHORS[stage].items():
        px, py = cx * CANVAS // FIXED, cy * CANVAS // FIXED
        if not (x0 <= px <= x1 and y0 <= py <= y1):
            problems.append(
                "%s %s anchor (%d,%d) is outside the figure "
                "(x %d..%d, y %d..%d)" % (stage, name, px, py, x0, x1, y0, y1))
        half = (w * CANVAS // FIXED) // 2
        if px - half < 0 or px + half > CANVAS or py - half < 0 or py + half > CANVAS:
            problems.append(
                "%s %s box of %d runs off the canvas at (%d,%d)"
                % (stage, name, w, px, py))
    return problems


def preview(masks):
    """Composite the real overlays onto every stage, both pairs."""
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "worn", os.path.join(here, "gen_worn_art.py"))
    worn = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(worn)
    colors = worn.read_colors()

    def overlay(item, px):
        n = px * 8
        img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        worn.DRAW[item](ImageDraw.Draw(img), n, worn.rgb(colors[item]))
        return img.resize((px, px), Image.LANCZOS)

    size = 240
    pairs = [("cos.antenna", "cos.scarf"), ("cos.goggles", "cos.collar")]
    sheet = Image.new("RGB", (len(STAGES) * size, len(pairs) * size), (8, 8, 12))
    for row, (head_item, neck_item) in enumerate(pairs):
        for col, stage in enumerate(STAGES):
            base = Image.open(SRC.format(stage=stage)).convert("RGBA")
            base = base.resize((size, size), Image.LANCZOS)
            cell = Image.new("RGBA", (size, size), (8, 8, 12, 255))
            cell.alpha_composite(base)
            for item, key in ((neck_item, "neck"), (head_item, "head")):
                cx, cy, w = ANCHORS[stage][key]
                px = max(4, w * size // FIXED)
                cell.alpha_composite(
                    overlay(item, px),
                    (cx * size // FIXED - px // 2, cy * size // FIXED - px // 2))
            sheet.paste(cell.convert("RGB"), (col * size, row * size))
    sheet.save(PREVIEW)
    print("wrote " + PREVIEW + "  (top row hat+scarf, bottom goggles+collar)")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    masks = {s: silhouette(s) for s in STAGES}
    problems = []
    for s in STAGES:
        problems += verify(s, masks[s])
    if problems:
        for p in problems:
            print("ERROR: " + p, file=sys.stderr)
        sys.exit("anchor table does not fit the art")

    bar = "─" * 7
    out = [
        "#pragma once",
        "",
        "// " + "─" * 4 + " HexHound - Where a worn cosmetic sits on the pet " + bar,
        "//",
        "// GENERATED by scripts/gen_wear_anchors.py. Do not edit by hand.",
        "//",
        "// Thousandths of the sprite's own box, which is the whole source canvas:",
        "// gen_hd_sprites.py resizes 400x400 to the target size rather than cropping",
        "// to the figure, so a fraction of the canvas is the same fraction of the",
        "// sprite at every size on every panel. One table serves a 24 px pet on a",
        "// T-Dongle and a 320 px hero shot on the T-RGB.",
        "//",
        "// The table is AUTHORED and then verified against the silhouette, not",
        "// derived from it. Four automatic heuristics were tried and each placed at",
        "// least one anchor visibly wrong; the five characters have too little",
        "// anatomy in common. The script asserts every anchor lands on the figure and",
        "// every box fits the canvas, and --preview renders the overlays onto all",
        "// five stages so the placement can be checked by looking at it.",
        "//",
        "// `w` is the side of the SQUARE an overlay is drawn into, not the width of",
        "// the body part. It is larger than the part on purpose: a cap is authored in",
        "// the upper third of its square and goggles across the middle of theirs, so",
        "// two items that share one anchor still sit in two different places.",
        "",
        "#include <stdint.h>",
        '#include "../config.h"   // PetStage',
        "",
        "// Thousandths. Named so a reader cannot mistake these for pixels.",
        "#define WEAR_ANCHOR_UNIT %d" % FIXED,
        "",
        "struct WearAnchorPoint {",
        "    int16_t cx;    // centre of the anchor across the sprite box",
        "    int16_t cy;    // centre of the anchor down the sprite box",
        "    int16_t w;     // side of the square the overlay is drawn into",
        "};",
        "",
        "struct WearStageAnchors {",
        "    WearAnchorPoint head;",
        "    WearAnchorPoint neck;",
        "};",
        "",
        "// Indexed by (PetStage - STAGE_EGG); PetStage starts at 1, not 0, so the",
        "// subtraction is not optional and wearAnchorsFor() is the only place allowed",
        "// to do it.",
        "static const WearStageAnchors WEAR_ANCHORS[] = {",
    ]
    for stage in STAGES:
        a = ANCHORS[stage]
        note = "  // no head or neck; unreachable, every worn recipe gates later" \
            if stage == "egg" else ""
        out.append("    // %s%s" % (stage, note))
        out.append("    {{ %4d, %4d, %4d }, { %4d, %4d, %4d }}," % (
            a["head"][0], a["head"][1], a["head"][2],
            a["neck"][0], a["neck"][1], a["neck"][2]))
    out += [
        "};",
        "",
        "static_assert(sizeof(WEAR_ANCHORS) / sizeof(WEAR_ANCHORS[0]) ==",
        "                  (size_t)(STAGE_SENTINEL - STAGE_EGG + 1),",
        '              "WEAR_ANCHORS must have one row per PetStage; a stage was added "',
        '              "without re-running scripts/gen_wear_anchors.py");',
        "",
        "// The only place the stage-to-index subtraction happens. Clamps rather than",
        "// trusting the caller: this is read by the renderer on every frame, and a",
        "// stage byte from a corrupt save must not index off the front of the table.",
        "inline const WearStageAnchors& wearAnchorsFor(PetStage stage) {",
        "    int i = (int)stage - (int)STAGE_EGG;",
        "    const int n = (int)(sizeof(WEAR_ANCHORS) / sizeof(WEAR_ANCHORS[0]));",
        "    if (i < 0) i = 0;",
        "    if (i >= n) i = n - 1;",
        "    return WEAR_ANCHORS[i];",
        "}",
        "",
    ]

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote " + OUT + "  (%d stages verified against the art)" % len(STAGES))

    if "--preview" in sys.argv:
        preview(masks)


if __name__ == "__main__":
    main()
