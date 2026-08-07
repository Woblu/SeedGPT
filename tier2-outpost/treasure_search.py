#!/usr/bin/env python3
# Orchestrator for the "exposed buried treasure" search (tier 2).
#
# A chest touching air is INCREDIBLY RARE: the buried-treasure structure places
# the chest at the bottom of the sand column, so it is almost always covered by
# sand (or underwater). Only where the beach has ~no sand over stone does the
# block above the chest end up air. cubiomes can't see this (no sand/water), so
# the real MC 1.21.1 worker runs FULL chunk gen (terrain+surface+water) and
# replicates the exact chest placement, then reads the block above the chest.
#
# Tier 1 (find.exe, FIND_HITSTREAM) streams every buried-treasure candidate;
# several JVM workers check them in parallel until `limit` exposed ones are found.
import argparse, json, os, queue, subprocess, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIND = os.path.join(ROOT, "build", "find.exe")
LOCATE = os.path.join(ROOT, "build", "locate.exe")
CP_FILE = os.path.join(HERE, "cp.txt")


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.21")
    ap.add_argument("--range", type=int, default=80_000_000)
    ap.add_argument("--limit", type=int, default=4, help="stop after this many EXPOSED hits")
    ap.add_argument("--workers", type=int, default=6)
    # The block the chest LANDED ON, which becomes its casing. Omit for the
    # original "chest touching air" search. Rates measured over 6646 chests:
    # sand 78%, gravel 18%, dirt 2.7%, stone 0.9%, and the interesting ones --
    # copper_ore, magma_block -- at roughly 0.015% each, so a magma casing costs
    # ~6600 chests of scanning per expected hit.
    ap.add_argument("--casing", default="",
                    help="e.g. magma_block, iron_ore, copper_ore, gravel")
    # Where in the seed walk to start. Without it every run rescans the same
    # seeds, which for a ~0.015% casing means running it again buys nothing.
    ap.add_argument("--offset", type=int, default=0)
    # How far out in each world to look. The old hardcoded 1500 gave 6 treasures
    # per seed; 20000 gives 1431. Only meaningful with --per-seed.
    ap.add_argument("--radius", type=int, default=10000)
    # Check EVERY treasure in a seed rather than the single one find.exe
    # reported. On by default: a casing hunt is bounded by chests examined, and
    # one chest per world was leaving almost every world unexamined.
    ap.add_argument("--per-seed", dest="per_seed", action="store_true", default=True)
    ap.add_argument("--first-only", dest="per_seed", action="store_false")
    args = ap.parse_args()

    q = {"version": args.version, "conditions": [
        {"id": "t", "structure": "buried_treasure",
         "within": min(args.radius, 2000), "of": "origin"}]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False,
                                     dir=os.path.join(ROOT, "build")) as f:
        json.dump(q, f); qpath = f.name

    cp = classpath()
    cand_q = queue.Queue(maxsize=256)
    stop = threading.Event()
    lock = threading.Lock()
    found = [0]
    checked = [0]

    env = dict(os.environ); env["FIND_HITSTREAM"] = "1"
    if args.offset:
        env["FIND_OFFSET"] = str(args.offset)
    finder = subprocess.Popen([FIND, qpath, str(args.range), "16"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, cwd=ROOT, bufsize=1, env=env)

    def all_treasures(seed):
        """Every treasure in the seed within --radius, not just the one find sent.

        find.exe reports m.pos[0] -- the FIRST matched position -- so one HIT is
        one chest, and the pipeline was checking a single treasure per world. For
        a casing at ~0.015% that is the difference between searching a world and
        glancing at it: seed 4 has 6 treasures within 1500 blocks and 1431 within
        20000. Expanding each seed here also lets a worker reuse the seed's
        generator state across chests instead of paying for it once per chest.
        """
        try:
            r = subprocess.run([LOCATE, str(seed), args.version, "buried_treasure",
                                str(args.radius)],
                               capture_output=True, text=True, timeout=120)
            out = []
            for ln in r.stdout.splitlines():
                p = ln.split()
                if len(p) == 2:
                    out.append((int(p[0]), int(p[1])))
            return out
        except Exception:
            return []

    def producer():
        try:
            for line in finder.stdout:
                if stop.is_set():
                    break
                if line.startswith("HIT "):
                    p = line.split()
                    if args.per_seed:
                        for (tx, tz) in all_treasures(p[1]):
                            if stop.is_set():
                                break
                            cand_q.put((p[1], tx, tz))
                    else:
                        cand_q.put((p[1], int(p[2]), int(p[3])))
        except Exception:
            pass
        finally:
            for _ in range(args.workers):
                try: cand_q.put(None)
                except Exception: pass

    # Two predicates over the same pipeline. "exposed" asks what is ABOVE the
    # chest and needs only terrain+surface, so OutpostWorldgen answers it.
    # "casing" asks what the chest LANDED ON -- the block the structure then
    # writes into every air or water face around it -- and ore and magma are
    # placed in decoration, so it needs OreGen, which runs applyBiomeDecoration.
    casing = (args.casing or "").strip()
    if casing and not casing.startswith("minecraft:"):
        casing = "minecraft:" + casing
    mode = ("OreGen", "oreserver", "fill") if casing \
        else ("OutpostWorldgen", "treasure", "above")

    def worker():
        p = subprocess.Popen([java_bin(), "-Xmx2g", "-cp", cp, mode[0], mode[1]],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, cwd=HERE, bufsize=1)
        for line in p.stdout:               # READY
            if line.strip().endswith("READY"):
                break
        try:
            while not stop.is_set():
                item = cand_q.get()
                if item is None:
                    break
                seed, x, z = item
                p.stdin.write(f"{seed} {x} {z}\n"); p.stdin.flush()
                cls, y = None, None
                key = "\t" + mode[2] + "="
                for line in p.stdout:
                    if key in line:
                        kv = dict(kvp.split("=", 1) for kvp in line.strip().split("\t") if "=" in kvp)
                        cls = kv.get(mode[2])
                        y = kv.get("chestY")
                        break
                    if "\terr=" in line:
                        break               # this chest is unknown, not absent
                with lock:
                    checked[0] += 1
                    hit = (cls == casing) if casing else (cls == "air")
                    if hit:
                        found[0] += 1
                        rec = {"seed": seed, "x": x, "z": z}
                        if casing:
                            rec.update({"y": int(y) if y else None, "casing": cls})
                        print(json.dumps(rec), flush=True)
                        if found[0] >= args.limit:
                            stop.set()
        finally:
            try: p.stdin.write("quit\n"); p.stdin.flush()
            except Exception: pass
            p.terminate()

    pt = threading.Thread(target=producer, daemon=True); pt.start()
    ws = [threading.Thread(target=worker, daemon=True) for _ in range(args.workers)]
    for w in ws: w.start()
    for w in ws: w.join()
    stop.set()
    try: finder.terminate()
    except Exception: pass
    try: os.unlink(qpath)
    except Exception: pass
    print(json.dumps({"summary": True, "exposed": found[0], "checked": checked[0],
                      "range": args.range, "casing": args.casing or None,
                      # OreGen's fill agrees with the server on 88% of chests, so
                      # a casing hit is a LEAD to confirm, not a settled result --
                      # and a miss is not evidence of absence.
                      "confirm": bool(args.casing)}), flush=True)


if __name__ == "__main__":
    main()
