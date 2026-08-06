#!/usr/bin/env python3
"""Hunt for a buried treasure that CASED ITSELF in ore.

The treasure piece does not just sit in terrain. It takes the block at its
landing spot and writes that block into whichever of the chest's six face
neighbours were air or water. So a chest that lands on iron ore comes out walled
in iron ore. treasure_casing.py measures this directly: faces come out 75%
uniform against 59% for the diagonals, and chests turn up with four faces of
andesite against two of pre-existing gravel -- four overwritten, two that were
already solid.

That is a different target from "a chest near some ore", and it changes the
search in two ways:

  - Score the six FACES, not all 26 neighbours. The twenty diagonals are terrain
    the structure never touched, so counting them buries the signal in noise.
  - A perfect find is 6/6, but 4/6 is already fully cased -- the other two faces
    were solid rock, so there was nothing there to overwrite. Judging by the
    fraction of AIR-OR-WATER faces that came out ore would be the truest score;
    that state is gone by the time we can look, so this reports the raw count
    and leaves the reading to you.

    python hunt_treasure_ore.py [--target iron_ore|any] [--min 2]
                                [--seeds N] [--start S] [--radius R] [--json f]

Probing is two-stage: all six faces against the 20 ores first (120 commands), and
only chests that show ore get the full 85-block characterisation. The ore screen
is a quarter the cost of the old 26-neighbour probe, so a run reaches four times
the chests.

The run also tallies every casing block it sees, whether or not it is ore. That
tally is the part worth reading if the hunt comes up empty: it says what the
casing CAN be, which is the difference between "did not find one" and "there is
none to find".
"""
import json
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                              # noqa: E402
from treasure_probe import locate, shell_probe, ORES, CANDIDATES  # noqa: E402
from treasure_casing import FACES                               # noqa: E402

# What a casing is made of, measured over 87 chests: sand 64, gravel 13,
# sandstone 6, stone 2, andesite 1, diorite 1. Two groups -- the sediment the
# chest sat IN and the rock it sat ON -- which is the fill rule showing through.
# Screening against these plus the ores identifies almost every face in one pass;
# anything else falls through to the full list rather than being called unknown.
COMMON = [
    "minecraft:sand", "minecraft:gravel", "minecraft:sandstone",
    "minecraft:stone", "minecraft:andesite", "minecraft:diorite",
    "minecraft:granite", "minecraft:dirt", "minecraft:clay", "minecraft:water",
    "minecraft:air", "minecraft:deepslate", "minecraft:tuff",
    "minecraft:red_sand", "minecraft:grass_block", "minecraft:coarse_dirt",
]
SCREEN = ORES + COMMON
ORESET = set(ORES)


def face_probe(srv, x, y, z, candidates):
    """Identify only the six face neighbours -- the ones the piece overwrites."""
    cmds, out = [], {}
    for oi, (dx, dy, dz) in enumerate(FACES):
        for ci, b in enumerate(candidates):
            cmds.append(f"execute if block {x+dx} {y+dy} {z+dz} {b} run say F{oi}_{ci}")
    lines = srv.run(cmds)
    import re
    hits = {}
    for line in lines:
        for m in re.finditer(r"\bF(\d+)_(\d+)\b", line):
            hits.setdefault(FACES[int(m.group(1))], []).append(candidates[int(m.group(2))])
    for o in FACES:
        got = hits.get(o, [])
        out[o] = got[0] if got else None
    return out


