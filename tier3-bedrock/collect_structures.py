#!/usr/bin/env python3
"""Harvest Bedrock structure positions from the oracle, in bulk.

`/locate structure X` only reports the NEAREST one to the caller, which would be
a single data point per server start -- far too slow to reverse a placement
grid. But console commands can be relocated:

    execute positioned <x> 64 <z> run locate structure village

so sweeping a grid of query origins inside ONE world yields dozens of distinct
structures for the price of one ~3s server start. That is what makes fitting
Bedrock's placement affordable at all.

    python collect_structures.py --seed 12345 --structure village --reach 12000
    python collect_structures.py --seeds 1,2,3 --structure pillager_outpost --json out.jsonl
"""

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, RE_FOUND, EXE          # noqa: E402


def sweep(bds, structure, reach, step):
    """Every distinct structure position reachable from a grid of query points."""
    found = {}
    for qx in range(-reach, reach + 1, step):
        for qz in range(-reach, reach + 1, step):
            lines = bds.command(
                f"execute positioned {qx} 64 {qz} run locate structure {structure}",
                timeout=30)
            for line in lines:
                m = RE_FOUND.search(line)
                if m:
                    p = (int(m.group("x")), int(m.group("z")))
                    # Keep the query point that found it: a position reported
                    # from far away may be outside the swept area, and knowing
                    # which origin produced it makes that visible.
                    found.setdefault(p, (qx, qz))
                    break
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int)
    ap.add_argument("--seeds", help="comma-separated")
    ap.add_argument("--structure", default="village")
    ap.add_argument("--reach", type=int, default=8000, help="sweep +/- this many blocks")
    ap.add_argument("--step", type=int, default=1000, help="spacing of query origins")
    ap.add_argument("--json", help="append results as JSON lines")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    seeds = ([a.seed] if a.seed is not None else []) + \
            ([int(s) for s in a.seeds.split(",")] if a.seeds else [])
    if not seeds:
        seeds = [12345]

    out = open(a.json, "a", encoding="utf-8") if a.json else None
    for seed in seeds:
        t0 = time.time()
        with Bds(seed, verbose=a.verbose) as bds:
            found = sweep(bds, a.structure, a.reach, a.step)
        rec = {"seed": seed, "structure": a.structure,
               "positions": sorted(found.keys()),
               "seconds": round(time.time() - t0, 1)}
        print(f"seed {seed}: {len(found)} distinct {a.structure} "
              f"in {rec['seconds']}s", file=sys.stderr)
        print(json.dumps(rec))
        if out:
            out.write(json.dumps(rec) + "\n"); out.flush()
    if out:
        out.close()


if __name__ == "__main__":
    main()
