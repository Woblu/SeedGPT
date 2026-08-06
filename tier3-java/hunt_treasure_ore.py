#!/usr/bin/env python3
"""Hunt for a buried treasure encased in ore.

A chest that landed inside an iron blob has ore on several of its 26 faces
instead of sand. That is a question about the finished world -- ore is placed
after terrain and before the structure decides where to sit -- so only the real
server can answer it.

    python hunt_treasure_ore.py [--target minecraft:iron_ore] [--min 2]
                                [--seeds N] [--start S] [--radius R] [--json f]

--target may be a specific ore, or "any" to rank by total ore contact.
Ranked output: the best chest found so far is printed as it is beaten, so a long
hunt is useful before it finishes.

Probing is ore-only (20 blocks x 26 neighbours) rather than the full 85-block
classification, which is a quarter of the commands. A hunt is bounded by how
many chests it can reach, not by how carefully it describes each one.
"""
import json
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer                          # noqa: E402
from treasure_probe import locate, shell_probe, ORES         # noqa: E402


def main():
    target, want, nseeds, start, radius = "minecraft:iron_ore", 2, 60, 0, 3000
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

    print(f"hunting buried treasure with >= {want} {target} touching it, "
          f"seeds {start}..{start+nseeds-1}, radius {radius}", flush=True)
    t0 = time.time()
    best, best_at, chests, hits, failed = 0, None, 0, [], 0
    tally, rows = Counter(), []

    for seed in range(start, start + nseeds):
        spots = locate(seed, version, radius)
        if not spots:
            continue
        try:
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                # Ask for every treasure's chunks up front. Generation is the
                # cost here, not the probing, and issuing the forceloads in one
                # batch lets the server work through them together instead of
                # stalling once per chest.
                for (x, z) in spots:
                    srv.forceload(x, z, radius_chunks=2)
                for (x, z) in spots:
                    if not srv.loaded(x, z):
                        continue                        # unknown, never "absent"
                    # Measured range is y 32..79; 10..110 keeps a wide margin
                    # while dropping two thirds of the column scan.
                    ys = srv.blocks(x, z, 10, 110, "minecraft:chest")
                    if not ys:
                        continue
                    for y in sorted(ys):
                        shell = shell_probe(srv, x, y, z, candidates=ORES)
                        counts = Counter(b for b in shell.values() if b)
                        chests += 1
                        for b, c in counts.items():
                            tally[b] += c
                        score = sum(counts.values()) if target == "any" \
                            else counts.get(target, 0)
                        if score:
                            rows.append({"seed": seed, "x": x, "y": y, "z": z,
                                         "score": score, "ore": dict(counts)})
                        if score > best:
                            best, best_at = score, (seed, x, y, z, dict(counts))
                            print(f"  best so far: seed {seed} chest ({x},{y},{z}) "
                                  f"{score}x {target.split(':')[1] if target!='any' else 'ore'} "
                                  f"{dict(counts)}", flush=True)
                        if score >= want:
                            hits.append((seed, x, y, z, dict(counts)))
        except Exception as e:
            failed += 1
            print(f"  seed {seed}: server failed ({e})", flush=True)
        finally:
            try:
                JavaServer(seed=seed).cleanup()
            except Exception:
                pass

    el = time.time() - t0
    print(f"\n{chests} chests probed across {nseeds} seeds in {el/60:.1f} min "
          f"({el/max(chests,1):.1f}s per chest)")
    if tally:
        print("ore seen touching a chest, all seeds:")
        for b, c in tally.most_common():
            print(f"  {c:4d}  {b}")
    if failed:
        print(f"WARNING: {failed} of {nseeds} seeds never ran (server failures). "
              f"This run did NOT cover what it was asked to.")
    if best_at:
        s, x, y, z, o = best_at
        print(f"\nBEST: seed {s} chest ({x},{y},{z}) -- {best} touching, {o}")
        print(f"  /tp {x} {y} {z}   (dig straight to it)")
    print(f"{len(hits)} chest(s) met the >= {want} bar")

    if jsonl:
        with open(jsonl, "a", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
