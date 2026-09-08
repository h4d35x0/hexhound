#!/usr/bin/env python3
"""Fit the step detector's tuning constants to a real counted walk.

    python scripts/fit_step_detector.py <capture.csv> --counted 100

Replays ONE captured walk through the REAL StepDetector many times, once per
candidate parameter set, and ranks the candidates by how close they come to the
number of steps you actually counted.

Why it works this way
---------------------
The detector is compiled from the shipping header (scripts/step_replay.cpp
includes src/modules/roam_module.h) and the constants are overridden with
-DHEXHOUND_TUNE_<NAME>=<value>. There is deliberately NO second implementation of
the algorithm: a Python copy would be free to drift from the firmware, and then
this would be tuning the copy rather than the product.

Capture a walk with the diagnostic build first:

    pio run -e waveshare-esp32-s3-lcd-128-stepcap -t upload --upload-port COMxx
    # roam, walk a COUNTED number of steps in the pocket you actually use, end roam
    # plug in, reset, save the [STEPCAP] BEGIN..END block to a .csv

One walk is enough because the INPUT is captured, not the verdict. Every
candidate below is scored against the same real waveform.
"""

import argparse
import itertools
import os
import re
import shutil
import subprocess
import sys
import tempfile

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY_SRC = os.path.join(PROJECT_DIR, "scripts", "step_replay.cpp")

# Candidate grid. Threshold ratio first, because it is the knob that decides
# whether a real footfall clears the bar at all - the header's own note says a
# 1.5x ratio lost a third of a brisk walk, and 1.25x (5/4) is the current value.
# The mg floor matters next: it is what a still device is judged by, and if it is
# above a pocket's real footfall amplitude no ratio can help.
GRID = {
    "STEP_THRESHOLD_NUM": [4, 5],            # with DEN=4 -> 1.00 1.25
    "STEP_THRESHOLD_DEN": [4],
    "STEP_THRESHOLD_MIN_MG": [95, 100, 105, 110, 115, 120],
    # Peaks that must arrive in cadence before ANY of them count. This is the knob
    # that separates walking from fidgeting, and the first real walk showed why it
    # matters more than the thresholds: every false positive while sitting still
    # committed EXACTLY 2 steps, i.e. two bumps from one shift in a seat satisfied
    # a cadence run of 2 and were paid immediately. Requiring 3 rejects a 2-peak
    # burst outright, and costs a real walk only one extra held-back opener,
    # because a real walk has dozens of consecutive in-cadence peaks.
    "STEP_CADENCE_MIN": [2, 3],
}


def parse_capture_header(path):
    """Pull the device's own count out of the capture, for comparison."""
    info = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            m = re.search(r"device_detected_steps=(\d+)", line)
            if m:
                info["device_steps"] = int(m.group(1))
            m = re.search(r"truncated=(\d+)", line)
            if m:
                info["truncated"] = bool(int(m.group(1)))
    return info


def find_gpp():
    for cand in (r"C:\msys64\mingw64\bin\g++.exe", "g++"):
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        elif shutil.which(cand):
            return cand
    sys.exit("g++ not found. On Windows add C:\\msys64\\mingw64\\bin to PATH.")


