#!/usr/bin/env python3
"""Does Bedrock's biome map match Java's for the same seed?

Everything about the Bedrock plan hinges on this. The published claim is that
since 1.18 both editions share terrain and biome generation, and if that holds
for the seeds we care about, then the existing cubiomes engine ALREADY answers
biome and terrain questions for Bedrock and the only new work is structure
placement. If it does not hold, Bedrock needs its own generator and the project
is an order of magnitude larger. Worth an hour to find out rather than assume.

Method. BDS has no "what biome is at (x,z)" command, but it has
`locate biome`, and that is enough to turn into a point query:

    java:    checkbiome  -> the biome our engine predicts at (x, z)
    bedrock: execute positioned x 64 z run locate biome minecraft:<that biome>

If the two agree, Bedrock's answer is (x, z) itself -- distance 0. If it points
somewhere far away, that biome is not there and the engines disagree. Asking
"is OUR answer true in your world" avoids needing Bedrock to enumerate anything.

Naming is a real trap: Bedrock kept the legacy biome ids Java renamed
(snowy_taiga is cold_taiga, badlands is mesa, windswept_hills is extreme_hills).
Only biomes whose mapping is certain are sampled; anything else is skipped and
counted, so a small sample size cannot masquerade as agreement.

    python parity_biomes.py --seeds 1,2,12345 --version 1.21
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bds_oracle import Bds, RE_FOUND, EXE          # noqa: E402

ROOT = Path(__file__).parent.parent
CHECKBIOME = ROOT / "build" / "checkbiome.exe"

# Java (cubiomes) name -> Bedrock id. Only entries that are certain; the rest
# are deliberately absent so an unmapped biome is skipped, not guessed.
JAVA_TO_BEDROCK = {
    "plains": "plains", "desert": "desert", "forest": "forest",
    "birch_forest": "birch_forest", "dark_forest": "roofed_forest",
    "jungle": "jungle", "sparse_jungle": "jungle_edge", "taiga": "taiga",
    "snowy_taiga": "cold_taiga", "savanna": "savanna", "swamp": "swampland",
    "badlands": "mesa", "ocean": "ocean", "deep_ocean": "deep_ocean",
    "cold_ocean": "cold_ocean", "lukewarm_ocean": "lukewarm_ocean",
    "warm_ocean": "warm_ocean", "frozen_ocean": "frozen_ocean",
    "river": "river", "beach": "beach", "mushroom_fields": "mushroom_island",
    "ice_spikes": "ice_plains_spikes", "snowy_plains": "ice_plains",
    "windswept_hills": "extreme_hills", "flower_forest": "flower_forest",
    "bamboo_jungle": "bamboo_jungle", "stony_shore": "stone_beach",
}


def java_biome(seed, version, x, z):
    p = subprocess.run([str(CHECKBIOME), str(seed), version, str(x), str(z)],
                       capture_output=True, text=True)
    m = re.search(r"biome=(\S+)", p.stdout)
    return m.group(1) if m else None


def bedrock_has_biome_at(bds, biome_id, x, z):
    """Distance from (x,z) to the nearest `biome_id` in Bedrock, or None."""
    lines = bds.command(f"execute positioned {x} 64 {z} run locate biome minecraft:{biome_id}")
    for line in lines:
        m = RE_FOUND.search(line)
        if m:
            bx, bz = int(m.group("x")), int(m.group("z"))
            return abs(bx - x) + abs(bz - z), (bx, bz)
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", default="1,12345,777")
    ap.add_argument("--version", default="1.21", help="Java version for the engine side")
    ap.add_argument("--step", type=int, default=512, help="sample grid spacing")
    ap.add_argument("--grid", type=int, default=1, help="samples per axis from -N..N")
    ap.add_argument("--tolerance", type=int, default=64,
                    help="Manhattan blocks of slack before calling it a disagreement")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock server missing at {EXE}")
    if not CHECKBIOME.exists():
        sys.exit("build/checkbiome.exe missing -- ./build.sh tools/checkbiome.c")

    pts = [(dx * a.step, dz * a.step)
           for dx in range(-a.grid, a.grid + 1)
           for dz in range(-a.grid, a.grid + 1)]
    agree = disagree = skipped = missing = 0
    detail = []

    for seed in [int(s) for s in a.seeds.split(",") if s.strip()]:
        with Bds(seed, verbose=a.verbose) as bds:
            for (x, z) in pts:
                jb = java_biome(seed, a.version, x, z)
                if jb is None:
                    missing += 1
                    continue
                bid = JAVA_TO_BEDROCK.get(jb)
                if bid is None:
                    skipped += 1
                    detail.append((seed, x, z, jb, "unmapped", None))
                    continue
                dist, where = bedrock_has_biome_at(bds, bid, x, z)
                if dist is None:
                    disagree += 1
                    detail.append((seed, x, z, jb, "absent-in-bedrock", None))
                elif dist <= a.tolerance:
                    agree += 1
                    detail.append((seed, x, z, jb, "agree", dist))
                else:
                    disagree += 1
                    detail.append((seed, x, z, jb, "elsewhere", dist))

    print(f"\n{'seed':>12} {'x':>7} {'z':>7}  {'java biome':<18} {'verdict':<18} dist")
    for row in detail:
        s, x, z, jb, verdict, d = row
        print(f"{s:>12} {x:>7} {z:>7}  {jb:<18} {verdict:<18} {'' if d is None else d}")
    total = agree + disagree
    pct = (100.0 * agree / total) if total else 0.0
    print(f"\nagree {agree} / {total} ({pct:.0f}%)   skipped(unmapped) {skipped}   no-java-answer {missing}")
    print("A high agreement rate means the Java engine already answers Bedrock biome\n"
          "questions and only structure placement is new. A low one means Bedrock\n"
          "needs its own generator.")


if __name__ == "__main__":
    main()
