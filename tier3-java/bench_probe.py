#!/usr/bin/env python3
"""Where does the time in an ore hunt actually go?

Optimising this by intuition has been wrong twice, so measure the four costs
separately before touching any of them:

    boot     starting a server and generating its spawn area
    gen      forceloading a treasure's chunks   (suspected dominant)
    scan     finding the chest's y in the column (100 commands)
    probe    identifying the six faces          (216 commands)

The distinction that matters is commands versus chunk generation. If commands
dominate, the win is asking fewer of them -- a destructive /fill ... replace
counts a whole column in one command instead of a hundred. If generation
dominates, command count is nearly free and the only lever is generating fewer
chunks or running more servers at once.

    python bench_probe.py [seed] [n_chests]
"""
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                 # noqa: E402
from treasure_probe import locate                  # noqa: E402
from hunt_treasure_ore import face_probe, SCREEN   # noqa: E402


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 777
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    spots = locate(seed, "1.21", 3000)[:n]
    if not spots:
        sys.exit("no treasures for that seed")
    t = Counter()

    t0 = time.time()
    srv = JavaServer(seed=seed, fresh=True, quiet=True)
    srv.start()
    t["boot"] = time.time() - t0

    chests = 0
    try:
        for (x, z) in spots:
            a = time.time()
            srv.forceload(x, z, radius_chunks=1)
            ok = srv.loaded(x, z)
            t["gen"] += time.time() - a
            if not ok:
                continue
            a = time.time()
            ys = srv.blocks(x, z, 10, 110, "minecraft:chest")
            t["scan"] += time.time() - a
            for y in sorted(ys):
                a = time.time()
                face_probe(srv, x, y, z, SCREEN)
                t["probe"] += time.time() - a
                chests += 1
    finally:
        srv.stop()
        srv.cleanup()

    total = sum(t.values())
    print(f"seed {seed}: {len(spots)} treasures, {chests} chests, "
          f"{total:.1f}s total\n")
    for k in ("boot", "gen", "scan", "probe"):
        share = 100 * t[k] / total if total else 0
        per = f"  ({t[k]/chests:.2f}s per chest)" if chests and k != "boot" else ""
        print(f"  {k:6s} {t[k]:7.1f}s  {share:4.1f}%{per}")
    if chests:
        print(f"\n  per chest overall: {total/chests:.2f}s")
    cmds = t["scan"] + t["probe"]
    if cmds > t["gen"]:
        print("\n=> COMMANDS dominate: fewer questions is the win "
              "(/fill ... replace counts a column in one)")
    else:
        print("\n=> CHUNK GENERATION dominates: command count is nearly free, "
              "so the levers are fewer chunks and more servers at once")
    return 0


if __name__ == "__main__":
    sys.exit(main())