def run_candidate(gpp, workdir, capture, overrides, walk_end_ms):
    """Returns (walk_steps, stationary_steps, total) or (None, None, None).

    Split by time, because a TOTAL is not a measure of correctness here. The very
    first real walk captured had 113 steps during the walking phase against 118-119
    counted, and then 6 more committed while sitting still, so the total landed on
    119 and looked perfect. Scoring on the total would have rewarded an undercount
    for being cancelled by false positives. Never do that.
    """
    exe = os.path.join(workdir, "replay.exe")
    cmd = [gpp, "-O2", "-std=gnu++17", "-o", exe, REPLAY_SRC]
    for name, value in overrides.items():
        cmd.append(f"-DHEXHOUND_TUNE_{name}={value}")
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        return None, None, build.stderr[-400:]

    run = subprocess.run([exe, capture, "--steps"], capture_output=True, text=True)
    if run.returncode != 0:
        return None, None, run.stderr[-400:]

    walk = stat = 0
    for line in run.stdout.splitlines():
        if not line.startswith("STEP "):
            continue
        _, t, n = line.split()
        if int(t) <= walk_end_ms:
            walk += int(n)
        else:
            stat += int(n)
    return walk, stat, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", help="CSV from the [STEPCAP] dump")
    ap.add_argument("--counted", type=int, required=True,
                    help="steps you actually counted on the walk")
    ap.add_argument("--tolerance-pct", type=float, default=5.0,
                    help="what counts as a good fit (default 5%%)")
    ap.add_argument("--walk-end-ms", type=int, required=True,
                    help="ms at which walking STOPPED. Steps committed after this "
                         "are false positives, because the walker was stationary.")
    ap.add_argument("--fp-weight", type=float, default=1.0,
                    help="cost of one false positive relative to one missed step")
    args = ap.parse_args()

    if not os.path.exists(args.capture):
        sys.exit(f"no such capture: {args.capture}")

    info = parse_capture_header(args.capture)
    if info.get("truncated"):
        print("WARNING: this capture is TRUNCATED. The walk was longer than the\n"
              "         buffer, so the tail is missing and the counted total\n"
              "         cannot match it. Re-capture a shorter walk.\n")

    gpp = find_gpp()
    print(f"counted steps (ground truth): {args.counted}")
    if "device_steps" in info:
        print(f"device counted at capture time: {info['device_steps']}")
    print(f"compiler: {gpp}\n")

    workdir = tempfile.mkdtemp(prefix="stepfit-")
    try:
        # Baseline FIRST, with no overrides at all, so the improvement is measured
        # against what actually ships rather than against the grid's first entry.
        bw, bs, err = run_candidate(gpp, workdir, args.capture, {},
                                    args.walk_end_ms)
        if bw is None:
            sys.exit(f"baseline build/run failed:\n{err}")
        base_score = abs(bw - args.counted) + args.fp_weight * bs
        print("BASELINE (shipping constants):")
        print(f"  walking phase : {bw} steps vs {args.counted} counted "
              f"(off by {bw - args.counted}, "
              f"{100.0*abs(bw-args.counted)/max(1,args.counted):.1f}%)")
        print(f"  stationary    : {bs} false positives")
        print(f"  score         : {base_score:.1f}  (missed + "
              f"{args.fp_weight}x FP)\n")

        names = list(GRID)
        results = []
        combos = list(itertools.product(*(GRID[n] for n in names)))
        print(f"sweeping {len(combos)} candidates...")
        for combo in combos:
            ov = dict(zip(names, combo))
            w, s, err = run_candidate(gpp, workdir, args.capture, ov,
                                      args.walk_end_ms)
            if w is None:
                print("  build failed:", ov, err)
                continue
            results.append((abs(w - args.counted) + args.fp_weight * s, w, s, ov))

        results.sort(key=lambda r: (r[0], abs(r[1] - args.counted)))
        tol = args.counted * args.tolerance_pct / 100.0

        print(f"\n{'score':>6} {'walk':>5} {'FP':>4}  parameters")
        print("-" * 78)
        for score, w, s, ov in results[:15]:
            good = abs(w - args.counted) <= tol and s == 0
            flag = "  <-- accurate AND no false positives" if good else ""
            params = " ".join(f"{k.replace('STEP_','')}={v}" for k, v in ov.items())
            print(f"{score:>6.1f} {w:>5} {s:>4}  {params}{flag}")

        if results:
            best_score, best_w, best_s, best = results[0]
            print(f"\nBEST: walking {best_w} vs {args.counted} counted, "
                  f"{best_s} false positives while stationary")
            print("  " + "  ".join(f"{k}={v}" for k, v in best.items()))
            if best_score >= base_score:
                print("\nNOTE: the grid did not beat the shipping constants. Either the\n"
                      "      current values are already right for this carry position, or\n"
                      "      the limitation is not in these parameters. Look at the\n"
                      "      waveform before widening the grid.")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
