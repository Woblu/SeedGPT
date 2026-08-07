#!/usr/bin/env python3
"""Find a buried treasure cased in iron, by spending the server only where it can pay.

The server costs ~6.5s per chest, nearly all of it chunk generation. The headless
tier-2 path replicates BuriedTreasurePiece's placement scan in-process and costs
0.108s -- 60x less, measured, agreeing with the server on 18 of 19 seed-4 chests.
It cannot answer the question directly, because it runs terrain and surface rules
but not applyBiomeDecoration, and ore is placed in decoration. What it CAN do is
say how deep a chest lands, 60x faster than the thing that knows about ore.

Depth is the whole game for iron. In 1.18+ iron comes from three configurations:
a middle band over y -24..56 peaking at 16, an upper band from y 80, and a small
uniform band over y -63..72. Above y 56 only the small one contributes, and the
measured chests sit at y 32..79 concentrated in 48..63 -- which is to say almost
all of them land where iron is scarcest. That is why 1300 chests turned up coal
(which peaks near y 96) and never iron.

So: scan cheaply, and pay the server only for chests deep enough to be worth it.

    python hunt_iron_casing.py [--max-y 46] [--seeds N] [--start S] [--radius R]

THIS FILTER IS A HEURISTIC, NOT A SIEVE. The fast path disagreed with the server
on 1 of 19 chests, because decoration can add surface blocks that move the
landing spot. A chest whose true depth is below the cut but whose predicted depth
is above it will be dropped and never probed. That is an acceptable trade for a
hunt, where the goal is to find one good thing rather than to enumerate all of
them -- but it would NOT be acceptable for a completeness claim, and the counts
printed at the end are of chests probed, never of chests that exist.
"""
import json
import os
import subprocess
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FAST = os.path.join(ROOT, "tier2-outpost")
sys.path.insert(0, HERE)
sys.path.insert(0, FAST)
from java_oracle import JavaServer                              # noqa: E402
from treasure_probe import locate, CANDIDATES                   # noqa: E402
from hunt_treasure_ore import face_probe, SCREEN, ORESET        # noqa: E402
from treasure_casing import FACES                               # noqa: E402
from bench_fast import java_bin, classpath                      # noqa: E402


