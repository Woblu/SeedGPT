#!/usr/bin/env python3
"""How many blacksmiths does a village actually have?

The 1.13 "blacksmith" became three separate buildings in 1.14, and each is
identified by the workstation the template places:

    armorer      blast_furnace
    toolsmith    smithing_table
    weaponsmith  grindstone

cubiomes cannot answer this. A 1.14+ village is assembled by the jigsaw
generator from templates chosen at generation time, and no seed-finding library
implements that assembler -- FeatureUtils and mc_feature both ship it unfinished.
tier2/ runs the real assembler under Fabric Loom, but only for 1.16.5 and it was
never wired to anything.

So this counts the workstations in the finished world instead, which is both
simpler and version-current: whatever the assembler decided, the blocks are
there to be counted. Counting is by `/fill ... replace`, the only cheap way --
probing a village-sized volume one position at a time is hundreds of thousands
of commands. That is DESTRUCTIVE, so each seed gets a fresh throwaway world and
is counted exactly once.

    python village_smiths.py <seed> [radius] [--version 1.21] [--min N]

Prints one line per village. Exit code 0 if any village met --min.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from java_oracle import JavaServer          # noqa: E402

LOCATE = os.path.join(ROOT, "build", "locate.exe")

SMITHS = {
    "armorer":     "minecraft:blast_furnace",
    "toolsmith":   "minecraft:smithing_table",
    "weaponsmith": "minecraft:grindstone",
}

# A village spreads about this far from its start piece. Generous on purpose:
# undercounting because an outlying house fell outside the box is exactly the
# kind of quiet wrongness this project tries not to ship.
HALF = 80
Y0, Y1 = -64, 180


def locate_villages(seed, version, radius):
    r = subprocess.run([LOCATE, str(seed), version, "village", str(radius)],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 2:
            out.append((int(p[0]), int(p[1])))
    return out


def count_smiths(srv, x, z):
    """Workstation counts around a village. Destroys them; call once per world.

    Returns (counts, bells). `bells` is the positive control: every village has
    a bell at its meeting point, so a village that reports zero bells did not
    report zero smiths either -- the box was wrong, or the chunks were not
    really there. Without it, "no smiths" and "no village loaded" are the same
    answer, which is the failure this whole backend is built to avoid.
    """
    srv.forceload(x, z, radius_chunks=(HALF // 16) + 2)
    if not srv.loaded(x, z):
        return None, None               # unknown, never zero
    bells = srv.count_block(x - HALF, Y0, z - HALF,
                            x + HALF, Y1, z + HALF, "minecraft:bell")
    got = {}
    for name, block in SMITHS.items():
        got[name] = srv.count_block(x - HALF, Y0, z - HALF,
                                    x + HALF, Y1, z + HALF, block)
    return got, bells


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = int(sys.argv[1])
    radius = 2000
    if len(sys.argv) > 2 and not sys.argv[2].startswith("-"):
        radius = int(sys.argv[2])
    version, want = "1.21", 0
    for i, a in enumerate(sys.argv):
        if a == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]
        if a == "--min" and i + 1 < len(sys.argv):
            want = int(sys.argv[i + 1])

    spots = locate_villages(seed, version, radius)
    print(f"seed {seed}: {len(spots)} village(s) within {radius} blocks")
    if not spots:
        return 1

    best = 0
    with JavaServer(seed=seed, fresh=True) as srv:
        for (x, z) in spots:
            got, bells = count_smiths(srv, x, z)
            if got is None:
                print(f"  village ({x},{z}) chunks would not load -- UNKNOWN")
                continue
            if bells == 0:
                print(f"  village ({x},{z}) NO BELL in the box -- the count is not "
                      f"trustworthy, reporting nothing rather than zero")
                continue
            total = sum(got.values())
            best = max(best, total)
            detail = ", ".join(f"{n} {k}" for k, n in got.items() if n)
            print(f"  village ({x:6d},{z:6d})  smiths={total}   (control: {bells} bell)"
                  + (f"   [{detail}]" if detail else ""))
    print(f"best village: {best} smiths")
    return 0 if best >= want else 1


if __name__ == "__main__":
    sys.exit(main())
