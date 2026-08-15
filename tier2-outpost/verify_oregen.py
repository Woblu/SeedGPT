#!/usr/bin/env python3
"""Does headless decoration agree with the server about the CASING block?

Speed is worthless if the answer is wrong, and this path has every opportunity to
be wrong: it decorates a single chunk rather than the 3x3 vanilla writes across,
it fakes light, and it drops every write that lands outside the chunk. So it is
checked against the server on the two quantities a hunt actually consumes:

    chestY   where the treasure lands
    casing   all six face blocks, compared one face at a time

This used to compare a single `fill` against the MODAL server face, because the
server cannot be asked for the fill directly -- the chest replaced that block.
That yardstick was blunt in both directions: it collapsed six blocks into one,
it could not see a chest walled in two different blocks at all, and a chest
whose faces were all pre-existing solid had no readable mode. OreGen now
replicates the piece's own casing loop and emits all six final faces, so the
comparison is face against face and the proxy is gone.

Faces the server cannot name -- a block outside its candidate list -- are
counted as UNKNOWN and reported separately. They are not agreements and they
are not disagreements, and rolling them into either number is how a
verification starts flattering itself.

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
            if "final" not in kv:
                raise RuntimeError("OreGen did not emit final= -- rebuild "
                                   "tier2-outpost before verifying against it")
            # `final`, not `cased`. The server shows a finished chunk, so the
            # only fair comparison is against the faces AFTER decoration has run
            # over the casing. Comparing `cased` would score the structure's own
            # write against a world that has moved on from it.
            return int(kv.get("chestY", -1)), kv["final"].split(",")
    return None, None


def main():
    seeds = [int(a) for a in sys.argv[1:]] or [4, 5000, 5001]
    p = ore_worker()
    y_ok = y_bad = f_ok = f_bad = err = 0
    unknown = chest_ok = chest_bad = 0
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
                    # Screened against the ore list first: ore is exactly what
                    # this path exists to find, and a face it cannot name is an
                    # UNKNOWN that must not be scored either way.
                    full = face_probe(srv, x, sy, z, SCREEN)
                    if any(b is None for b in full.values()):
                        full = face_probe(srv, x, sy, z, CANDIDATES)
                    gy, gfinal = ask(p, seed, x, z)
                    if gy is None:
                        err += 1
                        continue
                    if gy == sy:
                        y_ok += 1
                    else:
                        y_bad += 1
                        bad.append((seed, x, z, "chestY", gy, sy))
                    # chestY disagreement means the two are describing different
                    # blocks, so their faces are not comparable and scoring them
                    # would manufacture six disagreements out of one.
                    if gy != sy:
                        continue
                    faces_agree = True
                    for i, off in enumerate(FACES):
                        want, got = full.get(off), gfinal[i]
                        if want is None:
                            unknown += 1
                            faces_agree = False
                            continue
                        if want == got:
                            f_ok += 1
                        else:
                            f_bad += 1
                            faces_agree = False
                            bad.append((seed, x, z, f"face{off}", got, want))
                    if faces_agree:
                        chest_ok += 1
                    else:
                        chest_bad += 1
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
    tot_c = chest_ok + chest_bad
    print(f"faces:  {f_ok}/{tot_f} agree"
          f"{'' if not tot_f else f'  ({100*f_ok/tot_f:.0f}%)'}"
          f"   [{unknown} faces the server could not name, {err} errors]")
    print(f"chests: {chest_ok}/{tot_c} agree on ALL SIX faces"
          f"{'' if not tot_c else f'  ({100*chest_ok/tot_c:.0f}%)'}"
          f"   -- this is the number a hunt actually depends on")
    for (s, x, z, what, got, want) in bad[:15]:
        print(f"  seed {s} ({x},{z}) {what}: headless {got}  server {want}")
    if f_bad or y_bad:
        print("\nDISAGREEMENTS ABOVE. Until these are understood the headless "
              "path must not replace the server for ore questions -- a fast "
              "wrong answer is worse than a slow right one.")
    return 0 if not (f_bad or y_bad) else 1


if __name__ == "__main__":
    sys.exit(main())
