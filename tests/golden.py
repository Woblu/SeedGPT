#!/usr/bin/env python3
"""Same seed, same world, on every platform.

A finder that returns a wrong seed fails silently: nothing crashes, and the
player only finds out by flying to coordinates where nothing is. A different
libm, a fused multiply-add or an unsigned char is enough, and none of them
would stop the build. So this runs a fixed set of searches, seed descriptions
and maps, and compares a hash of every output with tests/golden.json. CI runs
it on Windows, Linux and macOS, x86-64 and ARM; the same file has to pass on
all of them.

    python tests/golden.py            compare with tests/golden.json
    python tests/golden.py --write    regenerate it (a deliberate engine change)

Single-threaded and small on purpose: about half a minute on one core.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = Path(__file__).with_name("golden.json")

# Every condition family with its own arithmetic, across the three eras of the
# generator: layers (<= 1.17), multi-noise (1.18+) and the current version.
SEARCHES = {
    "1.16 biome": ('{"version":"1.16","conditions":[{"id":"b","biome":"jungle","within":800}]}', 4000),
    "1.16 fortress": ('{"version":"1.16","conditions":[{"id":"f","structure":"fortress","within":400}]}', 40000),
    "1.21 village+biome": ('{"version":"1.21","conditions":[{"id":"v","structure":"village","within":600},'
                           '{"id":"b","biome":"forest","within":200,"of":"v"}]}', 40000),
    "1.21 pyramid loot": ('{"version":"1.21","conditions":[{"id":"l","loot":{"structure":"desert_pyramid",'
                          '"item":"diamond","count":1},"within":3000}]}', 2000),
    "1.21 ore": ('{"version":"1.21","conditions":[{"id":"o","ore":"diamond","count":12,"within":48}]}', 1500),
    "1.21 height": ('{"version":"1.21","conditions":[{"id":"h","height":170,"within":400}]}', 6000),
    "1.21 slime": ('{"version":"1.21","conditions":[{"id":"s","slime":7,"within":64}]}', 40000),
    "1.21 end city ship": ('{"version":"1.21","conditions":[{"id":"e","structure":"end_city","ship":true,'
                           '"within":2000}]}', 8000),
    "26.2 mansion": ('{"version":"26.2","conditions":[{"id":"m","structure":"mansion","within":1500}]}', 12000),
    "26.2 trial chambers": ('{"version":"26.2","conditions":[{"id":"t","structure":"trial_chambers",'
                            '"within":400}]}', 20000),
    "26.2 island": ('{"version":"26.2","conditions":[{"id":"i","island":true,"of":"origin","pct":80,'
                    '"within":200}]}', 4000),
}

SEEDS = ["0", "12345", "-4172144997902289642", "8486214866965744170"]
VERSIONS = ["1.12", "1.16", "1.18", "1.21", "26.2"]
MAP_DIMS = {"1.16": [""], "1.21": ["", "nether", "end"], "26.2": ["", "nether", "end"]}


def tool(name):
    for candidate in (ROOT / "build" / f"{name}.exe", ROOT / "build" / name):
        if candidate.exists():
            return str(candidate)
    sys.exit(f"build/{name}.exe is missing. Run: ./build.sh all")


def digest(data):
    return hashlib.sha256(data).hexdigest()[:24]


def run(args, env=None):
    done = subprocess.run(args, capture_output=True, env=env, timeout=300)
    if done.returncode != 0:
        sys.exit(f"failed ({done.returncode}): {' '.join(args)}\n{done.stderr.decode(errors='replace')}")
    return done.stdout


def collect():
    """Every output, as {name: "<hash> <detail>"}. The detail is what a diff
    shows, so a mismatch says how far off a result is, not merely that it is."""
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name, (query, seeds) in SEARCHES.items():
            qpath, tsv = tmp / "q.json", tmp / "hits.tsv"
            qpath.write_text(query, encoding="utf-8")
            tsv.unlink(missing_ok=True)
            run([tool("find"), str(qpath), str(seeds), "1"], dict(os.environ, FIND_TSV=str(tsv)))
            lines = sorted(tsv.read_text(encoding="utf-8").split("\n")) if tsv.exists() else []
            lines = [l.rstrip("\r") for l in lines if l.strip()]
            first = lines[0].split("\t")[0] if lines else "none"
            out[f"search {name}"] = f"{digest(chr(10).join(lines).encode())} {len(lines)} hits, first {first}"

        for version in VERSIONS:
            for seed in SEEDS:
                text = run([tool("describe"), seed, version, "3000", "-", "json"])
                rows = json.loads(text.decode("utf-8"))
                n = len(rows.get("structures", rows if isinstance(rows, list) else []))
                out[f"describe {version} {seed}"] = f"{digest(text.replace(bytes([13]), b''))} {n} rows"

        for version, dims in MAP_DIMS.items():
            for dim in dims:
                png = tmp / "m.png"
                args = [tool("map"), SEEDS[1], version, "1500", "256", str(png), "0", "0", "-"]
                run(args + ([dim] if dim else []))
                out[f"map {version} {dim or 'overworld'}"] = f"{digest(png.read_bytes())} {png.stat().st_size} bytes"
    return out


def main():
    actual = collect()
    if "--write" in sys.argv:
        GOLDEN.write_text(json.dumps(actual, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {GOLDEN.name}: {len(actual)} outputs")
        return 0
    expected = json.loads(GOLDEN.read_text(encoding="utf-8"))
    wrong = [k for k in sorted(set(expected) | set(actual)) if expected.get(k) != actual.get(k)]
    for k in wrong:
        print(f"DIFFERENT  {k}\n  expected {expected.get(k)}\n  actual   {actual.get(k)}")
    print(f"golden: {len(actual) - len(wrong)}/{len(actual)} outputs identical")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
