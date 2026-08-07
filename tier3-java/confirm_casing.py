#!/usr/bin/env python3
"""Confirm one casing claim against a real Minecraft server.

The fast path (tier2-outpost/OreGen) decorates a single chunk and agrees with a
real server about the casing block on 88% of chests. That is good enough to
SHORTLIST and not good enough to report, so every hit it produces is a lead. This
settles one.

    python confirm_casing.py <seed> <x> <z> [--expect magma_block]

Prints the six faces the chest actually has, the block that was written into
them, and -- if --expect is given -- whether the claim survives. Exit code is 0
only when the claim is confirmed, so a caller can branch on it.

The fill cannot be read directly: the chest replaced that block. It is inferred
from the faces, and only faces that were air or water get overwritten, so a face
already holding solid rock keeps its own block. The DOWN face is the floor that
stopped the descent and is solid by definition, so it is excluded -- counting it
was what made an earlier verification report disagreements that were its own
fault.
"""
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                          # noqa: E402
from treasure_probe import CANDIDATES                       # noqa: E402
from hunt_treasure_ore import face_probe                    # noqa: E402
from treasure_casing import FACES                           # noqa: E402

NAMES = ["west", "east", "down", "up", "north", "south"]


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    seed, x, z = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    expect = ""
    for i, a in enumerate(sys.argv):
        if a == "--expect" and i + 1 < len(sys.argv):
            expect = sys.argv[i + 1]
    if expect and not expect.startswith("minecraft:"):
        expect = "minecraft:" + expect

    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        srv.forceload(x, z, radius_chunks=1)
        if not srv.loaded(x, z):
            print("chunk would not load -- UNKNOWN, which is not the same as "
                  "'no treasure here'")
            return 2
        ys = sorted(srv.blocks(x, z, 10, 110, "minecraft:chest"))
        if not ys:
            print(f"no chest in the column at ({x},{z}) -- the fast path "
                  f"predicted a treasure the real world does not have there")
            return 2
        if len(ys) > 1:
            print(f"note: {len(ys)} chests in this column ({ys}); a shipwreck or "
                  f"ocean ruin can share it. Reporting the lowest.")
        y = ys[0]
        faces = face_probe(srv, x, y, z, CANDIDATES)
    try:
        JavaServer(seed=seed).cleanup()
    except Exception:
        pass

    print(f"chest at ({x},{y},{z}) in seed {seed}")
    for o, nm in zip(FACES, NAMES):
        b = faces.get(o)
        print(f"  {nm:6s} {b or '(not on the candidate list)'}")

    sides = {o: b for o, b in faces.items() if o != (0, -1, 0)}
    modal, n = Counter("?" if b is None else b for b in sides.values()).most_common(1)[0]
    print(f"\ncasing: {modal}  ({n} of 5 side/top faces)")
    if n < 3:
        print("  -- too few faces agree to call this a casing at all: most of "
              "them were already solid, so there was nothing for the fill to "
              "write into.")
    print(f"  /tp {x} {y} {z}")

    if not expect:
        return 0
    if modal == expect and n >= 3:
        print(f"\nCONFIRMED: the casing really is {expect}.")
        return 0
    print(f"\nNOT CONFIRMED: expected {expect}, the server says {modal}. "
          f"This is the 12% the fast path gets wrong, which is exactly why it "
          f"shortlists rather than decides.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
