#!/usr/bin/env python3
"""Hunt for a village with N blacksmiths, across seeds.

The two-tier flow this project always intended and never had: cubiomes finds
villages in microseconds, and the real server -- the only thing that knows what
the jigsaw assembler actually built -- counts the workstations on the survivors.

    python hunt_smiths.py --min 5 --seeds 200 [--radius 1500] [--start 0]

Cost is dominated by the server, not the search: ~14 s to boot one world plus
~8 s per village. So the radius is the lever -- a bigger radius means more
villages per boot, which is the only thing that amortises.

Every count carries its positive control (a village has one bell), and a village
whose bell is missing is reported as unknown rather than as zero. `bells > 1`
means the box caught more than one village meeting point, so that line is a
CLUSTER of villages, not one village -- stated rather than hidden, because "five
smiths" means different things for the two and only the caller knows which they
wanted.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                      # noqa: E402
from village_smiths import count_smiths, locate_villages  # noqa: E402


def main():
    want, nseeds, radius, start, version = 5, 100, 1500, 0, "1.21"
    for i, a in enumerate(sys.argv):
        if a == "--min" and i + 1 < len(sys.argv):    want = int(sys.argv[i + 1])
        if a == "--seeds" and i + 1 < len(sys.argv):  nseeds = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv): radius = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):  start = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]

    print(f"hunting a village with >= {want} smiths, seeds {start}..{start+nseeds-1}, "
          f"radius {radius}", flush=True)
    t0 = time.time()
    best, best_at, checked, found, failed = 0, None, 0, [], 0

    for seed in range(start, start + nseeds):
        spots = locate_villages(seed, version, radius)
        if not spots:
            continue
        try:
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                for (x, z) in spots:
                    got, bells = count_smiths(srv, x, z)
                    if got is None or bells == 0:
                        continue                     # unknown, never counted as 0
                    checked += 1
                    total = sum(got.values())
                    if total > best:
                        best, best_at = total, (seed, x, z, bells, dict(got))
                        print(f"  best so far: seed {seed} ({x},{z}) smiths={total} "
                              f"bells={bells} {got}", flush=True)
                    if total >= want:
                        found.append((seed, x, z, bells, dict(got)))
                        print(f"HIT seed {seed} village ({x},{z}) smiths={total} "
                              f"bells={bells} {got}", flush=True)
        except Exception as e:                       # a dead server must not end the hunt
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
    print(f"\n{checked} villages checked across {nseeds} seeds in {el/60:.1f} min "
          f"({el/max(checked,1):.1f}s per village)")
    if best_at:
        s, x, z, b, g = best_at
        print(f"best: seed {s} village ({x},{z}) smiths={best} bells={b} {g}")
    print(f"{len(found)} village(s) met the >= {want} bar")
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())
