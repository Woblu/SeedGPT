#!/usr/bin/env python3
"""Do two structures ACTUALLY share space, or do their nominal boxes just touch?

The `overlap` condition compares axis-aligned boxes of a structure's NOMINAL
size. That is a good candidate filter and a poor answer: it reported "mansion 57
blocks away, 192 overlap" for a pair whose buildings are ~15-20 apart, because
the distance is measured origin-to-origin and a mansion sprawls far past its
origin. A box overlap is not proof the buildings meet, and a large origin
distance is not proof they do not.

This asks the real world. For each structure it finds the blocks that only that
structure places, takes their real bounding box, and reports whether the two
boxes intersect -- and if not, the true gap between the nearest blocks.

    python verify_overlap.py <seed> <ax> <az> <structA> <bx> <bz> <structB>
                             [--radius 64] [--step 4]

Sampling is on a grid (default every 4 blocks), so a reported gap is accurate to
about the step. Sampling can MISS a thin protrusion, so "no overlap" from this is
"none found at this resolution", never a proof of separation -- and it says so.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer          # noqa: E402

# Blocks each structure places that ordinary terrain does not. Kept narrow on
# purpose: a block that also occurs naturally would make terrain look like
# structure and inflate every box.
# Signature blocks must be DISJOINT between the two structures, not merely
# characteristic. A first version listed cobblestone under both mansion and
# outpost, so each structure's box could be grown by the other's blocks -- which
# would manufacture exactly the overlap this is supposed to test for. Anything
# shared is therefore excluded, even where it is the most common block in the
# build.
SIGNATURE = {
    # dark oak and the andesite floor are the mansion's alone here.
    "mansion": ["minecraft:dark_oak_planks", "minecraft:dark_oak_log",
                "minecraft:polished_andesite", "minecraft:red_carpet",
                "minecraft:blue_carpet", "minecraft:white_carpet",
                "minecraft:bookshelf", "minecraft:dark_oak_stairs"],
    # the outpost's tent wool and its oak fencing; NOT cobblestone, which the
    # mansion also uses.
    "outpost": ["minecraft:white_wool", "minecraft:oak_fence",
                "minecraft:oak_log", "minecraft:birch_planks",
                "minecraft:oak_stairs"],
    "village": ["minecraft:bell", "minecraft:composter",
                "minecraft:fletching_table", "minecraft:smithing_table",
                "minecraft:cartography_table", "minecraft:loom"],
    "trial_chambers": ["minecraft:tuff_bricks", "minecraft:chiseled_tuff",
                       "minecraft:copper_bulb", "minecraft:vault"],
    "ancient_city": ["minecraft:deepslate_bricks", "minecraft:sculk_shrieker",
                     "minecraft:sculk_catalyst", "minecraft:soul_lantern"],
}


def box_of(srv, cx, cz, blocks, radius, step, y0=40, y1=140):
    """Real bounding box of any of `blocks` near (cx,cz), or None if none found."""
    cmds, pts = [], []
    for x in range(cx - radius, cx + radius + 1, step):
        for z in range(cz - radius, cz + radius + 1, step):
            for y in range(y0, y1 + 1, step):
                pts.append((x, y, z))
    for i, (x, y, z) in enumerate(pts):
        for b in blocks:
            cmds.append(f"execute if block {x} {y} {z} {b} run say P{i}")
    found = set()
    for line in srv.run(cmds):
        for m in re.finditer(r"\bP(\d+)\b", line):
            found.add(int(m.group(1)))
    if not found:
        return None, 0
    hit = [pts[i] for i in sorted(found)]
    xs = [p[0] for p in hit]; ys = [p[1] for p in hit]; zs = [p[2] for p in hit]
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)), len(hit)


def gap1d(a0, a1, b0, b1):
    """0 if the intervals meet, else the distance between them."""
    if a1 >= b0 and b1 >= a0:
        return 0
    return b0 - a1 if b0 > a1 else a0 - b1


def main():
    if len(sys.argv) < 8:
        sys.exit(__doc__)
    seed = int(sys.argv[1])
    ax, az, na = int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    bx, bz, nb = int(sys.argv[5]), int(sys.argv[6]), sys.argv[7]
    radius, step = 64, 4
    for i, a in enumerate(sys.argv):
        if a == "--radius" and i + 1 < len(sys.argv): radius = int(sys.argv[i + 1])
        if a == "--step" and i + 1 < len(sys.argv):   step = int(sys.argv[i + 1])

    sa = SIGNATURE.get(na) or sys.exit(f"no signature blocks for {na!r}")
    sb = SIGNATURE.get(nb) or sys.exit(f"no signature blocks for {nb!r}")

    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        for (x, z) in ((ax, az), (bx, bz)):
            srv.forceload(x, z, radius_chunks=(radius // 16) + 2)
        boxa, na_hits = box_of(srv, ax, az, sa, radius, step)
        boxb, nb_hits = box_of(srv, bx, bz, sb, radius, step)
    try:
        JavaServer(seed=seed).cleanup()
    except Exception:
        pass

    print(f"seed {seed}")
    print(f"  {na} at ({ax},{az}): {na_hits} sampled blocks, box {boxa}")
    print(f"  {nb} at ({bx},{bz}): {nb_hits} sampled blocks, box {boxb}")
    if not boxa or not boxb:
        print("\n  One structure's blocks were not found at this resolution. "
              "That is UNKNOWN, not 'not overlapping'.")
        return 2

    # A structure's own blocks must sit near its own origin. When they do not,
    # the signature is picking up the OTHER structure and the overlap it reports
    # is manufactured. This caught exactly that: an "outpost" box 40+ blocks from
    # the outpost, built from 4 blocks that were really the mansion's, because a
    # woodland mansion also contains oak logs, birch planks and oak stairs.
    # Disjoint-looking material lists are not disjoint.
    for (nm, box, ox, oz, nh) in ((na, boxa, ax, az, na_hits),
                                  (nb, boxb, bx, bz, nb_hits)):
        cx = (box[0] + box[3]) // 2
        cz = (box[2] + box[5]) // 2
        off = int(((cx - ox) ** 2 + (cz - oz) ** 2) ** 0.5)
        if off > 40 or nh < 8:
            print(f"\n  SUSPECT: the {nm} box is centred {off} blocks from the "
                  f"{nm} itself, from {nh} sampled blocks. Its signature is "
                  f"almost certainly matching the other structure's materials, "
                  f"so any overlap below is NOT trustworthy.")
            return 3

    dx = gap1d(boxa[0], boxa[3], boxb[0], boxb[3])
    dy = gap1d(boxa[1], boxa[4], boxb[1], boxb[4])
    dz = gap1d(boxa[2], boxa[5], boxb[2], boxb[5])
    origin_d = int(((ax - bx) ** 2 + (az - bz) ** 2) ** 0.5)
    print(f"\n  origin-to-origin distance : {origin_d} blocks "
          f"(what the search reports)")
    print(f"  real gap x/y/z            : {dx} / {dy} / {dz}")
    if dx == 0 and dy == 0 and dz == 0:
        print("\n  => THEY REALLY OVERLAP. The two structures' own blocks occupy "
              "the same space, confirmed in a generated world.")
        return 0
    flat = max(dx, dz)
    if dx == 0 and dz == 0:
        print(f"\n  => STACKED, not intersecting: they share ground area but are "
              f"{dy} blocks apart vertically.")
        return 1
    print(f"\n  => NOT overlapping at this resolution: {flat} blocks apart "
          f"horizontally. Sampling every {step} blocks can miss a thin "
          f"protrusion, so this is 'none found', not proof of separation.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
