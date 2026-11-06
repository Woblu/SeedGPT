#!/usr/bin/env python3
"""Separate a real ore CASING from a chest that merely touches ore.

The hunt scores ore faces, which conflates two different things. The fill only
overwrites faces that were air or water, so a face already holding solid rock
keeps whatever it had. One ore face among five sand is therefore terrain the
chest happened to sit against -- the very thing the 26-neighbour probe was
measuring before, and not what was asked for. A genuine casing shows up as the
MODAL face block being ore, since the fill writes one block into every face it
can reach.

    python ore_report.py ore_finds_*.jsonl

Reports casings and contact separately, and never merges them.
"""
import glob
import json
import sys
from collections import Counter

ORE = lambda b: b and (b.endswith("_ore") or b.endswith("_block") and "raw_" in b)


def main():
    pats = sys.argv[1:] or ["ore_finds_*.jsonl"]
    files = [f for p in pats for f in glob.glob(p)]
    if not files:
        sys.exit("no jsonl files matched")

    rows = []
    for f in files:
        with open(f, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))

    casings, contact = [], []
    for r in rows:
        faces = r.get("faces") or []
        n_ore = sum(1 for b in faces if ORE(b))
        (casings if n_ore >= 3 else contact).append((n_ore, r))

    casings.sort(key=lambda t: -t[0])
    contact.sort(key=lambda t: -t[0])

    print(f"{len(rows)} chests with any ore face, from {len(files)} file(s)\n")
    print(f"CASINGS (>=3 of 6 faces ore -- the fill wrote it): {len(casings)}")
    for n, r in casings[:20]:
        fs = [b.split(":")[1] if b else "?" for b in (r.get("faces") or [])]
        print(f"  {n}/6  seed {r['seed']:>8}  ({r['x']},{r['y']},{r['z']})  {fs}")
        print(f"        /tp {r['x']} {r['y']} {r['z']}")
    if not casings:
        print("  none -- no chest in this data was walled in by ore")

    print(f"\nCONTACT ONLY (1-2 ore faces, chest sat against it): {len(contact)}")
    tally = Counter()
    for n, r in contact:
        for b in (r.get("faces") or []):
            if ORE(b):
                tally[b] += 1
    for b, c in tally.most_common():
        print(f"  {c:4d}  {b}")
    best = contact[:5]
    for n, r in best:
        fs = [b.split(":")[1] if b else "?" for b in (r.get("faces") or [])]
        print(f"  {n}/6  seed {r['seed']:>8}  ({r['x']},{r['y']},{r['z']})  {fs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
