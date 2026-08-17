#!/usr/bin/env python3
"""Does the real game place the structures cubiomes says it does?

Everything this project reports comes from cubiomes -- a reimplementation.
locate.exe, describe.exe and the search agree with each other because they are
the SAME engine, and three parts of one reimplementation agreeing is one opinion.
This asks Minecraft.

    python audit_structures.py <seed> [--structures village,monument]
                               [--radius 2000] [--tol 8] [--max 8]

For every position cubiomes reports, it runs /locate FROM THAT POSITION for each
id the structure can be, and confirms a real one sits within --tol blocks.

WHY "for each id": several structures are FAMILIES sharing one placement config.
Ruined portals have seven ids in 1.18+; villages five; a /locate on one of them
finding something else proves nothing on its own. Auditing ruined portals
against the standard id alone made 34 real portals look like phantoms, and I
reported one as a phantom before noticing.

A confirmed position means a structure of that family is really there. It does
NOT check the variant label, the Y, or whether you can see it standing on the
coordinates -- those are separate failures and the second one is what made
ruined portals look invented.

KNOWN OPEN: trail_ruins really does produce phantoms, about one in six, in every
seed tried. Reproducible case -- seed 20094, cubiomes reports one at (608,-864)
and the game's nearest is 688 blocks away at (848,-176). It is NOT the biome
list: cubiomes allows old_growth_birch_forest, and the GAME agrees that is the
biome at both the phantom and the real one. So the placement itself diverges,
and until that is understood a trail_ruins coordinate should not be trusted.
Everything else audited clean: 31 of 32 across 19 structures.
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

# cubiomes name -> every minecraft: id the game may place at such a site.
FAMILY = {
    "ruined_portal": ["ruined_portal", "ruined_portal_desert", "ruined_portal_jungle",
                      "ruined_portal_swamp", "ruined_portal_mountain",
                      "ruined_portal_ocean"],
    "village": ["village_plains", "village_desert", "village_savanna",
                "village_snowy", "village_taiga"],
    "ocean_ruin": ["ocean_ruin_cold", "ocean_ruin_warm"],
    "shipwreck": ["shipwreck", "shipwreck_beached"],
    "igloo": ["igloo"],
    "outpost": ["pillager_outpost"],
    "swamp_hut": ["swamp_hut"],
    "desert_pyramid": ["desert_pyramid"],
    "jungle_temple": ["jungle_pyramid"],
    "monument": ["monument"],
    "mansion": ["mansion"],
    "ancient_city": ["ancient_city"],
    "trial_chambers": ["trial_chambers"],
    "trail_ruins": ["trail_ruins"],
    "buried_treasure": ["buried_treasure"],
    "mineshaft": ["mineshaft", "mineshaft_mesa"],
    "fortress": ["fortress"],
    "bastion": ["bastion_remnant"],
    "end_city": ["end_city"],
}

# Structures whose coordinates are NOT overworld. /locate has to be run in that
# dimension, which this does not do -- reporting them as unaudited beats
# reporting them as phantoms.
OTHER_DIM = {"fortress", "bastion", "end_city", "ruined_portal_nether"}

POS_RE = re.compile(r"\[(-?\d+), (?:~|-?\d+), (-?\d+)\]")


def cubiomes(seed, version, st, radius):
    r = subprocess.run([LOCATE, str(seed), version, st, str(radius)],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        if line.startswith("ERROR") or line.startswith("#"):
            continue
        p = line.split()
        if len(p) == 2:
            try:
                out.append((int(p[0]), int(p[1])))
            except ValueError:
                pass
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = sys.argv[1]
    radius, tol, cap, version = 2000, 8, 8, "1.21"
    only = None
    for i, a in enumerate(sys.argv):
        if a == "--radius" and i + 1 < len(sys.argv): radius = int(sys.argv[i + 1])
        if a == "--tol" and i + 1 < len(sys.argv):    tol = int(sys.argv[i + 1])
        if a == "--max" and i + 1 < len(sys.argv):    cap = int(sys.argv[i + 1])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]
        if a == "--structures" and i + 1 < len(sys.argv):
            only = [x.strip() for x in sys.argv[i + 1].split(",")]

    todo = [s for s in (only or sorted(FAMILY)) if s in FAMILY]
    unknown = [s for s in (only or []) if s not in FAMILY]
    print(f"seed {seed}, {version}, radius {radius}, up to {cap} per structure\n",
          flush=True)

    grand_ok = grand_bad = 0
    skipped = []
    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        for st in todo:
            if st in OTHER_DIM:
                skipped.append(f"{st} (not an overworld coordinate)")
                continue
            spots = cubiomes(seed, version, st, radius)
            if not spots:
                print(f"  {st:16} cubiomes reports none in range")
                continue
            spots.sort(key=lambda s: s[0] * s[0] + s[1] * s[1])
            spots = spots[:cap]
            ok = bad = 0
            worst = None
            for (x, z) in spots:
                bestd, bestv, best = 10 ** 9, None, None
                for v in FAMILY[st]:
                    out = srv.run(f"execute positioned {x} 64 {z} run "
                                  f"locate structure minecraft:{v}")
                    m = POS_RE.search(" ".join(out))
                    if not m:
                        continue
                    gx, gz = int(m.group(1)), int(m.group(2))
                    d = max(abs(gx - x), abs(gz - z))
                    if d < bestd:
                        bestd, bestv, best = d, v, (gx, gz)
                if bestd <= tol:
                    ok += 1
                else:
                    bad += 1
                    if worst is None:
                        worst = (x, z, bestv, best, bestd)
            grand_ok += ok
            grand_bad += bad
            flag = "" if not bad else "   <== PHANTOM"
            print(f"  {st:16} {ok}/{ok + bad} confirmed{flag}")
            if worst:
                x, z, bv, b, d = worst
                near = f"{bv} at {b}, {d} away" if b else "nothing found"
                print(f"      e.g. ({x},{z}) -- nearest real: {near}")

    tot = grand_ok + grand_bad
    print(f"\n{grand_ok}/{tot} confirmed across {len(todo)} structures, "
          f"{grand_bad} phantom")
    for s in skipped:
        print(f"NOT AUDITED: {s}")
    for s in unknown:
        print(f"NOT AUDITED: {s} (no family mapping -- add one rather than "
              f"assuming a single id)")
    return 1 if grand_bad else 0


if __name__ == "__main__":
    sys.exit(main())
