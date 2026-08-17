#!/usr/bin/env python3
"""Find a village with cave spider spawners next to it.

Cave spider spawners live in mineshaft SPIDER CORRIDORS. This project had
spawners recorded as parked -- dungeons are decoration-time features needing a
ServerLevel -- but that reasoning does not cover these: mineshaft corridors are
STRUCTURE PIECES, produced by the structure generator, so the real 1.21.1
generator can enumerate them offline.

    python hunt_spider_village.py [--seeds N] [--start S] [--min 2]
                                  [--radius 3] [--version 1.21]

--radius is in CHUNKS, Chebyshev, from the village origin -- the way structure
grids are actually measured.

THREE THINGS ARE CHECKED, and the third is the one most likely to be skipped:

  1. a village exists near the search centre
  2. a mineshaft near it has corridors flagged spiderCorridor -- the flag the
     game itself uses to decide which corridors get cobwebs and a spawner
  3. those corridors ACTUALLY GENERATE. A mineshaft piece whose bounding box
     touches liquid is skipped outright by the game, and on the seed this was
     built against that is 55 of 94 pieces. Counting boxes without this is wrong
     about more than half of them.

NOT checked: whether the spawner is reachable or surface-exposed. mineshaft_mesa
generates at y 60-66 and ordinary mineshaft at y -33..-27, so a badlands village
is where these are worth looking at -- but "exposed" is a terrain question this
does not answer, and a hit is a spawner that exists, not one you can walk to.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FAST = os.path.join(ROOT, "tier2-outpost")
LOCATE = os.path.join(ROOT, "build", "locate.exe")
sys.path.insert(0, FAST)
from bench_fast import java_bin, classpath          # noqa: E402


def locate(seed, version, structure, radius):
    r = subprocess.run([LOCATE, str(seed), version, structure, str(radius)],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 2:
            try:
                out.append((int(p[0]), int(p[1])))
            except ValueError:
                pass
    return out


class PieceServer:
    """Persistent piece enumerator.

    The first version of this hunt shelled out to `OutpostWorldgen pieces` and
    paid a full JVM bootstrap per village per mineshaft variant. It spent all
    its time in startup and was abandoned mid-run. One worker, many queries.
    """

    def __init__(self):
        self.p = subprocess.Popen(
            [java_bin(), "-Xmx3g", "-cp", classpath(), "OutpostWorldgen",
             "pieceserver"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=FAST, bufsize=1)
        for line in self.p.stdout:
            if line.rstrip().endswith("READY"):
                return
        raise RuntimeError("pieceserver never reported READY")

    def pieces(self, seed, structure, x, z, check_liquid=True):
        """Piece dicts for `structure` near (x,z), or None if none generated.

        None and [] are kept apart: None means the generator found no structure
        there, [] would mean one with no pieces. Only the first is a reason to
        skip rather than to report zero.
        """
        self.p.stdin.write(f"{seed} {structure} {x} {z}"
                           f"{' liquid' if check_liquid else ''}\n")
        self.p.stdin.flush()
        out, hit, err = [], False, False
        for line in self.p.stdout:
            line = re.sub(r"^.*STDOUT\]: ", "", line.rstrip())
            if line.startswith("END"):
                break
            if line.startswith("ERROR") or line.startswith("NOHIT"):
                err = True
            elif line.startswith("START"):
                hit = True
            elif line.startswith("P\t"):
                t = line.split("\t")
                d = {"box": tuple(int(v) for v in t[1:7]), "name": t[7]}
                for f in t[8:]:
                    if "=" in f:
                        k, v = f.split("=", 1)
                        d[k] = v
                out.append(d)
        if err or not hit:
            return None
        return out

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        self.p.terminate()


def cheb_chunks(box, vx, vz):
    """Chebyshev distance in CHUNKS from a village at (vx,vz) to a box centre."""
    cx = (box[0] + box[3]) // 2
    cz = (box[2] + box[5]) // 2
    return max(abs(cx - vx) >> 4, abs(cz - vz) >> 4)


def main():
    nseeds, start, want, radius, version = 30, 1, 2, 3, "1.21"
    for i, a in enumerate(sys.argv):
        if a == "--seeds" and i + 1 < len(sys.argv):   nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):   start = int(sys.argv[i + 1])
        if a == "--min" and i + 1 < len(sys.argv):     want = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]

    print(f"hunting villages with >= {want} cave spider spawner(s) within "
          f"{radius} chunks (chebyshev), seeds {start}..{start+nseeds-1}",
          flush=True)
    t0 = time.time()
    checked = hits = 0
    srv = PieceServer()
    for seed in range(start, start + nseeds):
        for (vx, vz) in locate(seed, version, "village", 2000):
            # Both mineshaft variants: the mesa one sits at y 60-66 and is the
            # interesting case, the ordinary one at y -33..-27.
            found = []
            for ms in ("mineshaft_mesa", "mineshaft"):
                ps = srv.pieces(seed, ms, vx, vz)
                if not ps:
                    continue
                for p in ps:
                    if p.get("spider") != "1":
                        continue
                    if p.get("liquid") == "1":
                        continue          # the game skips this piece entirely
                    if p.get("liquid") == "?":
                        continue          # UNKNOWN, never counted as a hit
                    if cheb_chunks(p["box"], vx, vz) <= radius:
                        found.append((ms, p["box"]))
            checked += 1
            if len(found) >= want:
                hits += 1
                print(f"  SPIDER VILLAGE  seed {seed}  village ({vx},{vz})  "
                      f"{len(found)} spawner corridor(s)", flush=True)
                for (ms, b) in found:
                    print(f"      {ms:15} box {b[0]},{b[1]},{b[2]} .. "
                          f"{b[3]},{b[4]},{b[5]}", flush=True)
                print(f"      /tp {vx} 90 {vz}", flush=True)
    srv.close()
    el = time.time() - t0
    print(f"\n{checked} villages checked in {el/60:.1f} min, {hits} matched")
    if not hits:
        print("no match in what was scanned. That is a statement about the "
              "villages checked, NOT evidence that none exists.")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
