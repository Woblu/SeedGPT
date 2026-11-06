#!/usr/bin/env python3
"""How many ruined portals does cubiomes report that the game does not place?

`locate_oracle.py` compares NEAREST to nearest, which is not enough here: ruined
portals come in seven variants in 1.18+ (standard, desert, jungle, swamp,
mountain, ocean, nether), each a separate structure id sharing one placement
config. So `/locate minecraft:ruined_portal` finding something else says nothing
on its own -- the site might hold a different variant and be perfectly real.

This asks the right question instead. For every position cubiomes reports, it
runs /locate for EVERY variant FROM THAT POSITION. If a real portal of any
variant sits within `--tol` blocks, the position is confirmed. If none does, the
game places nothing there and cubiomes is hallucinating a structure.

    python audit_ruined_portals.py <seed> [--radius 2000] [--tol 8] [--max 20]

A confirmed position is not proof the variant label is right, only that a portal
exists there. Mislabelling matters less than inventing.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from java_oracle import JavaServer          # noqa: E402

LOCATE = os.path.join(ROOT, "build", "locate.exe")

VARIANTS = ["ruined_portal", "ruined_portal_desert", "ruined_portal_jungle",
            "ruined_portal_swamp", "ruined_portal_mountain",
            "ruined_portal_ocean", "ruined_portal_nether"]

POS_RE = re.compile(r"\[(-?\d+), (?:~|-?\d+), (-?\d+)\]")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = sys.argv[1]
    radius, tol, cap, version = 2000, 8, 20, "1.21"
    for i, a in enumerate(sys.argv):
        if a == "--radius" and i + 1 < len(sys.argv): radius = int(sys.argv[i + 1])
        if a == "--tol" and i + 1 < len(sys.argv):    tol = int(sys.argv[i + 1])
        if a == "--max" and i + 1 < len(sys.argv):    cap = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]

    r = subprocess.run([LOCATE, seed, version, "ruined_portal", str(radius)],
                       capture_output=True, text=True)
    spots = []
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) == 2:
            try:
                spots.append((int(p[0]), int(p[1])))
            except ValueError:
                pass
    spots.sort(key=lambda s: s[0] * s[0] + s[1] * s[1])
    spots = spots[:cap]
    print(f"seed {seed}: checking {len(spots)} cubiomes ruined portals "
          f"against every variant the game knows\n", flush=True)

    real = phantom = 0
    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        for (x, z) in spots:
            best, bestv, bestd = None, None, 10 ** 9
            for v in VARIANTS:
                out = srv.run(f"execute positioned {x} 64 {z} run "
                              f"locate structure minecraft:{v}")
                m = POS_RE.search(" ".join(out))
                if not m:
                    continue
                gx, gz = int(m.group(1)), int(m.group(2))
                d = max(abs(gx - x), abs(gz - z))
                if d < bestd:
                    best, bestv, bestd = (gx, gz), v, d
            if bestd <= tol:
                real += 1
                print(f"  ok      ({x},{z})  {bestv} at {best}, {bestd} off")
            else:
                phantom += 1
                near = f"{bestv} at {best}, {bestd} away" if best else "nothing found"
                print(f"  PHANTOM ({x},{z})  nearest real: {near}")
    tot = real + phantom
    print(f"\n{real}/{tot} confirmed, {phantom}/{tot} PHANTOM "
          f"({100.0 * phantom / max(tot, 1):.0f}%)")
    return 1 if phantom else 0


if __name__ == "__main__":
    sys.exit(main())
