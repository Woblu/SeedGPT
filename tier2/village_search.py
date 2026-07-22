#!/usr/bin/env python3
# Orchestrator for the real-worldgen village-smith search (tier 2).
#
# The fast cubiomes finder can't see inside a jigsaw village, so this drives the
# headless Minecraft worldgen worker (VillageWorldgen). Each worker bootstraps
# Minecraft ONCE (~5 s) then streams seeds over stdin; we fan out across several
# workers. For each seed the worker scans a radius, generates every village with
# the real jigsaw assembler, and reports villages with >= N smith buildings.
#
# Usage:  python village_search.py --min 5 --radius 1200 --seeds 300 --workers 4
# Output: one JSON object per matching village on stdout, then a summary line.
import argparse, json, os, queue, re, subprocess, sys, threading

TIER2 = os.path.dirname(os.path.abspath(__file__))
CP_FILE = os.path.join(TIER2, "cp.txt")


def classpath():
    if not os.path.isfile(CP_FILE):
        sys.exit("cp.txt not found -- run: gradle printcp -q | sed 's/^CP://' | tr '\\n' ';' > cp.txt")
    cp = open(CP_FILE, encoding="utf-8").read().strip().rstrip(";")
    return os.path.join(TIER2, "out") + os.pathsep + cp


def worker(cp, seeds, radius, threshold, building, emit, done_evt, stop_evt):
    p = subprocess.Popen(
        ["java", "-cp", cp, "VillageWorldgen", "server"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, cwd=TIER2, bufsize=1,
    )
    # wait for READY
    for line in p.stdout:
        if line.strip() == "READY":
            break
    try:
        while not stop_evt.is_set():
            try:
                seed = seeds.get_nowait()
            except queue.Empty:
                break
            p.stdin.write(f"{seed} {radius} {threshold} {building}\n")
            p.stdin.flush()
            for line in p.stdout:
                line = line.rstrip("\n")
                if line.startswith("HIT\t"):
                    _, s, x, z, biome, n, breakdown = line.split("\t", 6)
                    emit({"seed": int(s), "x": int(x), "z": int(z), "biome": biome,
                          "smiths": int(n), "buildings": breakdown})
                elif line.endswith("\tDONE"):
                    break
    finally:
        try:
            p.stdin.write("quit\n"); p.stdin.flush()
        except Exception:
            pass
        p.terminate()
    done_evt()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=int, default=5, help="minimum matching buildings in one village")
    ap.add_argument("--building", default="smith",
                    help="building type: smith (group) | toolsmith weaponsmith armorer library "
                         "cartographer mason fletcher butcher shepherd fisher tannery temple farm animal_pen stable")
    ap.add_argument("--radius", type=int, default=1200, help="search radius per seed (blocks)")
    ap.add_argument("--seeds", type=int, default=300, help="how many world seeds to scan")
    ap.add_argument("--start", type=int, default=1, help="first seed")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--limit", type=int, default=0, help="stop after this many hits (0 = no limit)")
    args = ap.parse_args()

    cp = classpath()
    seeds = queue.Queue()
    for s in range(args.start, args.start + args.seeds):
        seeds.put(s)

    lock = threading.Lock()
    stop_evt = threading.Event()
    hits = [0]

    def emit(hit):
        with lock:
            print(json.dumps(hit), flush=True)
            hits[0] += 1
            if args.limit and hits[0] >= args.limit:
                stop_evt.set()

    remaining = [args.workers]
    all_done = threading.Event()

    def done():
        with lock:
            remaining[0] -= 1
            if remaining[0] == 0:
                all_done.set()

    building = re.sub(r"[^a-z_]", "", args.building.lower()) or "smith"
    threads = [threading.Thread(target=worker, args=(cp, seeds, args.radius, args.min, building, emit, done, stop_evt), daemon=True)
               for _ in range(args.workers)]
    for t in threads:
        t.start()
    all_done.wait()
    print(json.dumps({"summary": True, "hits": hits[0], "seeds_scanned": args.seeds,
                      "min": args.min, "building": building}), flush=True)


if __name__ == "__main__":
    main()