def main():
    target, want, nseeds, start, radius = "any", 2, 60, 0, 3000
    version, jsonl = "1.21", None
    for i, a in enumerate(sys.argv):
        if a == "--target" and i + 1 < len(sys.argv):  target = sys.argv[i + 1]
        if a == "--min" and i + 1 < len(sys.argv):     want = int(sys.argv[i + 1])
        if a == "--seeds" and i + 1 < len(sys.argv):   nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):   start = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]
        if a == "--json" and i + 1 < len(sys.argv):    jsonl = sys.argv[i + 1]
    if target != "any" and not target.startswith("minecraft:"):
        target = "minecraft:" + target

    print(f"hunting buried treasure CASED in >= {want} face(s) of {target}, "
          f"seeds {start}..{start+nseeds-1}, radius {radius}", flush=True)
    t0 = time.time()
    best, best_at, chests, hits, failed = 0, None, 0, [], 0
    casings, ore_tally, rows = Counter(), Counter(), []

    for seed in range(start, start + nseeds):
        spots = locate(seed, version, radius)
        if not spots:
            continue
        try:
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                # Generation is the cost, not probing. Ask for every treasure's
                # chunks up front so the server works through them together.
                for (x, z) in spots:
                    # 1, not 2 and not 0. Treasure always sits at
                    # (chunkX*16+9, chunkZ*16+9) -- every chest coordinate
                    # measured is 9 mod 16 -- so its faces are at offsets 8..10
                    # and a single chunk would geometrically suffice. Forcing
                    # only that chunk was measured and is NOT faster: the server
                    # generates the neighbourhood anyway to finish a chunk, so
                    # the 5.3s does not shrink, it just reappears in the column
                    # scan (0.04s -> 1.79s per chest). 1 keeps the margin for
                    # nothing, so it keeps the margin.
                    srv.forceload(x, z, radius_chunks=1)
                for (x, z) in spots:
                    if not srv.loaded(x, z):
                        continue                    # unknown, never "absent"
                    ys = srv.blocks(x, z, 10, 110, "minecraft:chest")
                    for y in sorted(ys):
                        chests += 1
                        # One probe against the ores plus the blocks a casing is
                        # actually made of. The full 85-block list is 3x the
                        # commands and all but a handful of it never matches a
                        # face, so it is held back for the chests that need it.
                        full = face_probe(srv, x, y, z, SCREEN)
                        if any(b is None for b in full.values()):
                            # Something not on the short list. Fall back rather
                            # than record it as "?" -- an unidentified face is
                            # exactly the one that might be the interesting one.
                            full = face_probe(srv, x, y, z, CANDIDATES)
                        counts = Counter(b for b in full.values() if b in ORESET)
                        score = sum(counts.values()) if target == "any" \
                            else counts.get(target, 0)
                        modal = Counter("?" if b is None else b
                                        for b in full.values()).most_common(1)[0][0]
                        casings[modal] += 1
                        for b, c in counts.items():
                            ore_tally[b] += c

                        if score:
                            row = {"seed": seed, "x": x, "y": y, "z": z,
                                   "score": score, "ore": dict(counts),
                                   "casing": modal,
                                   "faces": [full[o] for o in FACES]}
                            rows.append(row)
                            # Written as it is found, not at exit. A hunt runs
                            # for hours and gets killed; buffering to the end
                            # means an interrupted run loses everything it
                            # learned, which is how the first one lost its data.
                            if jsonl:
                                with open(jsonl, "a", encoding="utf-8") as fh:
                                    fh.write(json.dumps(row) + "\n")
                                    fh.flush()
                        if score > best:
                            best, best_at = score, (seed, x, y, z, dict(counts),
                                                    [full[o] for o in FACES])
                            name = target.split(':')[1] if target != 'any' else 'ore'
                            print(f"  best so far: seed {seed} ({x},{y},{z}) "
                                  f"{score}/6 faces {name} {dict(counts)}", flush=True)
                        if score >= want:
                            hits.append((seed, x, y, z, dict(counts)))
            # A run of this size is otherwise silent for hours, since nothing
            # prints unless a chest scores. Progress per seed makes the rate
            # visible while there is still time to act on it.
            el = time.time() - t0
            print(f"  [seed {seed}] {chests} chests, {el/60:.0f} min, "
                  f"{el/max(chests,1):.1f}s/chest, best {best}/6", flush=True)
        except Exception as e:
            failed += 1
            print(f"  seed {seed}: server failed ({e})", flush=True)
        finally:
            try:
                JavaServer(seed=seed).cleanup()
            except Exception:
                pass

    el = time.time() - t0
    print(f"\n{chests} chests across {nseeds} seeds in {el/60:.1f} min "
          f"({el/max(chests,1):.1f}s per chest)")
    if casings:
        print("casing block (most common of the six faces), all chests:")
        for b, c in casings.most_common():
            print(f"  {c:4d}  {b}")
    if ore_tally:
        print("ore seen on a face:")
        for b, c in ore_tally.most_common():
            print(f"  {c:4d}  {b}")
    else:
        print("NO ore on any face in this run. With the casing tally above, that "
              "is the useful result: it says what the casing block can be.")
    if failed:
        print(f"WARNING: {failed} of {nseeds} seeds never ran (server failures). "
              f"This run did NOT cover what it was asked to.")
    if best_at:
        s, x, y, z, o, f = best_at
        print(f"\nBEST: seed {s} chest ({x},{y},{z}) -- {best}/6 faces, {o}")
        print(f"  faces: {[b.split(':')[1] if b else '?' for b in f]}")
        print(f"  /tp {x} {y} {z}")
    print(f"{len(hits)} chest(s) met the >= {want} bar")

    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
