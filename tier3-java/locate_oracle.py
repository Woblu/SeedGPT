#!/usr/bin/env python3
"""Ask the REAL game where a structure is, and compare cubiomes against it.

Everything in this project that says "there is an outpost at (x,z)" comes from
cubiomes -- a reimplementation. describe.exe, locate.exe and the search all agree
with each other because they are the SAME engine; three parts of one
reimplementation agreeing is one opinion, not three. The only independent
authority is Minecraft itself.

    python locate_oracle.py <seed> <structure> [radius] [--version 1.21]

Runs `/locate structure` on a real server and prints what the game says, next to
what cubiomes says. A disagreement here is a real engine bug and matters far
beyond one seed; agreement is the only thing that makes a coordinate trustworthy.

/locate reports the NEAREST instance from the origin, so this compares nearest to
nearest. cubiomes listing more instances further out is not a disagreement.
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

# The game's ids, against the names this project's query language uses.
GAME_ID = {
    "outpost": "minecraft:pillager_outpost",
    "pillager_outpost": "minecraft:pillager_outpost",
    "mansion": "minecraft:mansion",
    "village": "minecraft:village_plains",
    "monument": "minecraft:monument",
    "desert_pyramid": "minecraft:desert_pyramid",
    "jungle_temple": "minecraft:jungle_temple",
    "swamp_hut": "minecraft:swamp_hut",
    "igloo": "minecraft:igloo",
    "shipwreck": "minecraft:shipwreck",
    "buried_treasure": "minecraft:buried_treasure",
    "treasure": "minecraft:buried_treasure",
    "ancient_city": "minecraft:ancient_city",
    "ruined_portal": "minecraft:ruined_portal",
    "trial_chambers": "minecraft:trial_chambers",
    "ocean_ruin": "minecraft:ocean_ruin_cold",
}


def cubiomes_says(seed, name, radius, version):
    r = subprocess.run([LOCATE, str(seed), version, name, str(radius)],
                       capture_output=True, text=True)
    if "ERROR unknown structure" in r.stdout:
        return None, r.stdout.strip()
    out = []
    for ln in r.stdout.splitlines():
        p = ln.split()
        if len(p) == 2:
            try:
                out.append((int(p[0]), int(p[1])))
            except ValueError:
                pass
    out.sort(key=lambda p: p[0] * p[0] + p[1] * p[1])
    return out, None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    seed = int(sys.argv[1])
    name = sys.argv[2]
    radius = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("-") else 3000
    version = "1.21"
    for i, a in enumerate(sys.argv):
        if a == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]

    gid = GAME_ID.get(name)
    if not gid:
        sys.exit(f"no game id mapped for {name!r} -- add one to GAME_ID rather "
                 f"than guessing, since a wrong id makes the game answer about "
                 f"a different structure")

    cub, err = cubiomes_says(seed, name, radius, version)
    if cub is None:
        sys.exit(f"cubiomes: {err}")

    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        out = srv.run([f"locate structure {gid}"])
    try:
        JavaServer(seed=seed).cleanup()
    except Exception:
        pass

    text = "\n".join(out)
    # "The nearest minecraft:pillager_outpost is at [592, ~, 672]" -- wording has
    # shifted between versions, so match the coordinates rather than the prose.
    m = re.search(r"\[\s*(-?\d+)\s*,\s*[~\d-]+\s*,\s*(-?\d+)\s*\]", text)
    game = (int(m.group(1)), int(m.group(2))) if m else None

    print(f"seed {seed}, {name} ({gid}), radius {radius}\n")
    print(f"  cubiomes nearest : {cub[0] if cub else 'NONE within radius'}")
    print(f"  the game says    : {game if game else 'no answer parsed'}")
    if cub:
        print(f"  cubiomes lists   : {len(cub)} within {radius}")

    if game is None:
        print("\nThe game gave no locatable answer. That is UNKNOWN -- it can "
              "mean the structure is outside the search range it will scan, not "
              "that it does not exist.")
        print(f"  raw: {text.strip()[-300:]}")
        return 2
    if not cub:
        print("\nDISAGREEMENT: the game has one and cubiomes does not.")
        return 1
    dx, dz = abs(game[0] - cub[0][0]), abs(game[1] - cub[0][1])
    # /locate reports the structure's start piece, which can sit a chunk or two
    # from the position cubiomes reports for the same structure.
    if dx <= 32 and dz <= 32:
        print(f"\nAGREE (within {max(dx, dz)} blocks). The coordinate is "
              f"trustworthy: an independent engine put it in the same place.")
        return 0
    print(f"\nDISAGREEMENT: {dx} blocks in x, {dz} in z. cubiomes is wrong here, "
          f"or right about a different instance -- either way a coordinate from "
          f"it should not be reported until this is understood.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
