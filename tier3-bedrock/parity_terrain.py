#!/usr/bin/env python3
"""Does Bedrock's terrain HEIGHT match Java's for the same seed?

The biome parity test (parity_biomes.py) showed the two editions agree on where
biomes are. That is not the same claim as agreeing on the ground level, and the
stilted-outpost work depends on the second one: it measures how far a Bedrock
structure's foundation has to reach using OUR terrain, which is only legitimate
if our terrain is Bedrock's terrain.

BDS has no height command, but it can be asked about a block:

    tickingarea add circle x 64 z 1 probe      (a block query needs a LOADED chunk)
    testforblock x y z air

so a binary search over y finds the surface in about ten probes. Water is not
air, so the search stops at the water surface over ocean -- sample points are
therefore compared only where our engine says the ground is above sea level,
and submerged columns are reported separately rather than silently averaged in.

    python parity_terrain.py --seeds 12345,1 --points 6
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, EXE                    # noqa: E402

ROOT = Path(__file__).parent.parent
SURFACE = ROOT / "build" / "surface.exe"
SEA = 63

RE_FOUND_BLOCK = re.compile(r"Successfully found the block", re.I)


def java_surface(seed, version, x, z):
    p = subprocess.run([str(SURFACE), str(seed), version, str(x), str(z), "0"],
                       capture_output=True, text=True)
    m = re.search(r"surface=(-?\d+)", p.stdout)
    return int(m.group(1)) if m else None


def is_air(bds, x, y, z):
    for line in bds.command(f"testforblock {x} {y} {z} air", timeout=20, stop=Bds.RE_BLOCK):
        if RE_FOUND_BLOCK.search(line):
            return True
        if "is Air" in line:            # "The block at .. is Air (expected: X)"
            return True
        if "The block at" in line:
            return False
        if "outside of the world" in line or "out of range" in line:
            return None            # not loaded: NOT the same as "not air"
    return None


def bedrock_surface(bds, x, z, lo=-64, hi=200):
    """Highest y whose block is not air: binary search on the air boundary."""
    # Radius must be <= 4 chunks; a larger value is rejected outright and the
    # probes below then read "outside of the world" rather than terrain.
    bds.command(f"tickingarea add circle {x} 64 {z} 4 probe", timeout=20)
    time.sleep(3.0)                      # the chunk has to actually generate
    try:
        if is_air(bds, x, hi, z) is None:
            return None                  # chunk never loaded
        if not is_air(bds, x, hi, z):
            return hi                    # terrain above the search window
        while hi - lo > 1:
            mid = (lo + hi) // 2
            a = is_air(bds, x, mid, z)
            if a is None:
                return None
            if a:
                hi = mid
            else:
                lo = mid
        return lo
    finally:
        bds.command("tickingarea remove probe", timeout=20)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", default="12345")
    ap.add_argument("--version", default="1.21")
    ap.add_argument("--points", type=int, default=6)
    ap.add_argument("--step", type=int, default=400)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    if not SURFACE.exists():
        sys.exit("build/surface.exe missing -- ./build.sh tools/surface.c")

    rows, agree, near, off, submerged = [], 0, 0, 0, 0
    for seed in [int(s) for s in a.seeds.split(",") if s.strip()]:
        with Bds(seed, verbose=a.verbose) as bds:
            for i in range(a.points):
                x = (i - a.points // 2) * a.step
                z = ((i * 7) % 5 - 2) * a.step        # spread out, stay cheap
                js = java_surface(seed, a.version, x, z)
                bs = bedrock_surface(bds, x, z)
                if js is None or bs is None:
                    rows.append((seed, x, z, js, bs, "no answer"))
                    continue
                if js < SEA:
                    submerged += 1
                    rows.append((seed, x, z, js, bs, "submerged (water counts in bedrock)"))
                    continue
                d = abs(js - bs)
                verdict = "exact" if d == 0 else (f"off by {d}" if d <= 2 else f"DIFFERENT ({d})")
                agree += (d == 0); near += (0 < d <= 2); off += (d > 2)
                rows.append((seed, x, z, js, bs, verdict))

    print(f"\n{'seed':>10} {'x':>7} {'z':>7} {'java':>6} {'bedrock':>8}  verdict")
    for r in rows:
        print(f"{r[0]:>10} {r[1]:>7} {r[2]:>7} {str(r[3]):>6} {str(r[4]):>8}  {r[5]}")
    print(f"\nexact {agree}   within 2 blocks {near}   different {off}   "
          f"submerged/skipped {submerged}")


if __name__ == "__main__":
    main()
