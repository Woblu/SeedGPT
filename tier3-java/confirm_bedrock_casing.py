#!/usr/bin/env python3
"""Settle a bedrock-cased buried treasure against a REAL pre-1.18 server.

Everything this project knows about the bedrock casing is DERIVED: the rule read
out of BuriedTreasurePiece's bytecode, the carved column read out of the 1.16.5
generator, and the arithmetic joining them. None of it has been observed. This
observes it.

    python confirm_bedrock_casing.py <seed> <x> <z> [--y0 0] [--y1 80]

The chest is expected in the BEDROCK BAND (y1-4), which is why this exists
instead of confirm_casing.py -- that one scans y 10..110 and would miss a chest
at y=4 entirely, reporting "no chest" for the one case worth checking.

Needs a 1.16.5 server jar and a JDK that can run it (1.16.5 predates Java 17 and
will not boot on 21). Both come from the environment:

    MC_SERVER_JAR=tier3-java/server-1.16.5.jar
    MC_JAVA="C:/Program Files/Eclipse Adoptium/jdk-17.../bin/java.exe"

What would falsify the whole account, and is therefore what to look at first:
the block directly BELOW the chest must be one of sandstone/stone/andesite/
granite/diorite. If it is bedrock, the descent rule as decoded is wrong.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                  # noqa: E402
from treasure_casing import FACES                   # noqa: E402

FLOOR_SET = ("minecraft:sandstone", "minecraft:stone", "minecraft:andesite",
             "minecraft:granite", "minecraft:diorite")

# Small enough to probe exhaustively, and it must include bedrock -- the block
# this whole exercise is about -- plus lava and water, which the casing loop
# treats as overwritable and which fill the bottom of a ravine.
CANDIDATES = [
    "minecraft:bedrock", "minecraft:stone", "minecraft:sandstone",
    "minecraft:andesite", "minecraft:granite", "minecraft:diorite",
    "minecraft:sand", "minecraft:gravel", "minecraft:dirt", "minecraft:water",
    "minecraft:lava", "minecraft:air", "minecraft:cave_air", "minecraft:obsidian",
    "minecraft:chest", "minecraft:coal_ore", "minecraft:iron_ore",
    "minecraft:gold_ore", "minecraft:redstone_ore", "minecraft:diamond_ore",
    "minecraft:lapis_ore", "minecraft:deepslate", "minecraft:tuff",
]


def identify(srv, spots):
    """One round trip for every (label, x, y, z): label -> block id or None."""
    cmds, out = [], {}
    keys = list(spots)
    for si, k in enumerate(keys):
        x, y, z = spots[k]
        for ci, b in enumerate(CANDIDATES):
            cmds.append(f"execute if block {x} {y} {z} {b} run say Q{si}_{ci}")
    lines = srv.run(cmds)
    hits = {}
    for line in lines:
        for m in re.finditer(r"\bQ(\d+)_(\d+)\b", line):
            hits.setdefault(int(m.group(1)), []).append(CANDIDATES[int(m.group(2))])
    for si, k in enumerate(keys):
        got = hits.get(si, [])
        out[k] = got[0] if got else None      # None = not on the list, UNKNOWN
    return out


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    seed, x, z = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    y0, y1 = 0, 80
    for i, a in enumerate(sys.argv):
        if a == "--y0" and i + 1 < len(sys.argv): y0 = int(sys.argv[i + 1])
        if a == "--y1" and i + 1 < len(sys.argv): y1 = int(sys.argv[i + 1])

    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        srv.forceload(x, z, radius_chunks=1)
        if not srv.loaded(x, z):
            print("chunk would not load -- UNKNOWN, not a verdict")
            return 2
        ys = sorted(srv.blocks(x, z, y0, y1, "minecraft:chest"))
        if not ys:
            print(f"no chest in the column at ({x},{z}) between y{y0} and y{y1} "
                  f"-- UNKNOWN rather than a verdict: the chest may be outside "
                  f"this range, and 'not found here' is not 'not there'.")
            return 2
        y = ys[0]
        print(f"seed {seed}: chest at ({x},{y},{z})")

        spots = {"below": (x, y - 1, z)}
        names = ["west", "east", "down", "up", "north", "south"]
        for nm, (dx, dy, dz) in zip(names, FACES):
            spots[nm] = (x + dx, y + dy, z + dz)
        for cy in range(max(y0, 0), min(y + 4, y1) + 1):
            spots[f"y{cy}"] = (x, cy, z)
        got = identify(srv, spots)

        print("\n  the six faces:")
        bedrock_faces = 0
        for nm in names:
            b = got.get(nm)
            if b == "minecraft:bedrock":
                bedrock_faces += 1
            print(f"    {nm:<6} {b or 'UNKNOWN (not on the candidate list)'}")

        below = got.get("below")
        print(f"\n  block below the chest: {below or 'UNKNOWN'}")
        if below in FLOOR_SET:
            print("    -> in the landing set, as the decoded rule requires")
        elif below == "minecraft:bedrock":
            print("    -> BEDROCK. The decoded descent rule says this is "
                  "IMPOSSIBLE; the rule is wrong.")
        elif below is not None:
            print("    -> NOT in the landing set. Either decoration replaced it "
                  "after placement, or the rule is wrong.")

        print("\n  column:")
        for cy in range(max(y0, 0), min(y + 4, y1) + 1):
            b = got.get(f"y{cy}")
            mark = "  <== chest" if cy == y else ""
            print(f"    y{cy:<3} {b or 'UNKNOWN'}{mark}")

        print(f"\n  {bedrock_faces} of 6 faces are bedrock")
        if bedrock_faces >= 4:
            print("  => BEDROCK-CASED. Confirmed against a real server.")
            rc = 0
        elif bedrock_faces:
            print(f"  => partially bedrock ({bedrock_faces}/6).")
            rc = 0
        else:
            print("  => NOT bedrock-cased.")
            rc = 1
    try:
        JavaServer(seed=seed).cleanup()
    except Exception:
        pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
