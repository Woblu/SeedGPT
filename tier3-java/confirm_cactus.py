#!/usr/bin/env python3
"""Confirm one cactus claim in a real Java world.

    python tier3-java/confirm_cactus.py <seed> <x> <z> [expected height]

The search is a fast approximation: it replays the decoration RNG exactly, but
on cubiomes' terrain, which disagrees with the game in roughly one column in ten
(overhangs, and the odd off-by-one). Since a mispredicted placement shifts every
later try in its patch, measured precision is ~80%.

That is fine for finding candidates and useless for claiming a record, so this
is the step between the two. It is non-destructive, so the world can be probed
again later.

Exit code 0 means confirmed.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from java_oracle import JavaServer


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    seed = int(sys.argv[1])
    x, z = int(sys.argv[2]), int(sys.argv[3])
    want = int(sys.argv[4]) if len(sys.argv) > 4 else None

    with JavaServer(seed, quiet=True) as srv:
        srv.run(f"forceload add {x - 32} {z - 32} {x + 32} {z + 32}")
        time.sleep(6)
        r = srv.cactus_height(x, z, 40, 140)

    if r is None:
        print(f"({x},{z}): chunk never loaded -- unknown, not disproved")
        return 3
    height, base = r
    print(f"seed {seed} at ({x},{z}): the game has "
          + (f"a {height}-block cactus with its base at y={base}"
             if height else "no cactus"))
    if want is None:
        return 0 if height else 1
    if height == want:
        print(f"CONFIRMED: {want} blocks, as claimed")
        return 0
    print(f"NOT CONFIRMED: claimed {want}, found {height}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
