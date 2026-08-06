#!/usr/bin/env python3
"""Hunt for buried treasure in unusual surroundings, across seeds.

The request behind this: a chest walled in by bedrock, or sharing its shell with
a spawner -- treasure that landed somewhere it has no business being. Whether
those exist at all is an empirical question, not one to argue about, so this
probes real chests and reports the distribution.

    python hunt_treasure.py [--seeds N] [--start S] [--radius R] [--json out.jsonl]

Each seed costs one server boot, so the radius is the lever: more treasures per
boot is the only thing that amortises it. Buried treasure is a 1-in-100 chunk
roll restricted to beaches and ocean floors, so even a wide radius yields few.

Reports, per run: the Y distribution of the chests, and every chest with a
neighbour on the INTERESTING list. A neighbour matching nothing on either list
is counted as unidentified and never silently folded into "ordinary".
"""
import json
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                                    # noqa: E402
from treasure_probe import locate, shell_probe, classify, INTERESTING  # noqa: E402


def main():
    nseeds, start, radius, version, jsonl = 40, 0, 3000, "1.21", None
    for i, a in enumerate(sys.argv):
        if a == "--seeds" and i + 1 < len(sys.argv):   nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):   start = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]
        if a == "--json" and i + 1 < len(sys.argv):    jsonl = sys.argv[i + 1]

    print(f"probing buried treasure, seeds {start}..{start+nseeds-1}, radius {radius}",
          flush=True)
    t0 = time.time()
    ys, blocks_seen, hits, n, unknown_total, failed = Counter(), Counter(), [], 0, 0, 0
    rows = []

    for seed in range(start, start + nseeds):
        spots = locate(seed, version, radius)
        if not spots:
            continue
        try:
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                for (x, z) in spots:
                    srv.forceload(x, z, radius_chunks=2)
                    if not srv.loaded(x, z):
                        continue                       # unknown, not absent
                    chest_ys = srv.blocks(x, z, -64, 120, "minecraft:chest")
                    if not chest_ys:
                        continue
                    for y in sorted(chest_ys):
                        shell = shell_probe(srv, x, y, z)
                        counts, interesting, unk = classify(shell)
                        n += 1
                        ys[y // 16 * 16] += 1
                        unknown_total += unk
                        for b, c in counts.items():
                            blocks_seen[b] += c
                        rows.append({"seed": seed, "x": x, "y": y, "z": z,
                                     "counts": counts, "interesting": interesting,
                                     "unknown": unk})
                        if interesting:
                            hits.append((seed, x, y, z, interesting))
                            print(f"  INTERESTING seed {seed} chest ({x},{y},{z}): "
                                  + ", ".join(f"{c}x {b.split(':')[1]}"
                                              for b, c in interesting.items()),
                                  flush=True)
        except Exception as e:
            failed += 1
            print(f"  seed {seed}: server failed ({e})", flush=True)
        finally:
            # Each world is ~60 MB. Fifty seeds is 3 GB, and a real hunt is
            # thousands -- so the world goes as soon as it has been read.
            try:
                JavaServer(seed=seed).cleanup()
            except Exception:
                pass

    el = time.time() - t0
    print(f"\n{n} chests probed across {nseeds} seeds in {el/60:.1f} min")
    if n:
        print("chest Y distribution (16-block bands):")
        for band in sorted(ys):
            print(f"  y {band:4d}..{band+15:<4d} {ys[band]}")
        print("most common neighbours:")
        for b, c in blocks_seen.most_common(10):
            print(f"  {c:5d}  {b}")
        print(f"unidentified neighbours: {unknown_total} "
              f"({100.0*unknown_total/(26*n):.1f}% of all probed)")
    if failed:
        print(f"WARNING: {failed} of {nseeds} seeds never ran (server failures). "
              f"This run did NOT cover what it was asked to.")
    print(f"{len(hits)} chest(s) with a neighbour on the interesting list")

    if jsonl:
        with open(jsonl, "a", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
