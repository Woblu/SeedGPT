#!/usr/bin/env python3
"""Confirm structure chest loot on a real server, chest by chest.

    python tier3-java/check_loot_seed.py SEED VERSION STRUCTURE X Z [STRUCTURE X Z ...]

A chest a structure places holds no items until something opens it. It holds
a LootTable and a LootTableSeed, and the items are rolled from those on first
access. So the seed is the first thing to check: it is where the engine's
1.18+ fix lives (the population seed, and so every loot seed, depends on all
64 bits of the world seed -- the engine used to pass 48). This boots one
throwaway world for SEED, finds every chest `checkloot --chests` predicts for
each structure, and reads:

  * LootTable and LootTableSeed. Each server chest is matched to the
    prediction with the same seed; one that matches only the prediction from
    the old 48-bit code, or nothing, is a failure. Whether the seed sat at the
    predicted position is reported separately: cubiomes pairs a desert
    pyramid's four seeds with its four chest positions in the wrong order for
    some orientations (the set of positions and the set of seeds are right).
  * the items, after making the server roll them without moving anything
    (copying slot 0 onto itself reads the slot, which unpacks the chest), as
    totals per item id plus each enchanted item's enchantments, against
    checkloot's roll for that seed.

The world is deleted afterwards. One server, a minute or two. The default
server.jar is 1.21.1, so VERSION should normally be 1.21.1.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from java_oracle import JavaServer  # noqa: E402

ROOT = os.path.dirname(HERE)
CHECKLOOT = os.path.join(ROOT, "build", "checkloot.exe")    # .exe on every OS (build.sh)
MASK48 = (1 << 48) - 1
CHEST_LINE = re.compile(r"^chest (-?\d+) (-?\d+) (\S+) (-?\d+):(.*)$")
ITEM_TOK = re.compile(r"(\d+)x([^\s{]+)(?:\{([^}]*)\})?")
# What the game turns an item into on the way out: an explorer map is filled.
RENAME = {"minecraft:map": "minecraft:filled_map"}


def bare(name):
    return name.split(":", 1)[-1]


def predict(seed, structure, x, z, version):
    """checkloot's chests: [dict(x, z, table, seed, items, enchants)]."""
    r = subprocess.run([CHECKLOOT, str(seed), structure, str(x), str(z), version, "--chests"],
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        m = CHEST_LINE.match(line.strip())
        if not m:
            continue
        items, enchants = {}, []
        for n, name, ench in ITEM_TOK.findall(m.group(5)):
            name = RENAME.get(name, name)
            items[name] = items.get(name, 0) + int(n)
            if ench:
                levels = tuple(sorted((bare(e.split(":")[0]), int(e.split(":")[1]))
                                      for e in ench.split(",")))
                enchants.append((name, levels))
        out.append(dict(x=int(m.group(1)), z=int(m.group(2)), table=m.group(3),
                        seed=int(m.group(4)), items=items, enchants=sorted(enchants)))
    return out


def server_items(nbt):
    """({id: total count}, [(id, enchantments)]) from an `Items` list as /data get prints it.

    Items are split on braces at depth one, so an id inside an item's
    components (an enchantment, a trim) is never mistaken for the item's own.
    """
    totals, enchants, depth, cur = {}, [], 0, []
    for ch in nbt:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = []
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                body, flat, d = "".join(cur), [], 0
                for c in body:                 # the item's own fields only
                    if c in "{[":
                        d += 1
                    elif c in "}]":
                        d -= 1
                    elif d == 0:
                        flat.append(c)
                flat = "".join(flat)
                mi = re.search(r'\bid: "([^"]+)"', flat)
                mc = re.search(r"\bcount: (\d+)", flat)
                if mi:
                    totals[mi.group(1)] = totals.get(mi.group(1), 0) + (int(mc.group(1)) if mc else 1)
                    lv = re.search(r'(?:stored_)?enchantments": \{levels: \{([^}]*)\}', body)
                    if lv:
                        levels = tuple(sorted((bare(k), int(v)) for k, v in
                                              re.findall(r'"([^"]+)": (\d+)', lv.group(1))))
                        enchants.append((mi.group(1), levels))
                continue
        if depth >= 1:
            cur.append(ch)
    return totals, sorted(enchants)


def read_chest(srv, cx, cz):
    """(y, table, seed, items, enchants) of the chest in this column, or a reason string."""
    ys = srv.blocks(cx, cz, -20, 140, "minecraft:chest")
    if ys is None:
        return "chunk did not load -- UNKNOWN"
    if not ys:
        return "no chest in this column -- MISSING"
    cy = min(ys)
    table = seed = None
    for line in srv.run([f"data get block {cx} {cy} {cz} LootTable",
                         f"data get block {cx} {cy} {cz} LootTableSeed"]):
        m = re.search(r'block data: "([^"]+)"', line)
        if m:
            table = m.group(1)
        m = re.search(r"block data: (-?\d+)L", line)
        if m:
            seed = int(m.group(1))
    # Roll the loot in place: reading slot 0 unpacks the chest, and writing the
    # same stack back moves nothing.
    srv.run(f"item replace block {cx} {cy} {cz} container.0 from block {cx} {cy} {cz} container.0")
    items, enchants = {}, []
    for line in srv.run(f"data get block {cx} {cy} {cz} Items"):
        if "block data:" in line:
            items, enchants = server_items(line.split("block data:", 1)[1])
    return cy, table, seed, items, enchants


def main():
    args = sys.argv[1:]
    if len(args) < 5 or (len(args) - 2) % 3:
        print(__doc__)
        return 2
    seed, version = int(args[0]), args[1]
    targets = [(args[i], int(args[i + 1]), int(args[i + 2])) for i in range(2, len(args), 3)]
    print(f"seed {seed} ({'above' if seed & ~MASK48 else 'within'} 2^48), version {version}")

    plans = []
    for structure, x, z in targets:
        full = predict(seed, structure, x, z, version)
        old = {c["seed"] for c in predict(seed & MASK48, structure, x, z, version)}
        print(f"  {structure} at {x},{z}: {len(full)} chest(s) predicted")
        plans.append((structure, full, old))
    total = sum(len(p[1]) for p in plans)
    if not total:
        print("nothing to check")
        return 2

    seeds_ok = items_ok = mispaired = 0
    with JavaServer(seed, fresh=True) as srv:
        try:
            for structure, full, old in plans:
                by_seed = {c["seed"]: c for c in full}
                for c in full:
                    got = read_chest(srv, c["x"], c["z"])
                    if isinstance(got, str):
                        print(f"  {structure} chest {c['x']},{c['z']}: {got}")
                        continue
                    cy, table, lseed, items, enchants = got
                    want = by_seed.get(lseed)
                    where = f"{structure} chest {c['x']},{cy},{c['z']}"
                    if want is None:
                        why = ("matches the OLD 48-bit prediction" if lseed in old else
                               "matches no prediction")
                        print(f"  {where}: {table} seed {lseed} -> MISMATCH, {why}")
                        continue
                    table_ok = bool(table) and table.endswith("/" + want["table"])
                    seeds_ok += table_ok
                    paired = (want["x"], want["z"]) == (c["x"], c["z"])
                    mispaired += not paired
                    same = items == want["items"] and enchants == want["enchants"]
                    items_ok += same
                    print(f"  {where}: {table} seed {lseed} -> "
                          f"{'MATCH' if table_ok else 'WRONG TABLE'}"
                          f"{'' if paired else ' (predicted at ' + str(want['x']) + ',' + str(want['z']) + ')'}"
                          f"; items {'MATCH' if same else 'DIFFER'}")
                    if not same:
                        print(f"      server    {sorted(items.items())} {enchants}")
                        print(f"      predicted {sorted(want['items'].items())} {want['enchants']}")
                    elif enchants:
                        print(f"      enchanted: {enchants}")
            if srv.errors:
                print("  server rejected:", srv.errors[:3])
        finally:
            srv.cleanup()
    print(f"loot seeds {seeds_ok}/{total}, items {items_ok}/{total}"
          + (f", {mispaired} seed(s) at a different chest of the same structure" if mispaired else ""))
    return 0 if seeds_ok == items_ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
