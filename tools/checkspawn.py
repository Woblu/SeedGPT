"""Independently verify that spawn-relative matches really are within radius
of the world spawn.

Reads find.exe's machine-readable TSV (FIND_TSV) rather than scraping its
human output -- the aligned "x=%6d" columns put spaces inside the field, which
quietly breaks naive awk/sed splitting. Spawn comes from describe.exe, a
separate binary, so this does not rest on find.exe agreeing with itself.

    usage: checkspawn.py <hits.tsv> <radius> <version> <describe.exe>
"""
import re
import subprocess
import sys

tsv, radius, version, describe = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]

rows = [r.split("\t") for r in open(tsv, encoding="utf-8").read().splitlines() if r.strip()]
if not rows:
    sys.exit("no hits in TSV")

worst, worst_seed = -1, None
for row in rows:
    seed = row[0]
    out = subprocess.run([describe, seed, version, "50"],
                         capture_output=True, text=True).stdout
    m = re.search(r"world spawn\s*:\s*x=(-?\d+)\s+z=(-?\d+)", out)
    if not m:
        sys.exit(f"could not read spawn for seed {seed}")
    sx, sz = int(m.group(1)), int(m.group(2))
    # row is: seed, (id, x, z) repeated
    for i in range(1, len(row) - 2, 3):
        x, z = int(row[i + 1]), int(row[i + 2])
        d = round(((x - sx) ** 2 + (z - sz) ** 2) ** 0.5)
        if d > worst:
            worst, worst_seed = d, seed

if worst > radius:
    sys.exit(f"seed {worst_seed}: {worst} blocks from spawn, radius was {radius}")
print(f"{len(rows)} seeds, worst {worst} blocks from spawn (radius {radius})")
