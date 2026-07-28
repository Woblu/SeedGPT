#!/usr/bin/env python3
"""Read Minecraft's own source, so a rule can be looked up instead of guessed.

Every correctness win in this project came from checking what the game actually
does -- the 1.18+ surface rules, the buried-treasure chest placement, the
outpost/village exclusion zone. Until now that meant `javap -c` and reading
bytecode, which is authoritative but slow and easy to misread. `gradle
genSources` in tier2-outpost decompiles the remapped jar to real Java; this
finds and searches it.

    python tools/mcsrc.py StructurePlacement            # print a class
    python tools/mcsrc.py --grep exclusion_zone         # search the sources
    python tools/mcsrc.py --grep "isPlacementForbidden" --context 6
    python tools/mcsrc.py --data structure_set/         # list/print data files

LICENSING: Mojang's mappings permit deobfuscation for local use, but the code
stays proprietary. Read it here; never copy it into this repo, and never
redistribute it. The sources jar is gitignored for that reason.
"""

import argparse
import glob
import os
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).parent.parent


def find_jar(kind="sources"):
    """Locate the loom-built jar. `kind` is "sources" or "classes".

    The classes jar is taken from tier2-outpost/cp.txt when present: the loom
    cache holds several jars whose names all look plausible, and the classpath
    names the one the backend actually compiles against -- the one carrying the
    data files.
    """
    if kind == "classes":
        cp = ROOT / "tier2-outpost" / "cp.txt"
        if cp.exists():
            raw = cp.read_text(encoding="utf-8").replace("\n", ";")
            for entry in raw.split(";"):
                if "minecraft-merged" in entry and entry.strip().endswith(".jar"):
                    return entry.strip()
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    pat = os.path.join(home, ".gradle", "caches", "fabric-loom", "minecraftMaven",
                       "net", "minecraft", "minecraft-merged", "*", "*.jar")
    jars = glob.glob(pat)
    want = [j for j in jars if ("-sources.jar" in j) == (kind == "sources")]
    if not want:
        sys.exit(f"no {kind} jar found. Run, in tier2-outpost:\n"
                 f"  JAVA_HOME='C:/Program Files/Java/jdk-21' "
                 f"../tools/gradle-dist/gradle-8.10.2/bin/gradle --no-daemon genSources")
    return max(want, key=os.path.getmtime)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?", help="class name (or part of a path) to print")
    ap.add_argument("--grep", help="regex to search across all sources")
    ap.add_argument("--data", help="also search/print data files matching this path fragment")
    ap.add_argument("--context", type=int, default=3, help="lines of context for --grep")
    ap.add_argument("--limit", type=int, default=40, help="max matches to print")
    a = ap.parse_args()

    if a.data:
        z = zipfile.ZipFile(find_jar("classes"))
        names = [n for n in z.namelist() if n.startswith("data/") and a.data in n]
        if not names:
            sys.exit(f"no data file matching {a.data!r}")
        if len(names) > 1 and not a.grep:
            print("\n".join(names[:a.limit]))
            print(f"\n{len(names)} files; narrow the path or add --grep")
            return
        for n in names[:a.limit]:
            body = z.read(n).decode("utf-8", "replace")
            if a.grep and not re.search(a.grep, body, re.I):
                continue
            print("=" * 8, n)
            print(body)
        return

    z = zipfile.ZipFile(find_jar("sources"))
    srcs = [n for n in z.namelist() if n.endswith(".java")]

    if a.grep:
        pat = re.compile(a.grep, re.I)
        shown = 0
        for n in srcs:
            body = z.read(n).decode("utf-8", "replace").split("\n")
            for i, line in enumerate(body):
                if not pat.search(line):
                    continue
                lo, hi = max(0, i - a.context), min(len(body), i + a.context + 1)
                print(f"--- {n}:{i+1}")
                for k in range(lo, hi):
                    print(f"  {'>' if k == i else ' '} {body[k]}")
                shown += 1
                if shown >= a.limit:
                    print(f"\n(stopped at {a.limit} matches; raise --limit)")
                    return
        if not shown:
            print("no matches")
        return

    if not a.name:
        print(f"{len(srcs)} decompiled sources. Give a class name or --grep.")
        return
    hits = [n for n in srcs if a.name.lower() in n.lower()]
    if not hits:
        sys.exit(f"no source matching {a.name!r}")
    if len(hits) > 1:
        exact = [n for n in hits if n.rsplit("/", 1)[-1].lower() == a.name.lower() + ".java"]
        if exact:
            hits = exact
        else:
            print("\n".join(hits[:a.limit]))
            print(f"\n{len(hits)} matches; be more specific")
            return
    print(z.read(hits[0]).decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
