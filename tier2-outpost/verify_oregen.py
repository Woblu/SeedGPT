#!/usr/bin/env python3
"""Does headless decoration agree with the server about the CASING block?

Speed is worthless if the answer is wrong, and this path has every opportunity to
be wrong: it decorates a single chunk rather than the 3x3 vanilla writes across,
it fakes light, and it drops every write that lands outside the chunk. So it is
checked against the server on the two quantities a hunt actually consumes:

    chestY   where the treasure lands
    fill     the block it lands on, which is what gets written into the faces

The server cannot be asked for `fill` directly -- the chest replaced that block --
so the comparison uses the modal face block, which IS the fill wherever the fill
had air or water to overwrite. Chests whose faces were all pre-existing solid
have no fill to read and are reported separately rather than counted as
agreement.

    python verify_oregen.py [seed ...]
"""
import os
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tier3-java"))
from bench_fast import java_bin, classpath                      # noqa: E402
from java_oracle import JavaServer                              # noqa: E402
from treasure_probe import locate, CANDIDATES                   # noqa: E402
from hunt_treasure_ore import face_probe, SCREEN                # noqa: E402
from treasure_casing import FACES                               # noqa: E402


def ore_worker():
    p = subprocess.Popen([java_bin(), "-cp", classpath(), "OreGen", "oreserver"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, cwd=HERE, bufsize=1)
    for line in p.stdout:
        if line.strip().endswith("READY"):
            return p
    raise RuntimeError("OreGen never reported READY")


def ask(p, seed, x, z):
    p.stdin.write(f"{seed} {x} {z}\n")
    p.stdin.flush()
    for line in p.stdout:
        if line.startswith("O\t"):
            t = line.rstrip("\n").split("\t")
            if any(f.startswith("err=") for f in t):
                return None, None
            kv = dict(f.split("=", 1) for f in t if "=" in f)
            return int(kv.get("chestY", -1)), kv.get("fill")
    return None, None


def main():
    seeds = [int(a) for a in sys.argv[1:]] or [4, 5000, 5001]
    p = ore_worker()
    y_ok = y_bad = f_ok = f_bad = nofill = err = 0
    bad = []
    try:
        for seed in seeds:
            spots = locate(seed, "1.21", 3000)
            with JavaServer(seed=seed, fresh=True, quiet=True) as srv:
                for (x, z) in spots:
                    srv.forceload(x, z, radius_chunks=1)
                for (x, z) in spots:
                    if not srv.loaded(x, z):
                        continue
                    ys = sorted(srv.blocks(x, z, 10, 110, "minecraft:chest"))
                    if len(ys) != 1:
                        continue                    # ambiguous column, skipped
                    sy = ys[0]
                    full = face_probe(srv, x, sy, z, CANDIDATES)
                    modal, n = Counter("?" if b is None else b
                                       for b in full.values()).most_common(1)[0]
                    gy, gfill = ask(p, seed, x, z)
                    if gy is None:
                        err += 1
                        continue
                    if gy == sy:
                        y_ok += 1
                    else:
                        y_bad += 1
                        bad.append((seed, x, z, "chestY", gy, sy))
                    if n < 3:
                        nofill += 1                 # no readable casing
                    elif gfill == modal:
                        f_ok += 1
                    else:
                        f_bad += 1
                        bad.append((seed, x, z, "fill", gfill, modal))
            JavaServer(seed=seed).cleanup()
    finally:
        try:
            p.stdin.write("quit\n")
            p.stdin.flush()
        except Exception:
            pass
        p.terminate()

    tot_y = y_ok + y_bad
    tot_f = f_ok + f_bad
    print(f"\nchestY: {y_ok}/{tot_y} agree"
          f"{'' if not tot_y else f'  ({100*y_ok/tot_y:.0f}%)'}")
    print(f"fill:   {f_ok}/{tot_f} agree"
          f"{'' if not tot_f else f'  ({100*f_ok/tot_f:.0f}%)'}"
          f"   [{nofill} chests had no readable casing, {err} errors]")
    for (s, x, z, what, got, want) in bad[:15]:
        print(f"  seed {s} ({x},{z}) {what}: headless {got}  server {want}")
    if f_bad or y_bad:
        print("\nDISAGREEMENTS ABOVE. Until these are understood the headless "
              "path must not replace the server for ore questions -- a fast "
              "wrong answer is worse than a slow right one.")
    return 0 if not (f_bad or y_bad) else 1


if __name__ == "__main__":
    sys.exit(main())
