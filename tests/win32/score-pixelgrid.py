#!/usr/bin/env python3
"""Score two pixelgrid captures by per-pixel MAE.

    python3 score-pixelgrid.py <a.txt> <b.txt>

For the GDI vs GDI+ comparison (docs/design/win32-gdiplus-renderer.md).
Both sides come from the SAME command buffer via canvas_read_pixel, so any
non-zero score is renderer difference rather than scene difference.

THRESHOLDS ARE NOT svg-compare-aevg.py's. Those (good <20) are calibrated
for whole-image SVG conformance and are far too loose here: an element
missing over 11% of the frame measures 13.4, which they would call "good".
The signal here lives in single digits.

MAE RANKS; A HUMAN JUDGES. High MAE on antialiased edges means GDI+ is
better; high MAE from a missing element means it is broken. This tool
cannot tell them apart, which is why it is triage and not a gate.
"""
import sys
import numpy as np


def load(path):
    vals = [int(l.strip(), 16) for l in open(path) if l.strip()]
    a = np.array(vals, dtype=np.uint32)
    return np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], axis=1).astype(float)


def band(mae):
    if mae < 1.0:   return "identical in practice — faithful, bought nothing"
    if mae < 8.0:   return "antialiasing / alpha scale — the EXPECTED improvement"
    if mae < 15.0:  return "substantial — look at it, could be better or broken"
    return "something missing or misplaced — regression until proven otherwise"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(a) != len(b):
        print(f"sample count differs: {len(a)} vs {len(b)}")
        return 1
    diff = np.abs(a - b)
    mae = float(np.mean(diff))
    ndiff = int((diff.sum(axis=1) > 0).sum())
    print(f"samples          : {len(a)}")
    print(f"MAE              : {mae:.4f}")
    print(f"differing samples: {ndiff} ({100.0 * ndiff / len(a):.1f}%)")
    print(f"reading          : {band(mae)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
