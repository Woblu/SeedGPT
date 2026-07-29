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
import queue
import random
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, EXE, MAX_INSTANCES      # noqa: E402
from collect_structures import sweep                # noqa: E402
from glitched_outposts import block_is              # noqa: E402

ROOT = Path(__file__).parent.parent
SURFACE = ROOT / "build" / "surface.exe"

# How many cobblestone columns get measured per outpost. The grid is sorted
# lowest-ground-first and a foundation reaches down on the low side, so the
# first few hits are the ones that matter -- probing the rest of the grid is
# ~30 wasted commands, and a command costs a server tick.
WANT_HITS = 4


def box_drop(seed, version, x, z, half=16):
    """Ground low/high/drop around a position in ONE call to the C engine.

    Ranking outposts this way costs a single subprocess each instead of one per
    column, which is what makes it affordable to sweep a wide area and then
    probe only the extreme tail. A tall foundation cannot exist without a tall
    drop, so this is a necessary condition and a very cheap one.
    """
    p = subprocess.run([str(SURFACE), str(seed), version, str(x), str(z), str(half)],
                       capture_output=True, text=True)
    m = re.search(r"low=(-?\d+) high=(-?\d+) drop=(-?\d+)", p.stdout)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


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


def wait_loaded(bds, x, z, timeout=20):
    """Block until the chunk answers block queries, or give up.

    A fixed sleep is a race, and losing it is silent: the first probe returns
    "not loaded", the loop treats it as "no cobblestone here", and the outpost
    is reported as having none. That is how a 66-block foundation got recorded
    as a zero. Poll instead, and let the caller SKIP rather than publish a
    number it did not measure.
    """
    end = time.time() + timeout
    while time.time() < end:
        if block_is(bds, x, 64, z, "air") is not None:
            return True
        time.sleep(0.5)
    return False


def cobble_column(bds, x, z, y0, y1, known_bottom=None):
    """Height of the cobblestone run at one column, in ~8 probes instead of ~45.

    BDS executes ONE console command per tick (measured: 61 ms sequential, 62 ms
    pipelined -- queueing more does not help), so the only way to go faster is
    to ask fewer questions. A foundation is a column of cobblestone rising from
    the ground to the tower's floor, so its top can be found by BISECTION rather
    than by walking every block.

    The run is not perfectly solid -- a stair or window course can interrupt it --
    so after bisecting, the top is nudged upward past small gaps. That costs a
    few probes and keeps the tall ones from being under-reported, which is the
    failure that matters here.
    """
    bot = known_bottom if known_bottom is not None else y0
    if block_is(bds, x, bot, z, "cobblestone") is not True:
        return 0, None, None

    # Bisect for the highest y that is still cobblestone, assuming the run is
    # contiguous from `bot` upward.
    lo, hi = bot, y1
    if block_is(bds, x, hi, z, "cobblestone") is True:
        top = hi
    else:
        while hi - lo > 1:
            mid = (lo + hi) // 2
            r = block_is(bds, x, mid, z, "cobblestone")
            if r is None:
                return 0, None, None            # chunk not loaded: no claim
            if r:
                lo = mid
            else:
                hi = mid
        top = lo

    # Step over short interruptions so a decorative course does not truncate a
    # tall foundation. Two consecutive non-cobble blocks end it, as before.
    y, misses = top + 1, 0
    while y <= y1 and misses <= 2:
        if block_is(bds, x, y, z, "cobblestone") is True:
            top, misses = y, 0
        else:
            misses += 1
        y += 1
    return top - bot + 1, top, bot


