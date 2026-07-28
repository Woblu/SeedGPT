#!/usr/bin/env python3
"""Find Bedrock pillager outposts standing on a tall cobblestone foundation.

This is the thing Java cannot do. Java drops a small dirt platform under a
structure that lands in air; Bedrock FILLS A FOUNDATION down to the ground out
of the structure's own material, so a watchtower placed on a cliff edge, a
ravine lip or over water grows a cobblestone column beneath it. Those are the
screenshots -- and they are a Bedrock behaviour, not a Java one.

Three stages, cheap to expensive, in the same shape as the Java tier-2 work:

  1. ORACLE   sweep `locate structure pillager_outpost` over a grid of query
              origins in one BDS world -- every outpost in the area for the
              price of one server start.
  2. PREFILTER  our C terrain (build/surface.exe) measures the ground drop
              across each outpost's footprint. Instant, and a tall foundation
              is impossible without a big drop, so this discards almost
              everything before any block is touched.
  3. VERIFY   for the survivors, load the chunk and probe the actual blocks:
              how far does cobblestone really extend below the tower? That is
              the number reported. Nothing is claimed that the game did not
              confirm block by block.

    python glitched_outposts.py --seeds 1,2,3 --min-drop 20
    python glitched_outposts.py --seeds 12345 --min-drop 10 --verify-top 3
"""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, RE_FOUND, EXE          # noqa: E402
from collect_structures import sweep               # noqa: E402

ROOT = Path(__file__).parent.parent
SURFACE = ROOT / "build" / "surface.exe"
RE_FOUND_BLOCK = re.compile(r"Successfully found the block", re.I)

# The watchtower sits inside the structure chunk; probe a few offsets around the
# reported position rather than assuming an exact anchor.
PROBE_OFFSETS = [(0, 0), (4, 4), (-4, -4), (4, -4), (-4, 4), (8, 0), (0, 8)]


def terrain_drop(seed, version, x, z, half=10):
    p = subprocess.run([str(SURFACE), str(seed), version, str(x), str(z), str(half)],
                       capture_output=True, text=True)
    m = re.search(r"surface=(-?\d+) low=(-?\d+) high=(-?\d+) drop=(-?\d+)", p.stdout)
    if not m:
        return None
    return {"surface": int(m.group(1)), "low": int(m.group(2)),
            "high": int(m.group(3)), "drop": int(m.group(4))}


def block_is(bds, x, y, z, block):
    """True / False / None, where None means THE CHUNK IS NOT LOADED.

    That distinction cost an afternoon: an unloaded chunk answers "Cannot test
    for block outside of the world", which is neither a match nor a mismatch.
    Folding it into False makes every probe look like "no structure here" and
    turns a loading bug into a confident wrong answer.
    """
    for line in bds.command(f"testforblock {x} {y} {z} {block}", timeout=20, stop=Bds.RE_BLOCK):
        if RE_FOUND_BLOCK.search(line):
            return True
        if "The block at" in line:
            return False
        if "outside of the world" in line or "out of range" in line:
            return None
    return None


def measure_foundation(bds, x, z, top, bottom):
    """Deepest cobblestone column under the tower, as the game actually built it.

    Returns (height, sample_x, sample_z, top_y, bottom_y). Walks down from the
    structure's own level looking for cobblestone, then follows it as far as it
    goes -- allowing a couple of non-cobble blocks so a window of stone or a
    stair step does not end the measurement early.
    """
    # Radius is capped at 4 chunks -- a bigger one is REJECTED, leaving nothing
    # loaded and every probe answering "outside of the world".
    bds.command(f"tickingarea add circle {x} 64 {z} 4 gp", timeout=20)
    time.sleep(3.0)
    best = (0, x, z, None, None)
    if block_is(bds, x, top, z, "air") is None:
        bds.command("tickingarea remove gp", timeout=20)
        raise RuntimeError(f"chunk at {x},{z} never loaded -- cannot verify")
    try:
        for (dx, dz) in PROBE_OFFSETS:
            px, pz = x + dx, z + dz
            first = None
            for y in range(top, bottom - 1, -1):
                r = block_is(bds, px, y, pz, "cobblestone")
                if r is None:
                    return best                 # chunk not loaded; give up quietly
                if r:
                    first = y
                    break
            if first is None:
                continue
            last, misses = first, 0
            for y in range(first - 1, bottom - 1, -1):
                if block_is(bds, px, y, pz, "cobblestone"):
                    last = y; misses = 0
                else:
                    misses += 1
                    if misses > 2:
                        break
            h = first - last + 1
            if h > best[0]:
                best = (h, px, pz, first, last)
        return best
    finally:
        bds.command("tickingarea remove gp", timeout=20)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", default="12345")
    ap.add_argument("--version", default="1.21", help="Java version for the terrain engine")
    ap.add_argument("--reach", type=int, default=1500)
    ap.add_argument("--step", type=int, default=500)
    ap.add_argument("--min-drop", type=int, default=20,
                    help="ground drop across the footprint before a candidate is worth probing")
    ap.add_argument("--verify-top", type=int, default=2,
                    help="how many candidates per seed to confirm in game")
    ap.add_argument("--json", help="append findings as JSON lines")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    if not SURFACE.exists():
        sys.exit("build/surface.exe missing -- ./build.sh tools/surface.c")

    out = open(a.json, "a", encoding="utf-8") if a.json else None
    for seed in [int(s) for s in a.seeds.split(",") if s.strip()]:
        t0 = time.time()
        with Bds(seed, verbose=a.verbose) as bds:
            found = sweep(bds, "pillager_outpost", a.reach, a.step)
            cands = []
            for (x, z) in found:
                d = terrain_drop(seed, a.version, x, z)
                if d:
                    cands.append((d["drop"], x, z, d))
            cands.sort(reverse=True)
            print(f"seed {seed}: {len(found)} outposts, "
                  f"biggest ground drop {cands[0][0] if cands else '-'}", file=sys.stderr)

            for (drop, x, z, d) in cands[:a.verify_top]:
                if drop < a.min_drop:
                    break
                h, px, pz, top, bot = measure_foundation(
                    bds, x, z, top=d["high"] + 4, bottom=max(-60, d["low"] - 8))
                rec = {"seed": seed, "x": x, "z": z, "predicted_drop": drop,
                       "terrain": d, "cobblestone_height": h,
                       "probe": {"x": px, "z": pz, "top": top, "bottom": bot},
                       "seconds": round(time.time() - t0, 1)}
                print(json.dumps(rec))
                if out:
                    out.write(json.dumps(rec) + "\n"); out.flush()
    if out:
        out.close()


if __name__ == "__main__":
    main()
