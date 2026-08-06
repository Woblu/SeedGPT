#!/usr/bin/env python3
"""Does buried treasure ENCASE itself, and in what?

The claim under test, which changes what a hunt should even look for: the
treasure piece does not simply sit in whatever terrain is there. It takes the
block it found at its landing spot and writes that block into the chest's
neighbours -- so a chest that lands on iron ore ends up walled in iron ore,
rather than merely being near some.

If that is right then two things follow, and both are checkable:

  1. The overwrite hits the six FACE neighbours (one per direction) and not the
     twenty diagonals. So the faces should be far more uniform than the corners,
     and where they differ the corners should look like ordinary terrain.
  2. Only neighbours that were air or water get replaced -- a face already
     holding solid rock keeps it. So a chest buried in solid ground shows less
     casing than one sitting in an open water pocket, not more.

Prediction 1 is the one worth testing, because it is the one that distinguishes
"encased by the structure" from "happens to be next to". If faces and diagonals
are equally uniform, the casing story is wrong and the chest is just sitting in
homogeneous terrain.

    python treasure_casing.py <seed> [radius] [--version 1.21]

Reports, per chest, the six faces and the twenty diagonals side by side.
"""
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                      # noqa: E402
from treasure_probe import locate, shell_probe, CANDIDATES  # noqa: E402

FACES = [(-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1)]


def split(shell):
    """Face neighbours vs diagonal neighbours."""
    faces = {o: b for o, b in shell.items() if o in FACES}
    diags = {o: b for o, b in shell.items() if o not in FACES}
    return faces, diags


def uniformity(d):
    """Share of neighbours holding the single most common block.

    None (not on the candidate list) is counted as its own value rather than
    dropped -- an unidentified neighbour is evidence about uniformity too.
    """
    if not d:
        return 0.0, None
    c = Counter("?" if b is None else b for b in d.values())
    blk, n = c.most_common(1)[0]
    return n / len(d), blk


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = int(sys.argv[1])
    radius = 3000
    if len(sys.argv) > 2 and not sys.argv[2].startswith("-"):
        radius = int(sys.argv[2])
    version = "1.21"
    for i, a in enumerate(sys.argv):
        if a == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]

    spots = locate(seed, version, radius)
    print(f"seed {seed}: {len(spots)} buried treasure(s) within {radius}")
    if not spots:
        return 1

    face_u, diag_u, n = [], [], 0
    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        for (x, z) in spots:
            srv.forceload(x, z, radius_chunks=2)
        for (x, z) in spots:
            if not srv.loaded(x, z):
                print(f"  ({x},{z}) chunk would not load -- UNKNOWN")
                continue
            ys = srv.blocks(x, z, 10, 110, "minecraft:chest")
            if not ys:
                continue
            for y in sorted(ys):
                shell = shell_probe(srv, x, y, z, candidates=CANDIDATES)
                faces, diags = split(shell)
                fu, fb = uniformity(faces)
                du, db = uniformity(diags)
                face_u.append(fu)
                diag_u.append(du)
                n += 1
                fs = ", ".join(f"{v.split(':')[1] if v else '?'}" for v in faces.values())
                print(f"  chest ({x},{y},{z})")
                print(f"      faces  {fu*100:3.0f}% {fb.split(':')[1] if fb and fb!='?' else fb}"
                      f"   [{fs}]")
                print(f"      diags  {du*100:3.0f}% {db.split(':')[1] if db and db!='?' else db}")

    if not n:
        print("no chests reached -- nothing measured, and this is NOT evidence "
              "either way about the casing claim")
        return 1
    f = sum(face_u) / n
    d = sum(diag_u) / n
    print(f"\n{n} chests: faces {f*100:.0f}% uniform, diagonals {d*100:.0f}% uniform")
    if f > d + 0.15:
        print("=> faces are markedly more uniform than diagonals: consistent with "
              "the structure WRITING its own casing, not just sitting in terrain")
    elif abs(f - d) <= 0.15:
        print("=> faces and diagonals are about equally uniform: this does NOT "
              "support the casing claim; the chest may just be in uniform terrain")
    else:
        print("=> diagonals MORE uniform than faces -- unexpected under either "
              "story; worth looking at before trusting any hunt built on this")
    return 0


if __name__ == "__main__":
    sys.exit(main())
