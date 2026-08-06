#!/usr/bin/env python3
"""How fast is the headless path against the server, on the same chests?

The server was measured at ~6.5s per chest, nearly all of it chunk generation
plus a 44s boot per seed. This path generates the same chunk in-process with no
server, no disk and no boot per seed, and replicates BuriedTreasurePiece's
placement scan directly. This times it so the speedup is a number rather than a
hope, and checks the chest Y it reports against what the server found.

    python bench_fast.py
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CP_FILE = os.path.join(HERE, "cp.txt")

# The chests to check against are LOCATED FRESH for the seed under test, never
# copied from notes. A first version of this hard-coded two chests taken from a
# different seed's run; the server correctly reported nothing there, and the
# fast path -- which computes a placement Y for any column handed to it, treasure
# or not -- answered anyway. That reads exactly like a real disagreement and
# cost an investigation. Both sides must be asked about the same world.
SEED = 4
RADIUS = 3000


def java_bin():
    for cand in (os.environ.get("JAVA21_HOME"), os.environ.get("JAVA_HOME"),
                 r"C:\Program Files\Java\jdk-21"):
        if cand:
            j = os.path.join(cand, "bin", "java.exe")
            if os.path.isfile(j):
                return j
    return "java"


def classpath():
    cp = open(CP_FILE, encoding="utf-8").read().replace("\r", "").strip().rstrip(";")
    cp = os.pathsep.join(x for x in cp.split("\n") if x)
    return os.path.join(HERE, "out") + os.pathsep + cp


def server_truth(seed, radius):
    """What the server says, for exactly the treasures we are about to time."""
    sys.path.insert(0, os.path.join(ROOT, "tier3-java"))
    from java_oracle import JavaServer
    from treasure_probe import locate
    spots = locate(seed, "1.21", radius)
    truth = {}
    with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
        for (x, z) in spots:
            srv.forceload(x, z, radius_chunks=1)
        for (x, z) in spots:
            if not srv.loaded(x, z):
                continue
            ys = sorted(srv.blocks(x, z, 10, 110, "minecraft:chest"))
            # A column can hold a shipwreck or ocean-ruin chest too, and this
            # comparison is about buried treasure. Ambiguous columns are dropped
            # rather than guessed at.
            truth[(x, z)] = ys[0] if len(ys) == 1 else None
    JavaServer(seed=seed).cleanup()
    return truth


def main():
    truth = server_truth(SEED, RADIUS)
    usable = [(k, v) for k, v in truth.items() if v is not None]
    print(f"seed {SEED}: {len(truth)} treasures located, "
          f"{len(usable)} with exactly one chest in the column\n")

    t0 = time.time()
    p = subprocess.Popen([java_bin(), "-cp", classpath(), "OutpostWorldgen", "treasure"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, cwd=HERE, bufsize=1)
    for line in p.stdout:
        if line.strip().endswith("READY"):
            break
    boot = time.time() - t0
    print(f"boot {boot:.1f}s  (server boot was 44s, and this one is paid ONCE "
          f"for every seed rather than once per seed)\n")

    agree, dis, bad = 0, 0, []
    times = []
    for ((x, z), want_y) in usable:
        seed = SEED
        a = time.time()
        p.stdin.write(f"{seed} {x} {z}\n")
        p.stdin.flush()
        got = None
        for line in p.stdout:
            if "chestY=" in line:
                kv = dict(t.split("=", 1) for t in line.strip().split("\t") if "=" in t)
                got = int(kv.get("chestY", -1))
                break
        dt = time.time() - a
        times.append(dt)
        ok = (got == want_y)
        agree += ok
        dis += (not ok)
        if not ok:
            bad.append((x, z, got, want_y))
            print(f"  {dt:6.3f}s  ({x},{z})  chestY={got}  server {want_y}"
                  f"  <== DISAGREES")
    try:
        p.stdin.write("quit\n")
        p.stdin.flush()
    except Exception:
        pass
    p.terminate()

    per = sum(times) / len(times)
    print(f"\n{per:.3f}s per chest against the server's ~6.5s "
          f"=> {6.5/per:.0f}x")
    print(f"agreement with the server: {agree}/{agree+dis}")
    if dis:
        print("DISAGREEMENTS ABOVE. The fast path is not a drop-in replacement "
              "until these are explained -- speed is worthless if the placement "
              "it replicates is not the one the game uses.")
    return 0 if not dis else 1


if __name__ == "__main__":
    sys.exit(main())