class OrePath:
    """Headless worker WITH decoration: (seed, x, z) -> (chest Y, fill block).

    OreGen supplies a WorldGenLevel by proxy so applyBiomeDecoration can run
    without a server, which puts ore in the chunk. Verified against the server
    over 49 chests: chestY agrees 96%, fill 78%. The fill number is why this
    SHORTLISTS rather than decides -- a chest whose fill it calls ore is sent to
    the server for confirmation, and the server has the final word.

    The residual risk is the other direction: a chest whose fill really is ore
    but which this calls sand is dropped and never confirmed. At 78% that is a
    real miss rate, so a null result from this pipeline is not evidence that no
    ore casing exists -- only that none was found among what it shortlisted.
    """

    def __init__(self):
        self.restarts = 0
        self._spawn()

    def _spawn(self):
        self.p = subprocess.Popen(
            [java_bin(), "-Xmx2g", "-cp", classpath(), "OreGen", "oreserver"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=FAST, bufsize=1)
        for line in self.p.stdout:
            if line.strip().endswith("READY"):
                return
        raise RuntimeError("OreGen never reported READY")

    def probe(self, seed, x, z):
        # A dead worker used to take the whole run with it via a broken pipe,
        # losing every chest scanned so far. It is restarted instead, and the
        # one chest that died is reported as unknown rather than as a result.
        try:
            self.p.stdin.write(f"{seed} {x} {z}\n")
            self.p.stdin.flush()
        except (OSError, ValueError):
            self.restarts += 1
            try:
                self.p.terminate()
            except Exception:
                pass
            self._spawn()
            return None, None
        for line in self.p.stdout:
            if line.startswith("O\t"):
                t = line.rstrip("\n").split("\t")
                if any(f.startswith("err=") for f in t):
                    return None, None
                kv = dict(f.split("=", 1) for f in t if "=" in f)
                return int(kv.get("chestY", -1)), kv.get("fill")
        return None, None

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        self.p.terminate()


class FastPath:
    """Persistent headless worker: (seed, x, z) -> predicted chest Y."""

    def __init__(self):
        self.p = subprocess.Popen(
            [java_bin(), "-cp", classpath(), "OutpostWorldgen", "treasure"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=FAST, bufsize=1)
        for line in self.p.stdout:
            if line.strip().endswith("READY"):
                return
        raise RuntimeError("fast path never reported READY")

    def chest_y(self, seed, x, z):
        self.p.stdin.write(f"{seed} {x} {z}\n")
        self.p.stdin.flush()
        for line in self.p.stdout:
            if "chestY=" in line:
                kv = dict(t.split("=", 1) for t in line.strip().split("\t")
                          if "=" in t)
                return int(kv.get("chestY", -1))
        return -1

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        self.p.terminate()


def main():
    max_y, nseeds, start, radius = 46, 200, 5000, 4000
    version, jsonl = "1.21", "iron_casing.jsonl"
    for i, a in enumerate(sys.argv):
        if a == "--max-y" and i + 1 < len(sys.argv):   max_y = int(sys.argv[i + 1])
        if a == "--seeds" and i + 1 < len(sys.argv):   nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):   start = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--json" and i + 1 < len(sys.argv):    jsonl = sys.argv[i + 1]

    print(f"scanning seeds {start}..{start+nseeds-1} for treasures landing at "
          f"y <= {max_y}, then probing only those", flush=True)
    t0 = time.time()
    fast = OrePath()
    scanned, deep, probed, best, best_at = 0, [], 0, 0, None
    casings, ore_tally = Counter(), Counter()
    fills = Counter()

    try:
        for seed in range(start, start + nseeds):
            for (x, z) in locate(seed, version, radius):
                scanned += 1
                y, fill = fast.probe(seed, x, z)
                if y is None:
                    continue
                fills[fill] += 1
                # Two ways onto the shortlist. An ore fill is the direct hit --
                # this path can see ore now, which is the whole point of adding
                # decoration. Depth stays as a second route because the fill
                # agrees with the server only 78% of the time, so trusting it
                # alone would drop real candidates it merely misread as sand.
                if (fill and fill.endswith("_ore")) or (0 < y <= max_y):
                    deep.append((seed, x, z, y))
            if (seed - start + 1) % 20 == 0:
                el = time.time() - t0
                print(f"  scanned {scanned} treasures, {len(deep)} deep enough "
                      f"({100*len(deep)/max(scanned,1):.1f}%), {el/60:.1f} min",
                      flush=True)
    finally:
        fast.close()

    el = time.time() - t0
    print(f"\nfast scan: {scanned} treasures in {el/60:.1f} min "
          f"({el/max(scanned,1):.3f}s each) -> {len(deep)} to probe\n", flush=True)
    if not deep:
        print(f"nothing landed at y <= {max_y}. Raise --max-y or widen the scan; "
              f"this is a statement about what was scanned, not about what exists.")
        return 1

    # Group by seed so each world is booted once.
    by_seed = {}
    for (seed, x, z, y) in deep:
        by_seed.setdefault(seed, []).append((x, z, y))

    for seed, spots in sorted(by_seed.items()):
        try:
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                for (x, z, _) in spots:
                    srv.forceload(x, z, radius_chunks=1)
                for (x, z, py) in spots:
                    if not srv.loaded(x, z):
                        continue
                    ys = srv.blocks(x, z, 10, 110, "minecraft:chest")
                    for y in sorted(ys):
                        probed += 1
                        full = face_probe(srv, x, y, z, SCREEN)
                        if any(b is None for b in full.values()):
                            full = face_probe(srv, x, y, z, CANDIDATES)
                        counts = Counter(b for b in full.values() if b in ORESET)
                        modal = Counter("?" if b is None else b
                                        for b in full.values()).most_common(1)[0][0]
                        casings[modal] += 1
                        for b, c in counts.items():
                            ore_tally[b] += c
                        score = counts.get("minecraft:iron_ore", 0)
                        tot = sum(counts.values())
                        row = {"seed": seed, "x": x, "y": y, "z": z,
                               "predicted_y": py, "iron": score, "ore": dict(counts),
                               "casing": modal,
                               "faces": [full[o] for o in FACES]}
                        if tot:
                            with open(jsonl, "a", encoding="utf-8") as fh:
                                fh.write(json.dumps(row) + "\n")
                                fh.flush()
                        if score > best:
                            best, best_at = score, row
                            print(f"  IRON {score}/6  seed {seed} ({x},{y},{z})  "
                                  f"{[b.split(':')[1] if b else '?' for b in row['faces']]}",
                                  flush=True)
        except Exception as e:
            print(f"  seed {seed}: server failed ({e})", flush=True)
        finally:
            try:
                JavaServer(seed=seed).cleanup()
            except Exception:
                pass

    el = time.time() - t0
    print(f"\n{probed} chests probed of {scanned} scanned, {el/60:.1f} min total")
    if casings:
        print("casing block:")
        for b, c in casings.most_common():
            print(f"  {c:4d}  {b}")
    if ore_tally:
        print("ore on a face:")
        for b, c in ore_tally.most_common():
            print(f"  {c:4d}  {b}")
    if best_at:
        r = best_at
        print(f"\nBEST IRON: seed {r['seed']} ({r['x']},{r['y']},{r['z']}) "
              f"{best}/6 faces")
        print(f"  /tp {r['x']} {r['y']} {r['z']}")
    else:
        print("\nno iron on any face among the chests probed")
    return 0 if best else 1


if __name__ == "__main__":
    sys.exit(main())
