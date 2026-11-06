#!/usr/bin/env python3
"""Piece-exact structure overlap for structures cubiomes cannot assemble.

`build/structoverlap.exe` intersects real piece bounding boxes, which is the
right question -- the search compares nominal boxes and measures distance
origin-to-origin, and a mansion sprawls so far past its origin that a pair
reported 57 blocks apart had buildings ~15-20 apart. But cubiomes only ships
piece generators for fortress and end_city, so for everything else that tool
answers UNKNOWN.

Mansion is exactly the case that motivated the overlap tool and exactly the case
it could not do.

This closes it from the other side. tier2-outpost loads the real 1.21.1 server
jar, so `OutpostWorldgen pieces` runs Minecraft's OWN structure generator and
reports every piece's true bounding box -- 511 of them for a mansion. No
reimplementation, and it works for every structure rather than adding one more
special case.

    python piece_overlap.py <seed> <structA> <ax> <az> <structB> <bx> <bz>

Exit code: 0 overlap, 1 separate, 2 UNKNOWN. UNKNOWN is reserved for "a
structure could not be generated there", which is NOT a finding that the two are
apart -- distinguishing those is the whole point.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FAST = os.path.join(ROOT, "tier2-outpost")
sys.path.insert(0, FAST)
from bench_fast import java_bin, classpath          # noqa: E402


def pieces(seed, structure, x, z):
    """Every piece box of `structure` near (x,z), or None if none generated.

    None and [] mean different things and are kept apart: None is "the generator
    found no structure there", [] would be "a structure with no extent". Only the
    first is a reason to answer UNKNOWN.
    """
    r = subprocess.run(
        [java_bin(), "-Xmx2g", "-cp", classpath(), "OutpostWorldgen",
         "pieces", str(seed), structure, str(x), str(z)],
        capture_output=True, text=True, cwd=FAST)
    out = []
    hit = False
    for line in r.stdout.splitlines():
        line = re.sub(r"^.*STDOUT\]: ", "", line).rstrip()
        if line.startswith("NOHIT") or line.startswith("ERROR"):
            return None
        if line.startswith("START"):
            hit = True
        elif line.startswith("P\t"):
            t = line.split("\t")
            out.append((int(t[1]), int(t[2]), int(t[3]),
                        int(t[4]), int(t[5]), int(t[6]),
                        t[7] if len(t) > 7 else "?"))
    return out if hit else None


def ov1(a0, a1, b0, b1):
    """Overlap of two closed intervals on one axis; 0 when they miss."""
    lo, hi = max(a0, b0), min(a1, b1)
    return hi - lo + 1 if hi >= lo else 0


def main():
    if len(sys.argv) < 8:
        sys.exit(__doc__)
    seed = sys.argv[1]
    na, ax, az = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    nb, bx, bz = sys.argv[5], int(sys.argv[6]), int(sys.argv[7])

    pa = pieces(seed, na, ax, az)
    pb = pieces(seed, nb, bx, bz)
    print(f"seed {seed}  {na}({ax},{az}) x {nb}({bx},{bz})")
    if pa is None or pb is None:
        which = na if pa is None else nb
        print(f"  {na} pieces: {'-' if pa is None else len(pa)}")
        print(f"  {nb} pieces: {'-' if pb is None else len(pb)}")
        print(f"UNKNOWN: no {which} generated near that position, so whether they "
              f"overlap is not decidable here -- which is NOT the same as them "
              f"not overlapping.")
        return 2
    print(f"  {na}: {len(pa)} pieces\n  {nb}: {len(pb)} pieces")

    pairs = shared = 0
    min_gap = None
    for a in pa:
        for b in pb:
            ox = ov1(a[0], a[3], b[0], b[3])
            oy = ov1(a[1], a[4], b[1], b[4])
            oz = ov1(a[2], a[5], b[2], b[5])
            if ox and oy and oz:
                pairs += 1
                shared += ox * oy * oz
            else:
                gx = 0 if ox else (a[0] - b[3] if a[0] > b[3] else b[0] - a[3])
                gz = 0 if oz else (a[2] - b[5] if a[2] > b[5] else b[2] - a[5])
                g = max(gx, gz)
                if min_gap is None or g < min_gap:
                    min_gap = g
    if pairs:
        print(f"OVERLAP: {pairs} piece pairs intersect, {shared} blocks of shared "
              f"volume. The two structures really occupy the same space.")
        return 0
    print(f"SEPARATE: no piece of one meets any piece of the other; nearest "
          f"pieces are {min_gap} blocks apart horizontally.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
