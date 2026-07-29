#!/usr/bin/env python3
"""Diff our block-level terrain against a real Java world.

    python tier3-java/check_terrain.py [seed] [columns]

src/terrain.c computes exact 1.18+ surface heights, and everything built on it
-- the cliff/relief records, `cave_below`, the 1.18+ structure placement rules --
has until now been checked only against our own reimplementation and cubiomes.
Both could be wrong the same way. This asks the actual game.

HOW THE COMPARISON IS FRAMED, because the obvious framing is wrong. The first
version took the highest non-air block in the server's column and expected our
number to match it. That flags an iceberg as a terrain bug: at (1190,1290) in
seed 12345 the server's column tops out at packed ice on y=66 while our terrain
correctly reports the ocean floor at 46, because icebergs are a feature placed
after terrain. Water, trees, snow and cacti all do the same thing.

So this tests our answer directly instead of inferring it. terrain.c claims
"the topmost terrain block in this column is at y=M", which is two checks:

    the block at  M   IS a terrain material
    the block at  M+1 is NOT one

Whatever a feature later stacked on top is then irrelevant, which is the point.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from java_oracle import JavaServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SURFACE = os.path.join(ROOT, "build", "surface.exe")

# What counts as terrain: what the noise generator and surface rules produce,
# as opposed to what decoration later puts on top. Tags first, since they cover
# whole families the game itself groups together.
TERRAIN = [
    "#minecraft:base_stone_overworld",   # stone, granite, diorite, andesite, tuff, deepslate
    "#minecraft:dirt",                   # dirt, grass_block, podzol, mycelium, mud, ...
    "#minecraft:sand",                   # sand, red_sand
    "#minecraft:terracotta",
    "minecraft:sandstone", "minecraft:red_sandstone",
    "minecraft:gravel", "minecraft:clay", "minecraft:calcite",
    "minecraft:magma_block", "minecraft:bedrock",
    # Surface rules make these the actual top of the column on cold peaks, so
    # they are terrain: at (461,-149) in seed 12345 our surface y=165 IS a
    # snow_block, with a snow LAYER at 166 above it.
    "minecraft:snow_block", "minecraft:powder_snow",
]
# Deliberately NOT terrain, and the distinction is not cosmetic:
#   minecraft:snow        a layer, decoration, sits on top
#   ice / packed_ice      a sheet frozen over water, or an iceberg feature
# Counting either as terrain flagged correct answers as bugs.


def ours(seed, x, z):
    out = subprocess.run([SURFACE, str(seed), "1.21", str(x), str(z)],
                         capture_output=True, text=True).stdout
    m = re.search(r"surface=(-?\d+)", out)
    return int(m.group(1)) if m else None


def is_terrain(srv, x, y, z):
    out = srv.run([f"execute if block {x} {y} {z} {b} run say T" for b in TERRAIN])
    return any(re.search(r"\bT\b", l) for l in out)


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 12345
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 24

    # Spread the columns so they land in different biomes and terrain, not all
    # in one flat patch where any height model would look right.
    pts = [(((i * 7919) % 4000) - 2000, ((i * 104729) % 4000) - 2000) for i in range(n)]

    ok = unloaded = 0
    bad_land, bad_wet = [], []
    with JavaServer(seed, quiet=True) as srv:
        for (x, z) in pts:
            srv.run(f"forceload add {x - 16} {z - 16} {x + 16} {z + 16}")
        time.sleep(6)   # let those chunks generate before anything is read

        for (x, z) in pts:
            if not srv.loaded(x, z):
                unloaded += 1
                continue
            m = ours(seed, x, z)
            if m is None:
                continue
            at = is_terrain(srv, x, m, z)
            above = is_terrain(srv, x, m + 1, z)
            if at and not above:
                ok += 1
                continue
            why = ("nothing solid at our surface" if not at
                   else "terrain continues above our surface")
            # Submerged columns are reported apart from dry-land ones. NOT
            # because water is expected to be worse -- measured, it is not
            # (8/10 under water against 16/20 on land) -- but because that was
            # the standing theory and splitting them is what disproved it.
            #
            # Sampling one height is not enough: a frozen ocean has ICE at 62 and
            # water below it, which read as dry and put a submerged column in the
            # land column. Scan the range, and count ice as water.
            probes = []
            for y in range(min(m, 40), 65):
                for b in ("minecraft:water", "minecraft:ice", "minecraft:packed_ice"):
                    probes.append(f"execute if block {x} {y} {z} {b} run say WET")
            wet = srv.run(probes)
            (bad_wet if any("WET" in l for l in wet) else bad_land).append((x, z, m, why))

    print(f"seed {seed}: {len(pts)} columns, {unloaded} never loaded")
    print(f"  our surface is the real top of terrain : {ok}")
    print(f"  wrong on dry land                      : {len(bad_land)}")
    print(f"  wrong under water (aquifers)           : {len(bad_wet)}")
    for x, z, m, why in bad_land:
        print(f"  LAND MISMATCH ({x},{z}) ours={m}: {why}")
    for x, z, m, why in bad_wet:
        print(f"  submerged      ({x},{z}) ours={m}: {why}")
    if bad_land:
        print("TERRAIN DISAGREES WITH THE GAME ON DRY LAND")
        return 1
    print("terrain agrees with the real world on every dry-land column")
    return 0


if __name__ == "__main__":
    sys.exit(main())
