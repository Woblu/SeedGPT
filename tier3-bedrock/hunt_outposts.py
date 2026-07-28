#!/usr/bin/env python3
"""Hunt Bedrock outposts with tall cobblestone foundations -- by brute force.

The earlier pipeline tried to PREDICT which outposts would be stilted and got
it wrong: it ranked by the ground drop across the footprint, but Bedrock
anchors the tower on the high side, so the drop it liked was beside the tower
rather than under it. Predicting properly needs Bedrock's placement algorithm,
which is not recovered yet (tools/bedrockfit ruled out the cheap hypothesis).

So: stop predicting. The oracle gives real outposts and block probes give the
truth, which is enough to find them the honest way -- look at a lot of them.

Per outpost, the probe is aimed rather than exhaustive:

  * our terrain (build/surface.exe) samples a grid of columns around the
    reported position, for free, and picks the LOWEST few -- a foundation, if
    there is one, reaches down on the low side
  * those columns are probed upward for cobblestone, one block at a time

What gets reported is the contiguous cobblestone column the game actually
built, next to the ground level underneath it. A normal watchtower sits on a
base a few blocks deep; anything much taller is the foundation fill that Java
cannot produce.

    python hunt_outposts.py --seeds 1,2,3 --json finds.jsonl
    python hunt_outposts.py --random 200 --min-cobble 10 --json finds.jsonl
"""

import argparse
import json
import random
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, EXE                     # noqa: E402
from collect_structures import sweep                # noqa: E402
from glitched_outposts import block_is              # noqa: E402

ROOT = Path(__file__).parent.parent
SURFACE = ROOT / "build" / "surface.exe"


def ground(seed, version, x, z):
    p = subprocess.run([str(SURFACE), str(seed), version, str(x), str(z), "0"],
                       capture_output=True, text=True)
    m = re.search(r"surface=(-?\d+)", p.stdout)
    return int(m.group(1)) if m else None


def column_grid(seed, version, x, z, half=12, step=4):
    """Ground level at every column on a grid around a position.

    Step 4, not 8: a watchtower base is only about seven blocks across, and a
    coarser grid steps straight over it -- which is exactly how the first
    version of this hunt reported "no cobblestone" at an outpost whose
    foundation had already been found by hand.
    """
    cols = []
    for dx in range(-half, half + 1, step):
        for dz in range(-half, half + 1, step):
            g = ground(seed, version, x + dx, z + dz)
            if g is not None:
                cols.append((g, x + dx, z + dz))
    cols.sort()
    return cols


def cobble_column(bds, x, z, y0, y1):
    """Tallest run of cobblestone in [y0, y1] at one column, or (0, None, None).

    Two non-cobble blocks are tolerated inside a run: real foundations have the
    odd stair or window course, and stopping at the first gap would under-report
    exactly the tall ones we are looking for.
    """
    best, run_top, run_bot = 0, None, None
    cur_top = None
    misses = 0
    for y in range(y1, y0 - 1, -1):
        r = block_is(bds, x, y, z, "cobblestone")
        if r is None:
            return 0, None, None            # chunk not loaded: no claim
        if r:
            if cur_top is None:
                cur_top = y
            misses = 0
            if cur_top - y + 1 > best:
                best, run_top, run_bot = cur_top - y + 1, cur_top, y
        elif cur_top is not None:
            misses += 1
            if misses > 2:
                cur_top, misses = None, 0
    return best, run_top, run_bot


def hunt_seed(bds, seed, version, reach, step, min_drop, min_cobble, verbose=False):
    finds = []
    outposts = sweep(bds, "pillager_outpost", reach, step)
    for (x, z) in sorted(outposts):
        cols = column_grid(seed, version, x, z)
        if not cols:
            continue
        drop = cols[-1][0] - cols[0][0]
        if drop < min_drop:
            continue                        # flat ground cannot hide a foundation
        bds.command(f"tickingarea add circle {x} 64 {z} 4 hunt", timeout=20)
        time.sleep(2.5)
        try:
            # Cheap detector first: cobblestone sitting AT ground level. That is
            # true both of the tower's own base and of any foundation filled
            # down the low side, so one probe per column finds the structure
            # without walking every column top to bottom.
            hits = []
            for (g, cx, cz) in cols:
                r = block_is(bds, cx, g + 1, cz, "cobblestone")
                if r is None:
                    break                   # chunk not loaded: claim nothing
                if r:
                    hits.append((g, cx, cz))
            best = (0, None, None, None, None)
            for (g, cx, cz) in hits[:4]:    # lowest ground first: cols is sorted
                h, top, bot = cobble_column(bds, cx, cz, g - 4, g + 40)
                if h > best[0]:
                    best = (h, cx, cz, top, bot)
            h, cx, cz, top, bot = best
            if verbose:
                print(f"    {x},{z} drop={drop} cobble={h}", file=sys.stderr)
            if h >= min_cobble:
                finds.append({"seed": seed, "x": x, "z": z, "drop": drop,
                              "cobble_height": h, "cobble_top": top,
                              "cobble_bottom": bot, "column": [cx, cz],
                              "ground_at_column": ground(seed, version, cx, cz)})
        finally:
            bds.command("tickingarea remove hunt", timeout=20)
    return finds


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", help="comma-separated seeds")
    ap.add_argument("--random", type=int, help="hunt this many random 32-bit seeds")
    ap.add_argument("--version", default="1.21", help="Java version for the terrain engine")
    ap.add_argument("--reach", type=int, default=2500)
    ap.add_argument("--step", type=int, default=800)
    ap.add_argument("--min-drop", type=int, default=12,
                    help="ground variation nearby before an outpost is worth probing")
    ap.add_argument("--min-cobble", type=int, default=8,
                    help="report a foundation at least this tall")
    ap.add_argument("--json", help="append finds as JSON lines")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    if not SURFACE.exists():
        sys.exit("build/surface.exe missing -- ./build.sh tools/surface.c")

    seeds = [int(s) for s in a.seeds.split(",")] if a.seeds else []
    if a.random:
        # Bedrock seeds are 32-bit signed; sample the space it actually has.
        rng = random.Random(20260726)
        seeds += [rng.randint(-2**31, 2**31 - 1) for _ in range(a.random)]
    if not seeds:
        seeds = [12345]

    out = open(a.json, "a", encoding="utf-8") if a.json else None
    total = 0
    for i, seed in enumerate(seeds, 1):
        t0 = time.time()
        try:
            with Bds(seed) as bds:
                finds = hunt_seed(bds, seed, a.version, a.reach, a.step,
                                  a.min_drop, a.min_cobble, a.verbose)
        except Exception as e:                        # noqa: BLE001
            print(f"[{i}/{len(seeds)}] seed {seed}: FAILED {e}", file=sys.stderr)
            continue
        total += len(finds)
        print(f"[{i}/{len(seeds)}] seed {seed}: {len(finds)} find(s) "
              f"in {time.time()-t0:.0f}s (running total {total})", file=sys.stderr)
        for f in finds:
            print(json.dumps(f), flush=True)
            if out:
                out.write(json.dumps(f) + "\n"); out.flush()
    if out:
        out.close()


if __name__ == "__main__":
    main()
