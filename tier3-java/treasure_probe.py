#!/usr/bin/env python3
"""What is actually around a buried treasure chest, in the real world?

Buried treasure normally sits under a cap of sand or gravel on a beach or ocean
floor. Sometimes it does not: the chest is placed into whatever terrain the
column happens to hold, and where that terrain is unusual the chest ends up
encased in something unusual -- the cases people hunt for, like a chest walled
in by bedrock or sharing its shell with a monster spawner.

cubiomes can say WHERE a treasure is. It cannot say what surrounds it, because
that is the finished world after every generation stage, not a placement
formula. So this asks the real server, which is the only thing that knows.

    python treasure_probe.py <seed> [radius] [--version 1.21] [--json out.jsonl]

IDENTIFICATION IS BY CANDIDATE LIST, not by reading the block. `/execute if
block` can only ask "is it X?", and `/data get block` only speaks for block
entities, so a neighbour that matches nothing on the list is reported as
UNKNOWN -- never as air, and never as ordinary. That distinction is the whole
point: an unknown neighbour might be the interesting one.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from java_oracle import JavaServer          # noqa: E402

LOCATE = os.path.join(ROOT, "build", "locate.exe")

# What a buried treasure is normally packed in. Being on this list is what makes
# a chest boring; the list is deliberately generous so that "interesting" means
# interesting rather than merely unlisted.
ORDINARY = [
    "minecraft:sand", "minecraft:red_sand", "minecraft:gravel", "minecraft:dirt",
    "minecraft:coarse_dirt", "minecraft:rooted_dirt", "minecraft:clay",
    "minecraft:stone", "minecraft:cobblestone", "minecraft:andesite",
    "minecraft:diorite", "minecraft:granite", "minecraft:tuff",
    "minecraft:deepslate", "minecraft:sandstone", "minecraft:water",
    "minecraft:air", "minecraft:seagrass", "minecraft:tall_seagrass",
    "minecraft:kelp", "minecraft:kelp_plant", "minecraft:grass_block",
    "minecraft:moss_block", "minecraft:dripstone_block", "minecraft:calcite",
    "minecraft:snow_block", "minecraft:powder_snow", "minecraft:terracotta",
]

# The reasons to care. Bedrock and spawner are the two the request named; the
# rest are the other ways a chest can end up somewhere it has no business being.
INTERESTING = [
    "minecraft:bedrock", "minecraft:spawner", "minecraft:trial_spawner",
    "minecraft:obsidian", "minecraft:crying_obsidian", "minecraft:ancient_debris",
    "minecraft:diamond_ore", "minecraft:deepslate_diamond_ore",
    "minecraft:emerald_ore", "minecraft:deepslate_emerald_ore",
    "minecraft:gold_ore", "minecraft:redstone_ore", "minecraft:lapis_ore",
    # Every deepslate variant, not just the two rare ones. Without these a chest
    # encased in deepslate iron came back UNIDENTIFIED -- honest, but not found,
    # which for a hunt is the same as missing it.
    "minecraft:deepslate_iron_ore", "minecraft:deepslate_copper_ore",
    "minecraft:deepslate_coal_ore", "minecraft:deepslate_gold_ore",
    "minecraft:deepslate_redstone_ore", "minecraft:deepslate_lapis_ore",
    "minecraft:raw_iron_block", "minecraft:raw_copper_block",
    "minecraft:raw_gold_block", "minecraft:iron_block",
    "minecraft:budding_amethyst", "minecraft:amethyst_block",
    "minecraft:amethyst_cluster", "minecraft:chest", "minecraft:trapped_chest",
    "minecraft:mossy_cobblestone", "minecraft:rail", "minecraft:oak_planks",
    "minecraft:dark_oak_planks", "minecraft:oak_fence", "minecraft:cobweb",
    "minecraft:lava", "minecraft:magma_block", "minecraft:sculk",
    "minecraft:sculk_catalyst", "minecraft:sculk_shrieker", "minecraft:sculk_vein",
    "minecraft:reinforced_deepslate", "minecraft:end_portal_frame",
    "minecraft:iron_bars", "minecraft:blue_ice", "minecraft:packed_ice",
    "minecraft:ice", "minecraft:prismarine", "minecraft:dark_prismarine",
    "minecraft:sea_lantern", "minecraft:wet_sponge", "minecraft:tnt",
    "minecraft:copper_ore", "minecraft:iron_ore", "minecraft:coal_ore",
    "minecraft:netherrack", "minecraft:soul_sand", "minecraft:glowstone",
]

CANDIDATES = ORDINARY + INTERESTING

# Ore-only, for a targeted hunt: probing 26 neighbours against 16 blocks instead
# of 60 is a quarter of the commands, and a hunt is bounded by how many chests it
# can reach, not by how carefully it describes each one.
ORES = [b for b in INTERESTING if b.endswith("_ore")
        or b.endswith("raw_iron_block") or b.endswith("raw_copper_block")
        or b.endswith("raw_gold_block") or b == "minecraft:iron_block"]


def locate(seed, version, radius):
    """Every buried treasure in range, from the fast side."""
    r = subprocess.run([LOCATE, str(seed), version, "buried_treasure", str(radius)],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2:
            out.append((int(parts[0]), int(parts[1])))
    return out


def shell_probe(srv, x, y, z, candidates=None):
    """Identify all 26 neighbours of (x,y,z) in ONE server round trip.

    Calling identify() per position would be 26 round trips plus 26 positive
    controls; console commands all run in the same tick, so asking everything at
    once costs about what asking once costs.
    """
    offsets = [(dx, dy, dz)
               for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
               if (dx, dy, dz) != (0, 0, 0)]
    cands = candidates or CANDIDATES
    cmds = []
    for oi, (dx, dy, dz) in enumerate(offsets):
        for ci, b in enumerate(cands):
            cmds.append(f"execute if block {x+dx} {y+dy} {z+dz} {b} run say P{oi}_{ci}")
    out = srv.run(cmds)
    hits = {}
    for line in out:
        for m in re.finditer(r"\bP(\d+)_(\d+)\b", line):
            oi, ci = int(m.group(1)), int(m.group(2))
            hits.setdefault(offsets[oi], []).append(cands[ci])
    result = {}
    for off in offsets:
        got = hits.get(off, [])
        result[off] = got[0] if got else None      # None = not on the list
    return result


def classify(shell):
    counts, unknown = {}, 0
    for blk in shell.values():
        if blk is None:
            unknown += 1
        else:
            counts[blk] = counts.get(blk, 0) + 1
    interesting = {b: n for b, n in counts.items() if b in INTERESTING}
    return counts, interesting, unknown


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = int(sys.argv[1])
    radius = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 3000
    version = "1.21"
    jsonl = None
    for i, a in enumerate(sys.argv):
        if a == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]
        if a == "--json" and i + 1 < len(sys.argv):
            jsonl = sys.argv[i + 1]

    spots = locate(seed, version, radius)
    print(f"seed {seed}: {len(spots)} buried treasure(s) within {radius} blocks")
    if not spots:
        return 1

    rows = []
    with JavaServer(seed=seed, fresh=True) as srv:
        for (x, z) in spots:
            srv.forceload(x, z, radius_chunks=2)
            if not srv.loaded(x, z):
                print(f"  ({x},{z}) chunk would not load -- UNKNOWN, not reported as absent")
                continue
            ys = srv.blocks(x, z, -64, 120, "minecraft:chest")
            if not ys:
                # The treasure is placed by the structure, so no chest here means
                # the column was overwritten later or the placement did not take.
                print(f"  ({x},{z}) no chest in the column -- structure did not survive generation")
                continue
            for y in sorted(ys):
                shell = shell_probe(srv, x, y, z)
                counts, interesting, unknown = classify(shell)
                top = ", ".join(f"{n}x {b.split(':')[1]}"
                                for b, n in sorted(counts.items(), key=lambda kv: -kv[1])[:4])
                flag = ""
                if interesting:
                    flag = "  <== " + ", ".join(
                        f"{n}x {b.split(':')[1]}"
                        for b, n in sorted(interesting.items(), key=lambda kv: -kv[1]))
                print(f"  chest ({x},{y},{z})  {top}"
                      + (f"  [{unknown} unidentified]" if unknown else "") + flag)
                rows.append({"seed": seed, "x": x, "y": y, "z": z,
                             "counts": {b: n for b, n in counts.items()},
                             "interesting": interesting, "unknown": unknown})

    if jsonl:
        with open(jsonl, "a", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
