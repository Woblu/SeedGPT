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
    args = ap.parse_args()

    q = {"version": args.version, "conditions": [
        {"id": "t", "structure": "buried_treasure", "within": 1500, "of": "origin"}]}
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
    finder = subprocess.Popen([FIND, qpath, str(args.range), "16"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, cwd=ROOT, bufsize=1, env=env)

    def producer():
        try:
            for line in finder.stdout:
                if stop.is_set():
                    break
                if line.startswith("HIT "):
                    p = line.split()
                    cand_q.put((p[1], int(p[2]), int(p[3])))
        except Exception:
            pass
        finally:
            for _ in range(args.workers):
                try: cand_q.put(None)
                except Exception: pass

    def worker():
        p = subprocess.Popen([java_bin(), "-cp", cp, "OutpostWorldgen", "treasure"],
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
                cls = None
                for line in p.stdout:
                    if "\tabove=" in line:
                        kv = dict(kvp.split("=", 1) for kvp in line.strip().split("\t") if "=" in kvp)
                        cls = kv.get("above")
                        break
                with lock:
                    checked[0] += 1
                    if cls == "air":
                        found[0] += 1
                        print(json.dumps({"seed": seed, "x": x, "z": z}), flush=True)
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
                      "range": args.range}), flush=True)


if __name__ == "__main__":
    main()
