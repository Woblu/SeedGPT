#!/usr/bin/env python3
"""Diff the cactus simulation against a real Java world, block for block.

    python tier3-java/check_cactus.py <seed> <x> <z> [chunk radius]

The simulation replays decoration RNG in exact order, and a single mispredicted
placement desynchronises every later try in the patch. So "looks about right" is
worthless here; the only useful question is whether it names the same columns at
the same heights as the game. Two directions, because they fail differently:

  PRECISION  every column we predict is probed in the real world, and its cactus
             run must start at the same y and be the same height.

  RECALL     every chunk is then emptied with `/fill ... replace`, which reports
             how many cactus blocks were really there. Anything above what we
             predicted is a cactus we missed entirely -- which per-column probing
             of our own predictions could never reveal.

Recall is destructive, so it runs last.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from java_oracle import JavaServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACTUS = os.path.join(ROOT, "build", "cactus.exe")

Y0, Y1 = 40, 120


def simulate(seed, x, z, radius):
    out = subprocess.run([CACTUS, str(seed), "1.21", str(x), str(z), str(radius)],
                         capture_output=True, text=True).stdout
    got = {}
    for line in out.splitlines():
        m = re.match(r"CACTUS (-?\d+) (-?\d+) (-?\d+) (\d+)", line)
        if m:
            cx, cz, by, h = (int(v) for v in m.groups())
            got[(cx, cz)] = (by, h)
    return got


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 12345
    x = int(sys.argv[2]) if len(sys.argv) > 2 else -2468
    z = int(sys.argv[3]) if len(sys.argv) > 3 else 832
    cr = int(sys.argv[4]) if len(sys.argv) > 4 else 2

    radius = cr * 16
    cx0, cx1 = (x - radius) >> 4, (x + radius) >> 4
    cz0, cz1 = (z - radius) >> 4, (z + radius) >> 4
    pred = simulate(seed, x, z, radius)
    print(f"simulation predicts {len(pred)} cactus columns "
          f"({sum(h for _, h in pred.values())} blocks) "
          f"in chunks [{cx0}..{cx1}]x[{cz0}..{cz1}]")

    # fresh=True: this script empties chunks to count them, so it must never
    # inherit a world a previous run already emptied.
    with JavaServer(seed, quiet=True, fresh=True) as srv:
        srv.run(f"forceload add {cx0 * 16 - 16} {cz0 * 16 - 16} "
                f"{cx1 * 16 + 31} {cz1 * 16 + 31}")
        time.sleep(8)   # these chunks have to generate before anything is read

        # ---- precision -------------------------------------------------
        exact = wrong = unloaded = 0
        for (px, pz), (by, h) in sorted(pred.items()):
            r = srv.cactus_height(px, pz, Y0, Y1)
            if r is None:
                unloaded += 1
                continue
            gh, gbase = r
            if gh == h and gbase == by:
                exact += 1
            else:
                wrong += 1
                if wrong <= 12:
                    real = f"height {gh} at y={gbase}" if gh else "no cactus"
                    print(f"  MISPREDICT ({px},{pz}) we say height {h} at y={by}; "
                          f"the game has {real}")
        print(f"\nprecision: {exact} exact, {wrong} wrong, {unloaded} unloaded")

        # ---- recall (destructive) --------------------------------------
        print("emptying each chunk to count what was really there...")
        missed = 0
        for ccx in range(cx0, cx1 + 1):
            for ccz in range(cz0, cz1 + 1):
                bx, bz = ccx * 16, ccz * 16
                out = srv.run(f"fill {bx} {Y0} {bz} {bx + 15} {Y1} {bz + 15} "
                              f"minecraft:air replace minecraft:cactus")
                real = 0
                for l in out:
                    m = re.search(r"filled (\d+) block", l)
                    if m:
                        real = int(m.group(1))
                ours = sum(h for (px, pz), (_, h) in pred.items()
                           if bx <= px <= bx + 15 and bz <= pz <= bz + 15)
                if real != ours:
                    missed += 1
                    if missed <= 12:
                        print(f"  CHUNK ({ccx},{ccz}) we say {ours} blocks, "
                              f"the game has {real}")
        nchunks = (cx1 - cx0 + 1) * (cz1 - cz0 + 1)
        print(f"recall: {nchunks - missed}/{nchunks} chunks match on block count")

    ok = (wrong == 0 and missed == 0)
    print("\nSIMULATION MATCHES THE GAME" if ok else "\nSIMULATION DISAGREES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
