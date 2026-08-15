#!/usr/bin/env python3
"""Is the "floating island" the search found actually floating?

The condition is computed from cubiomes' density function, and the definition had
to be tightened twice: "air at the cap's height" called ~24% of outposts islands
and "air a few blocks lower" still called 18%, because on a hillside the ground
genuinely is absent at those heights. Only the real game settles whether the
current definition means anything.

    python verify_floating.py <seed> <x> <z> [--gap 24]

Reads the real column at (x,z) and at eight points `gap` blocks out. An island
shows: a solid cap, air beneath it, and every surrounding column topping out
BELOW the cap's underside.

WHY THIS NO LONGER TALKS TO A SERVER
------------------------------------
Three attempts asked a running server through chat commands, and this file used
to refuse to answer at all because two of them were wrong in ways that would have
certified islands that are not there:

  1. Full sweep. Reading every height across nine columns in ONE batch lost
     results -- surrounding surfaces came back y=1 and y=14 where the terrain is
     60-80. The sweep was right; the batching was not.
  2. Binary search. Cheaper, and STRUCTURALLY invalid here. Bisection needs a
     column that is air above and solid below. A floating island is exactly the
     shape that breaks it -- solid, air, solid -- so the search settles on an
     arbitrary boundary. Measured: island top y=45, surrounding ground y=9-35,
     against real terrain at 62-81.

None of it was necessary. tier2-outpost runs Minecraft's own
NoiseBasedChunkGenerator, so the column is an array in memory: no commands, no
batch limit, no bisection, and no 45-second server boot. `OreGen floatserver`
reads it directly, which is both exact and faster than either attempt.

The refusal is gone because the thing it was protecting against is gone. It is
still a refusal, not a guess, whenever the backend cannot answer -- an unreadable
column prints UNKNOWN and exits 2.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FAST = os.path.join(ROOT, "tier2-outpost")
sys.path.insert(0, FAST)
from bench_fast import java_bin, classpath      # noqa: E402

DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]

# An island has to clear both bars. The void is what makes it an island rather
# than a hill; the ring is what makes it an island rather than a cave roof or an
# overhang, which is the distinction that made the first two definitions call a
# fifth of all outposts floating.
MIN_VOID = 8
MIN_LOWER = 6


class FloatPath:
    """Persistent floatserver worker: (seed, x, z, gap) -> the column's shape."""

    def __init__(self):
        self.p = subprocess.Popen(
            [java_bin(), "-Xmx2g", "-cp", classpath(), "OreGen", "floatserver"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=FAST, bufsize=1)
        for line in self.p.stdout:
            if line.strip().endswith("READY"):
                return
        raise RuntimeError("floatserver never reported READY")

    def ask(self, seed, x, z, gap):
        self.p.stdin.write(f"{seed} {x} {z} {gap}\n")
        self.p.stdin.flush()
        for line in self.p.stdout:
            if line.startswith("F\t"):
                t = line.rstrip("\n").split("\t")
                return dict(f.split("=", 1) for f in t if "=" in f)
        return None

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        self.p.terminate()


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    seed, x, z = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    gap = 24
    for i, a in enumerate(sys.argv):
        if a == "--gap" and i + 1 < len(sys.argv):
            gap = int(sys.argv[i + 1])

    w = FloatPath()
    try:
        kv = w.ask(seed, x, z, gap)
    finally:
        w.close()

    # Three distinct ways to have no answer, kept distinct. Collapsing them into
    # "not floating" is the failure this whole file is a monument to.
    if kv is None:
        print("floatserver returned nothing -- UNKNOWN, not a verdict")
        return 2
    if "err" in kv:
        print(f"floatserver failed: {kv['err']} -- UNKNOWN, not a verdict")
        return 2
    if "empty" in kv:
        print(f"seed {seed} at ({x},{z}): no solid block anywhere in the column."
              f"\n  => NOT floating, and nothing is there at all.")
        return 1

    top, cap = int(kv["top"]), int(kv["cap"])
    void, lower = int(kv["void"]), int(kv["lower"])
    ring = [int(v) for v in kv["ring"].split(",")]

    print(f"seed {seed} at ({x},{z})")
    print(f"  island top       y={top}")
    print(f"  island underside y={cap}")
    print(f"  void beneath     {void} blocks (down to y={cap - void})")
    print(f"\n  surrounding ground at {gap} blocks out:")
    for (dx, dz), rt in zip(DIRS, ring):
        if rt == -999:
            mark = "  (no solid block in that column)"
        elif rt < cap:
            mark = "  below the underside"
        else:
            mark = ""
        print(f"    ({dx:+d},{dz:+d})  top y={rt}{mark}")

    print(f"\n  {lower}/8 directions have ground below the island's underside")
    if void >= MIN_VOID and lower >= MIN_LOWER:
        print("  => FLOATING: solid cap, a real void beneath, and the ground "
              "around it lies lower on almost every side.")
        return 0
    if void < MIN_VOID:
        print(f"  => NOT floating: the void under the cap is {void} blocks, "
              f"under the {MIN_VOID} this requires.")
    else:
        print(f"  => NOT floating: the void is real but only {lower}/8 sides "
              f"drop away, so this is a cave roof or an overhang rather than "
              f"an island.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
