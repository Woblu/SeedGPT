#!/usr/bin/env python3
"""What is worth going to in THIS seed?

The rest of this project searches for a seed matching criteria. This is the
other direction: you already have a world, and you want to know where the good
chests are. Inspired by Kludwisz's CoolStuffFinder, which does the same for four
specific nether chest configurations.

    python find_cool_stuff.py <seed> [--version 1.21] [--radius 3000]
                              [--item minecraft:diamond --min 8]
                              [--ench looting 3]

With no --item it reports every chest that clears a built-in notability bar per
structure. With --item it reports every chest holding at least --min of that
item, notable or not.

ENCHANTMENTS ARE READ, including levels: --ench looting 3 finds items carrying
Looting III or better. cubiomes has carried these on ItemStack all along and
nothing here read them until now.

Bastion and fortress share salt and geometry and are MUTUALLY EXCLUSIVE at a
site: locate reports whichever one is really there, so a site is scanned as one
or the other and never as both. Getting that wrong is how a mineshaft hunt in
this project once counted every corridor twice.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOCATE = os.path.join(ROOT, "build", "locate.exe")
CHECKLOOT = os.path.join(ROOT, "build", "checkloot.exe")
CHECKRP = os.path.join(ROOT, "build", "checkrpchest.exe")

# Structures whose chests this engine can actually read. Anything not here is
# skipped LOUDLY at the end rather than quietly producing no finds.
LOOTABLE = ["ruined_portal", "desert_pyramid", "jungle_temple", "igloo",
            "outpost", "shipwreck", "fortress", "bastion"]

# The bar for "worth the trip", per structure. Deliberately conservative: a
# report that flags every chest is the same as no report at all.
NOTABLE = {
    "bastion":        [("minecraft:obsidian", 24), ("minecraft:gilded_blackstone", 12),
                       ("minecraft:ancient_debris", 2), ("minecraft:netherite_scrap", 2),
                       ("minecraft:diamond", 6), ("minecraft:golden_apple", 3),
                       ("minecraft:enchanted_golden_apple", 1)],
    "fortress":       [("minecraft:diamond", 8), ("minecraft:golden_horse_armor", 1),
                       ("minecraft:diamond_horse_armor", 1)],
    "ruined_portal":  [("minecraft:enchanted_golden_apple", 2), ("minecraft:gold_block", 2),
                       ("minecraft:diamond", 4)],
    "desert_pyramid": [("minecraft:enchanted_golden_apple", 2), ("minecraft:diamond", 5),
                       ("minecraft:emerald", 6), ("minecraft:gold_ingot", 12)],
    "jungle_temple":  [("minecraft:diamond", 3), ("minecraft:emerald", 4)],
    "igloo":          [("minecraft:emerald", 2), ("minecraft:golden_apple", 1)],
    "outpost":        [("minecraft:enchanted_book", 1)],
    "shipwreck":      [("minecraft:diamond", 3), ("minecraft:emerald", 5),
                       ("minecraft:gold_ingot", 8)],
}

ITEM_RE = re.compile(r"(\d+)x(minecraft:[a-z_]+)")
# "1xminecraft:golden_sword{looting 3}" -- checkloot prints these on an
# "enchanted:" line, checkrpchest inline after the item.
ENCH_RE = re.compile(r"(\d+)x(minecraft:[a-z_]+)\{([a-z_]+) (\d+)\}")

# Configurations worth a special mention, the way CoolStuffFinder names four.
# Only ones this engine can actually decide; nothing here is approximated.
FAMOUS = [
    ("bastion",       "obsidian",                None, 0, 46,
     "a bastion double chest with 46+ obsidian"),
    ("ruined_portal", "enchanted_golden_apple",  None, 0, 4,
     "a ruined portal chest with 4+ enchanted golden apples"),
    ("ruined_portal", "golden_sword",            "looting", 3, 1,
     "a ruined portal chest with a Looting III golden sword"),
]


def run(args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def locate(seed, version, structure, radius):
    out = []
    for line in run([LOCATE, str(seed), version, structure, str(radius)]).splitlines():
        if line.startswith("ERROR"):
            return None            # unknown name: UNKNOWN, never "none here"
        p = line.split()
        if len(p) == 2:
            try:
                out.append((int(p[0]), int(p[1])))
            except ValueError:
                pass
    return out


def loot_at(seed, version, structure, x, z):
    """({item: count}, [(item, ench, level, count)]) or (None, None)."""
    if structure == "ruined_portal":
        txt = run([CHECKRP, str(seed), version, "--at", str(x), str(z)])
    else:
        txt = run([CHECKLOOT, str(seed), structure, str(x), str(z), version])
    if not txt or "unsupported" in txt:
        return None, None
    items, ench = {}, []
    for n, name in ITEM_RE.findall(txt):
        items[name] = items.get(name, 0) + int(n)
    for n, name, e, lv in ENCH_RE.findall(txt):
        ench.append((name, e, int(lv), int(n)))
    return items, ench


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    seed = sys.argv[1]
    version, radius = "1.21", 3000
    want_item, want_min = None, 1
    want_ench, want_ench_lv = None, 1
    for i, a in enumerate(sys.argv):
        if a == "--ench" and i + 1 < len(sys.argv):
            want_ench = sys.argv[i + 1]
            if i + 2 < len(sys.argv) and sys.argv[i + 2].isdigit():
                want_ench_lv = int(sys.argv[i + 2])
        if a == "--version" and i + 1 < len(sys.argv): version = sys.argv[i + 1]
        if a == "--radius" and i + 1 < len(sys.argv):  radius = int(sys.argv[i + 1])
        if a == "--item" and i + 1 < len(sys.argv):    want_item = sys.argv[i + 1]
        if a == "--min" and i + 1 < len(sys.argv):     want_min = int(sys.argv[i + 1])
    if want_item and not want_item.startswith("minecraft:"):
        want_item = "minecraft:" + want_item

    print(f"seed {seed}, {version}, within {radius} blocks of origin", flush=True)
    if want_ench:
        print(f"looking for items with {want_ench} {want_ench_lv}+", flush=True)
    elif want_item:
        print(f"looking for chests with >= {want_min} {want_item}", flush=True)
    t0 = time.time()
    finds, scanned, skipped = [], 0, []

    for st in LOOTABLE:
        spots = locate(seed, version, st, radius)
        if spots is None:
            skipped.append(st)
            continue
        for (x, z) in spots:
            items, ench = loot_at(seed, version, st, x, z)
            if items is None:
                continue
            scanned += 1
            if want_ench:
                hits = [(f"{it.split(':')[-1]}", lv) for (it, e, lv, c) in ench
                        if e == want_ench and lv >= want_ench_lv]
                if hits:
                    finds.append((st, x, z,
                                  [(f"{want_ench} {lv} {nm}", 1) for nm, lv in hits],
                                  items))
            elif want_item:
                n = items.get(want_item, 0)
                if n >= want_min:
                    finds.append((st, x, z, [(want_item, n)], items))
            else:
                hit = [(it, items.get(it, 0)) for it, need in NOTABLE.get(st, [])
                       if items.get(it, 0) >= need]
                # The named configurations, checked alongside the ordinary bar.
                for (fst, fitem, fe, flv, fn, label) in FAMOUS:
                    if fst != st:
                        continue
                    if fe:
                        c = sum(cc for (it, e, lv, cc) in ench
                                if it.endswith(fitem) and e == fe and lv >= flv)
                    else:
                        c = items.get("minecraft:" + fitem, 0)
                    if c >= fn:
                        hit.append(("** " + label, c))
                if hit:
                    finds.append((st, x, z, hit, items))

    el = time.time() - t0
    # Sorted by how far past the bar they are, so the best thing in the world is
    # the first line rather than the first one the scan happened to reach.
    finds.sort(key=lambda f: -max(n for _, n in f[3]))
    if finds:
        print(f"\n{len(finds)} notable chest(s):\n", flush=True)
        for (st, x, z, hit, items) in finds[:25]:
            what = ", ".join(f"{n}x {it.split(':')[-1]}" for it, n in hit)
            print(f"  {st:14} x={x:<7} z={z:<7}  {what}")
            print(f"      /tp {x} ~ {z}")
            rest = " ".join(f"{c}x{i.split(':')[-1]}" for i, c in
                            sorted(items.items(), key=lambda kv: -kv[1])[:8])
            print(f"      chest: {rest}")
    else:
        print(f"\nnothing cleared the bar in {scanned} chests. That is a "
              f"statement about this seed within {radius} blocks, not about the "
              f"seed as a whole -- widen the radius or name an item with --item.")

    print(f"\n{scanned} chests read in {el:.1f}s")
    if skipped:
        # Never silent: a structure that could not be located is a hole in the
        # scan, and a hole reported as "nothing found there" is the failure this
        # project keeps rediscovering.
        print(f"NOT SCANNED (locate does not know these here): {', '.join(skipped)}")
    print("Enchantments and their levels ARE read -- try --ench looting 3.")
    return 0 if finds else 1


if __name__ == "__main__":
    sys.exit(main())
