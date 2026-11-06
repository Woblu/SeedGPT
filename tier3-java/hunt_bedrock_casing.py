#!/usr/bin/env python3
"""Find buried treasure encased in BEDROCK (pre-1.18 worlds).

This is not a glitch and it is not a special case in the code. It falls out of
two ordinary rules meeting:

  1. The placement scan descends until the block BELOW is one of
     sandstone/stone/andesite/granite/diorite. Bedrock is NOT in that set, so a
     chest does not land ON bedrock -- it falls THROUGH the band until the block
     below is finally stone.
  2. The `fill` is the block AT the landing spot, and it is written into every
     face that was air or liquid. Land inside the bedrock band and the fill IS
     bedrock.

The UP face always takes the fill and never the floor block, so a chest at y=4
writes bedrock into y=5. Natural bedrock stops at y=4 -- a bedrock block at y=5
is a fingerprint of the casing rather than of terrain.

WHAT IS ACTUALLY RARE is the column, not the casing. The chest must fall from
the ocean floor into the bedrock band, which needs a ravine reaching y<=5 at
exactly the treasure's (9,9). Everything else follows. So that is what this
screens for.

    python hunt_bedrock_casing.py [--seeds N] [--start S] [--radius R]
                                  [--version 1.16]

Verified against the known find: seed 4184348446460745435, treasure at (41,73),
column carved open to y=4 with stone at y=3 -> chest at y=4.

NOT A COMPLETENESS CLAIM. The screen runs on carved noise terrain, without the
bedrock layer itself (setBedrock draws from the same random the surface builder
has already used, so reproducing it needs the surface step). A hit means the
column reaches the band; whether that particular chest is cased in bedrock or in
the hardcoded SAND depends on the bedrock pattern in its own column, which is
1-in-5 for y=4. Confirm on a server.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TIER2 = os.path.join(ROOT, "tier2")
LOCATE = os.path.join(ROOT, "build", "locate.exe")
sys.path.insert(0, os.path.join(ROOT, "tier2-outpost"))
from bench_fast import java_bin                     # noqa: E402

# The bedrock band is y0-4, and the chest needs the block below it to be stone,
# so the deepest useful landing is y=4 and the shallowest interesting one is a
# little above the band. Anything that only reaches y=6+ cannot touch bedrock.
BAND_TOP = 5


def cp2():
    raw = open(os.path.join(TIER2, "cp.txt"), encoding="utf-8").read()
    raw = raw.replace("\r", "").strip().rstrip(";")
    parts = [x for x in re.split(r"[;\n]", raw) if x]
    return os.path.join(TIER2, "out") + os.pathsep + os.pathsep.join(parts)


def locate(seed, version, radius):
    r = subprocess.run([LOCATE, str(seed), version, "buried_treasure", str(radius)],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 2:
            out.append((int(p[0]), int(p[1])))
    return out


class CarvePath:
    """Persistent 1.16 carved-column worker."""

    def __init__(self):
        self.p = subprocess.Popen(
            [java_bin(), "-Xmx3g", "-cp", cp2(), "VillageWorldgen", "carveserver"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, cwd=TIER2, bufsize=1)
        for line in self.p.stdout:
            if line.rstrip().endswith("READY"):
                return
        raise RuntimeError("carveserver never reported READY")

    def ask(self, seed, x, z):
        self.p.stdin.write(f"{seed} {x} {z}\n")
        self.p.stdin.flush()
        for line in self.p.stdout:
            # System.out is wrapped by Minecraft's logger, so every line arrives
            # with a "[..] [main/INFO]: [STDOUT]: " prefix. Stripping it here
            # rather than at the call sites.
            line = re.sub(r"^.*STDOUT\]: ", "", line.rstrip())
            if line.startswith("C\t"):
                t = line.split("\t")
                if any(f.startswith("err=") for f in t):
                    return None
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
    nseeds, start, radius, version = 40, 1, 2000, "1.16"
    for i, a in enumerate(sys.argv):
        if a == "--seeds" and i + 1 < len(sys.argv):   nseeds = int(sys.argv[i + 1])
        if a == "--start" and i + 1 < len(sys.argv):   start = int(sys.argv[i + 1])
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]

    print(f"hunting bedrock-cased buried treasure, {version}, seeds "
          f"{start}..{start+nseeds-1}, radius {radius}", flush=True)
    t0 = time.time()
    w = CarvePath()
    scanned = hits = 0
    deepest = []
    try:
        for seed in range(start, start + nseeds):
            for (x, z) in locate(seed, version, radius):
                d = w.ask(seed, x, z)
                if not d:
                    continue
                scanned += 1
                open_to = int(d.get("openTo", 999))
                bottom = d.get("bottom", "?")
                by = int(d.get("bottomY", -1))
                deepest.append((open_to, seed, x, z, bottom, by))
                if open_to <= BAND_TOP:
                    hits += 1
                    print(f"  BEDROCK BAND  seed {seed} ({x},{z})  column open to "
                          f"y={open_to}, first solid {bottom.split(':')[-1]} at "
                          f"y={by}  -> chest lands at y={by+1}", flush=True)
                    print(f"        confirm on a {version} server: "
                          f"/tp {x} {by+1} {z}", flush=True)
            if (seed - start + 1) % 10 == 0:
                el = time.time() - t0
                print(f"  {scanned} treasures, {el/60:.1f} min, {hits} reaching "
                      f"the band", flush=True)
    finally:
        w.close()

    el = time.time() - t0
    print(f"\n{scanned} treasures in {el/60:.1f} min ({el/max(scanned,1):.2f}s each)")
    deepest.sort()
    print("\ndeepest columns (how far the placement scan can fall):")
    for (o, seed, x, z, bottom, by) in deepest[:15]:
        print(f"  open to y={o:3d}   seed {seed:>12}  ({x},{z})  first solid "
              f"{bottom.split(':')[-1]} at y={by}")
    if not hits:
        print(f"\nno column reached the bedrock band in {scanned} treasures. That "
              f"is a statement about what was scanned, NOT evidence that none "
              f"exists -- it needs a ravine bottoming out at exactly (9,9).")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