def hunt_seed(bds, seed, version, reach, step, min_drop, min_cobble,
              top_by_drop=2, verbose=False):
    finds = []
    outposts = sweep(bds, "pillager_outpost", reach, step)
    # Rank every outpost by the ground drop around it -- one cheap C call each --
    # and probe only the most extreme. Probing is the expensive stage, so it
    # should be spent where a tall foundation is even possible.
    ranked = []
    for (x, z) in outposts:
        d = box_drop(seed, version, x, z)
        if d:
            ranked.append((d[2], x, z))
    ranked.sort(reverse=True)
    if verbose and ranked:
        print(f"    {len(ranked)} outposts, biggest drops "
              f"{[r[0] for r in ranked[:5]]}", file=sys.stderr)
    for (drop, x, z) in ranked[:top_by_drop]:
        if drop < min_drop:
            break                           # flat ground cannot hide a foundation
        cols = column_grid(seed, version, x, z)
        if not cols:
            continue
        bds.command(f"tickingarea add circle {x} 64 {z} 4 hunt", timeout=20)
        try:
            if not wait_loaded(bds, x, z):
                if verbose:
                    print(f"    {x},{z} SKIPPED -- chunk never loaded", file=sys.stderr)
                continue
            # Cheap detector first: cobblestone sitting AT ground level. That is
            # true both of the tower's own base and of any foundation filled
            # down the low side, so one probe per column finds the structure
            # without walking every column top to bottom.
            hits, lost = [], False
            for (g, cx, cz) in cols:
                r = block_is(bds, cx, g + 1, cz, "cobblestone")
                if r is None:
                    lost = True             # unloaded: NOT the same as "no cobble"
                    break
                if r:
                    hits.append((g, cx, cz))
                    if len(hits) >= WANT_HITS:
                        break               # cols is sorted lowest-first, and
                                            # only these get measured anyway
            if lost:
                if verbose:
                    print(f"    {x},{z} SKIPPED -- probe lost the chunk", file=sys.stderr)
                continue
            best = (0, None, None, None, None)
            for (g, cx, cz) in hits:        # lowest ground first: cols is sorted
                # The detector already proved cobblestone at g+1, so the run's
                # bottom is known and only its top has to be found.
                # Ceiling near build height, not a fixed offset: bisection
                # makes a wide window cost ~1 extra probe, while a narrow one
                # silently TRUNCATES the tall finds. A 60-block foundation was
                # reported as 40 purely because the old window stopped at g+40.
                h, top, bot = cobble_column(bds, cx, cz, g + 1, 200,
                                            known_bottom=g + 1)
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
    ap.add_argument("--rng", type=int, default=20260726,
                    help="sampling seed. Change it to explore NEW seeds -- the "
                         "default is fixed so a run is reproducible, which also "
                         "means restarting re-hunts the same ones.")
    ap.add_argument("--version", default="1.21", help="Java version for the terrain engine")
    ap.add_argument("--reach", type=int, default=6000)
    ap.add_argument("--step", type=int, default=800)
    ap.add_argument("--top-by-drop", type=int, default=2,
                    help="probe only this many outposts per seed, steepest first")
    ap.add_argument("--min-drop", type=int, default=12,
                    help="ground variation nearby before an outpost is worth probing")
    ap.add_argument("--min-cobble", type=int, default=8,
                    help="report a foundation at least this tall")
    ap.add_argument("--json", help="append finds as JSON lines")
    ap.add_argument("--workers", type=int, default=1,
                    help=f"parallel Bedrock servers (1-{MAX_INSTANCES}). Each is a "
                         f"real Minecraft server holding ~0.35 GB and a core while "
                         f"it generates, so this is capped in code.")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    if not SURFACE.exists():
        sys.exit("build/surface.exe missing -- ./build.sh tools/surface.c")

    seeds = [int(s) for s in a.seeds.split(",")] if a.seeds else []
    if a.random:
        # Bedrock seeds are 32-bit signed; sample the space it actually has.
        rng = random.Random(a.rng)
        seeds += [rng.randint(-2**31, 2**31 - 1) for _ in range(a.random)]
    if not seeds:
        seeds = [12345]

    out = open(a.json, "a", encoding="utf-8") if a.json else None
    lock = threading.Lock()
    work = queue.Queue()
    for i, sd in enumerate(seeds):
        work.put((i + 1, sd))
    state = {"total": 0, "done": 0}

    def worker(instance):
        """One server, many seeds. Each worker owns its own BDS directory and
        port, because a server runs one command per tick -- more ticking
        servers is the only way to run more commands per second."""
        while True:
            try:
                idx, seed = work.get_nowait()
            except queue.Empty:
                return
            t0 = time.time()
            try:
                with Bds(seed, instance=instance) as bds:
                    finds = hunt_seed(bds, seed, a.version, a.reach, a.step,
                                      a.min_drop, a.min_cobble, a.top_by_drop,
                                      a.verbose)
            except Exception as e:                    # noqa: BLE001
                with lock:
                    print(f"[{idx}/{len(seeds)}] seed {seed}: FAILED {e}", file=sys.stderr)
                continue
            with lock:
                state["total"] += len(finds); state["done"] += 1
                print(f"[{state['done']}/{len(seeds)}] seed {seed}: {len(finds)} find(s) "
                      f"in {time.time()-t0:.0f}s  (w{instance}, running total "
                      f"{state['total']})", file=sys.stderr)
                for f in finds:
                    print(json.dumps(f), flush=True)
                    if out:
                        out.write(json.dumps(f) + chr(10)); out.flush()

    # Clamp rather than trust the flag: this runs on someone's own machine,
    # and four Minecraft servers is already a noticeable share of it.
    nworkers = max(1, min(a.workers, MAX_INSTANCES, len(seeds)))
    if nworkers != a.workers:
        print(f"using {nworkers} worker(s) (capped at {MAX_INSTANCES})", file=sys.stderr)
    threads = [threading.Thread(target=worker, args=(i,), daemon=True)
               for i in range(nworkers)]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print(f"{state['total']} find(s) from {state['done']} seeds in "
          f"{time.time()-t0:.0f}s on {nworkers} worker(s)", file=sys.stderr)
    if out:
        out.close()


if __name__ == "__main__":
    main()
