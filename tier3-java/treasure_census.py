#!/usr/bin/env python3
"""What CAN a buried treasure land on, and how deep can it get?

An earlier note here said a buried treasure "cannot" be encased in bedrock. That
was measured from the y range of 108 chests, not from the placement rule, and it
was stated far too strongly. The rule allows something those 108 never showed:

    the scan descends from the ocean floor until the block BELOW is one of
    sandstone/stone/andesite/granite/diorite -- and it descends THROUGH air and
    water

So sediment lying over an open cave or ravine does not stop it. It falls through
the void and lands on the cave floor, which can be far below the seabed. And a
chest landing where the block at its own position is air takes the block below as
its fill, then writes that into every air face around it -- a chest genuinely
encased in whatever the cave floor is made of.

At 0.09s per chest headless against 6.5s on the server, the honest way to answer
"what is possible" is to look at tens of thousands of chests rather than argue
from a hundred.

    python treasure_census.py [--seeds N] [--start S] [--radius R] [--deep 30]

Reports the depth distribution and what the chest lands on, and lists the deepest
finds so they can be confirmed on the server. Fill agrees with the server 88% of
the time, so anything surprising here is a LEAD, not a result.
"""
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from treasure_probe import locate                      # noqa: E402
from hunt_iron_casing import OrePath                   # noqa: E402


def main():
    nseeds, start, radius, deep_at = 150, 20000, 4000, 30
    for i, a in enumerate(sys.argv):
        if a == "--seeds" and i + 1 < len(sys.argv):  nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):  start = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv): radius = int(sys.argv[i + 1])
        if a == "--deep" and i + 1 < len(sys.argv):   deep_at = int(sys.argv[i + 1])

    print(f"census: seeds {start}..{start+nseeds-1}, radius {radius}", flush=True)
    t0 = time.time()
    w = OrePath()
    fills, depths = Counter(), Counter()
    n, deepest = 0, []
    try:
        for seed in range(start, start + nseeds):
            for (x, z) in locate(seed, "1.21", radius):
                y, fill = w.probe(seed, x, z)
                if y is None or y <= -64:
                    fills["<no placement>"] += 1
                    continue
                n += 1
                fills[fill] += 1
                depths[y // 10 * 10] += 1
                if y <= deep_at:
                    deepest.append((y, seed, x, z, fill))
            if (seed - start + 1) % 25 == 0:
                el = time.time() - t0
                print(f"  {n} chests, {el/60:.1f} min, {len(deepest)} below y{deep_at}",
                      flush=True)
    finally:
        w.close()

    el = time.time() - t0
    print(f"\n{n} chests in {el/60:.1f} min ({el/max(n,1):.3f}s each)\n")
    print("depth (10-block bands):")
    for band in sorted(depths):
        print(f"  y {band:4d}..{band+9:<4d}  {depths[band]:6d}  "
              f"{'#' * min(60, depths[band] * 60 // max(depths.values()))}")
    print("\nwhat the chest lands on (headless view, 88% agreement with server):")
    for b, c in fills.most_common(25):
        print(f"  {c:6d}  {b}")

    deepest.sort()
    if deepest:
        print(f"\ndeepest {min(len(deepest), 20)} (CONFIRM THESE ON THE SERVER "
              f"-- headless fill is 88%, not gospel):")
        for (y, seed, x, z, fill) in deepest[:20]:
            print(f"  y={y:4d}  seed {seed:>8}  ({x},{z})  on {fill}")
    else:
        print(f"\nnothing below y{deep_at} in this sample")
    return 0


if __name__ == "__main__":
    sys.exit(main())
