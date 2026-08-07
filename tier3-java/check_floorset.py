#!/usr/bin/env python3
"""Is the hardcoded chest-floor set actually the game's?

Everything said about buried treasure casings in this project rests on one
constant, hand-transcribed into OutpostWorldgen.chestFloor():

    { SANDSTONE, STONE, ANDESITE, GRANITE, DIORITE }

The game scans down from the ocean floor until the block BELOW is in that set,
and the whole "ore is not a stopping block, so the scan falls through it and
lands ON it" argument depends on the set being exactly right. It is NOT read from
the game -- it is a list somebody typed -- and it has never been tested directly.
A missing member would mean the scan stops earlier than we model, in a place we
never look.

The test is direct and does not need the fast path at all: ask a REAL server for
every buried treasure chest it placed, and read the block immediately beneath it.
That block is, by definition, one the game stopped on. If any of them is outside
the set, the set is incomplete.

    python check_floorset.py [seed ...]

Exit 0 only if every floor block observed is in the set.
"""
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                  # noqa: E402
from treasure_probe import locate, CANDIDATES       # noqa: E402

FLOOR = {"minecraft:sandstone", "minecraft:stone", "minecraft:andesite",
         "minecraft:granite", "minecraft:diorite"}


def main():
    seeds = [int(a) for a in sys.argv[1:]] or [4, 5000, 5001]
    seen, outside, unknown, n = Counter(), [], 0, 0

    for seed in seeds:
        spots = locate(seed, "1.21", 3000)
        with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
            for (x, z) in spots:
                srv.forceload(x, z, radius_chunks=1)
            for (x, z) in spots:
                if not srv.loaded(x, z):
                    continue
                ys = sorted(srv.blocks(x, z, 10, 110, "minecraft:chest"))
                if len(ys) != 1:
                    continue            # ambiguous column; another chest may share it
                y = ys[0]
                # One command per candidate for the single block below the chest.
                cmds = [f"execute if block {x} {y-1} {z} {b} run say F{i}"
                        for i, b in enumerate(CANDIDATES)]
                got = None
                for line in srv.run(cmds):
                    m = re.search(r"\bF(\d+)\b", line)
                    if m:
                        got = CANDIDATES[int(m.group(1))]
                        break
                n += 1
                if got is None:
                    unknown += 1        # not on the list: reported, never assumed
                    continue
                seen[got] += 1
                if got not in FLOOR:
                    outside.append((seed, x, y, z, got))
        try:
            JavaServer(seed=seed).cleanup()
        except Exception:
            pass

    print(f"{n} chests across {len(seeds)} seeds\n")
    print("block DIRECTLY BENEATH the chest -- by definition one the game stopped on:")
    for b, c in seen.most_common():
        mark = "" if b in FLOOR else "   <== NOT IN THE HARDCODED SET"
        print(f"  {c:5d}  {b}{mark}")
    if unknown:
        print(f"  {unknown:5d}  (not on the candidate list -- unidentified, "
              f"which is not the same as absent)")

    if outside:
        print(f"\nTHE SET IS INCOMPLETE. {len(outside)} chest(s) sit on a block "
              f"the set does not contain, so the game stops on blocks this "
              f"project models it as falling through. Everything derived from "
              f"the set -- which casings are reachable, how deep a chest can "
              f"get -- is affected.")
        for (s, x, y, z, b) in outside[:10]:
            print(f"  seed {s} ({x},{y},{z}) sits on {b}")
        return 1
    if unknown:
        print(f"\nNo contradiction found, but {unknown} floor block(s) could not "
              f"be identified, so this does not fully clear the set.")
        return 1
    print(f"\nEvery floor block observed is in the set. That is consistent with "
          f"the transcription being right; it is {n} chests of evidence, not a "
          f"proof, and a member the set has that the game does NOT would be "
          f"invisible to this test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
