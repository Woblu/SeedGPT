#!/usr/bin/env python3
"""Is the "floating island" the search found actually floating?

The condition is computed from cubiomes' density function, and the definition
had to be tightened twice: "air at the cap's height" called ~24% of outposts
islands and "air a few blocks lower" still called 18%, because on a hillside the
ground genuinely is absent at those heights. Only the real game settles whether
the current definition means anything.

    python verify_floating.py <seed> <x> <z> [--cap Y] [--gap 24]

Reads the actual column at (x,z) and at eight points `gap` blocks out, and prints
what is solid where. An island shows: solid cap, air beneath it, and every
surrounding column topping out BELOW the cap.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer      # noqa: E402

# Classify by what is NOT terrain, not by a list of what is. An 18-block SOLID
# list called y=103 empty when it held pale_oak_leaves, which made a tree canopy
# read as absent and a real disagreement look like a different one. The set of
# things that are NOT ground is small and enumerable; the set of things that ARE
# is not, so the whitelist was always going to lie eventually.
NOT_TERRAIN = [
    "minecraft:air", "minecraft:cave_air", "minecraft:void_air",
    "minecraft:water", "minecraft:lava", "minecraft:vine", "minecraft:snow",
    "minecraft:short_grass", "minecraft:tall_grass", "minecraft:fern",
    "minecraft:large_fern", "minecraft:dead_bush", "minecraft:seagrass",
    "minecraft:kelp", "minecraft:kelp_plant", "minecraft:glow_lichen",
    "minecraft:hanging_roots", "minecraft:moss_carpet",
]
LEAFY = ("_leaves", "_log", "_wood", "_sapling", "_stem", "_hyphae",
         "_planks", "_fence", "_slab", "_stairs")
AIRY = ("minecraft:air", "minecraft:cave_air", "minecraft:void_air")


def is_terrain(block):
    """Ground, as opposed to air, liquid, plants or anything built."""
    if block is None:
        return None                      # UNKNOWN -- never silently "no"
    if block in NOT_TERRAIN:
        return False
    return not any(block.endswith(sfx) for sfx in LEAFY)


DIRS = [(1,0),(-1,0),(0,1),(0,-1),(1,1),(1,-1),(-1,1),(-1,-1)]


def top_solid(srv, x, z, y0, y1):
    """Highest y in [y0,y1] holding one of the solid blocks, else None."""
    best = None
    for b in SOLID:
        for y in srv.blocks(x, z, y0, y1, b):
            if best is None or y > best:
                best = y
    return best


# UNDER REPAIR -- this script does not currently give a trustworthy answer.
#
# Two faults are known and neither is fixed:
#   1. top_solid() still references a SOLID list that was deleted when the
#      classification moved to is_terrain(). It raises NameError.
#   2. Before that, the batched column scan was returning implausible readings --
#      surrounding surfaces at y=1 and y=14 where the real terrain is 60-80 --
#      which means results were being lost, not measured. Sweeping ~260 heights
#      across nine columns in one batch is the likely cause.
#
# It refuses rather than answering, because everything downstream treats this as
# ground truth: the floating-island condition is only ever going to be as
# trustworthy as its confirmer, and a confirmer that quietly reports a wrong
# surface height would certify islands that are not there. The repair is a
# binary search for the surface per column instead of a full sweep.
BROKEN = ("verify_floating is under repair and will not report a verdict.\n"
          "  - top_solid() references a deleted SOLID list (NameError)\n"
          "  - the batched column scan loses results: surrounding surfaces read\n"
          "    y=1 and y=14 where the terrain is 60-80\n"
          "Fix: binary-search each column for its surface instead of sweeping\n"
          "260 heights across nine columns in one batch. Until then this is\n"
          "UNKNOWN, which is not a statement about whether anything floats.")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    print(BROKEN)
    return 2
    seed, x, z = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    gap = 24
    for i, a in enumerate(sys.argv):
        if a == "--gap" and i + 1 < len(sys.argv):
            gap = int(sys.argv[i + 1])

    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        srv.forceload(x, z, radius_chunks=3)
        for dx, dz in DIRS:
            srv.forceload(x + dx * gap, z + dz * gap, radius_chunks=1)
        if not srv.loaded(x, z):
            print("chunk would not load -- UNKNOWN, not a verdict")
            return 2
        cap = top_solid(srv, x, z, -60, 200)
        if cap is None:
            print("no solid block in the column at all")
            return 2
        # Walk down through the slab, then measure the void under it.
        solid = set()
        for b in SOLID:
            solid.update(srv.blocks(x, z, -60, 200, b))
        base = cap
        while base - 1 in solid:
            base -= 1
        void = 0
        y = base - 1
        while y > -60 and y not in solid:
            void += 1
            y -= 1

        print(f"seed {seed} at ({x},{z})")
        print(f"  island top      y={cap}")
        print(f"  island underside y={base}")
        print(f"  void beneath     {void} blocks (down to y={base-void})")
        print(f"\n  surrounding ground at {gap} blocks out:")
        lower = 0
        for dx, dz in DIRS:
            rx, rz = x + dx * gap, z + dz * gap
            rs = top_solid(srv, rx, rz, -60, 200) if srv.loaded(rx, rz) else None
            mark = ""
            if rs is None:
                mark = "  (unknown -- chunk not loaded)"
            elif rs < base:
                lower += 1
                mark = "  below the underside"
            print(f"    ({dx:+d},{dz:+d})  top y={rs}{mark}")

    try:
        JavaServer(seed=seed).cleanup()
    except Exception:
        pass

    print(f"\n  {lower}/8 directions have ground below the island's underside")
    if void >= 8 and lower >= 6:
        print("  => FLOATING: solid cap, a real void beneath, and the ground "
              "around it lies lower on almost every side.")
        return 0
    if void < 8:
        print("  => NOT floating: there is no meaningful void under the cap.")
    else:
        print("  => NOT floating: the void is real but the terrain continues "
              "beside it, so this is a cave or an overhang rather than an island.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
