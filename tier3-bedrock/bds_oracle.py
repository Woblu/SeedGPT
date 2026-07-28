#!/usr/bin/env python3
"""Ground truth for BEDROCK world generation, straight from the real server.

The finder's whole premise is that a result is verifiable. For Java that means
an independent JDK reference; there is no such thing for Bedrock -- the
generator is closed-source C++ -- so the only honest oracle is Bedrock
Dedicated Server itself. That matters more here than it did for Java: Bedrock's
interesting seeds come from its quirks, and a clean-room reimplementation
reproduces the spec, not the quirks.

The trick that makes this cheap is `/locate`. BDS answers

    locate structure village 0 0
    locate biome jungle 0 0

from the console with exact coordinates, without loading chunks, writing
LevelDB, or running a scripting API. One server run per seed, a handful of
queries, parse stdout.

    python bds_oracle.py --seed 12345 --structures village,ruined_portal
    python bds_oracle.py --seeds 1,2,3 --biomes jungle --json out.json

A seed change needs a fresh world, so each seed costs one server start
(~10-20s). That is the budget: tens of seeds per minute, not thousands.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from queue import Queue, Empty

ROOT = Path(__file__).parent
SERVER = ROOT / "server"
EXE = SERVER / "bedrock_server.exe"
LEVEL = "seedprobe"

# BDS phrases its answers in prose, and the wording has shifted across versions,
# so match loosely and pull the numbers out rather than pinning an exact string.
RE_FOUND = re.compile(
    r"nearest\s+(?:(?:structure|biome)\s+)?(?P<what>[\w:. ]+?)\s+is\s+at\s+"
    r"(?:block\s+)?(?P<x>-?\d+)\s*,\s*(?P<y>-?\d+|~|\(y\?\))\s*,\s*(?P<z>-?\d+)",
    re.I)
RE_NONE = re.compile(r"could not find|no .* found|unable to find", re.I)
RE_READY = re.compile(r"Server started", re.I)


class Bds:
    """One server instance on one seed, driven through stdin/stdout."""

    def __init__(self, seed, verbose=False, boot_timeout=180):
        self.seed = str(seed)
        self.verbose = verbose
        self.boot_timeout = boot_timeout
        self.proc = None
        self.lines = Queue()

    # -- lifecycle ---------------------------------------------------------
    def __enter__(self):
        self._write_properties()
        self._wipe_world()
        self.proc = subprocess.Popen(
            [str(EXE)], cwd=str(SERVER), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()
        if not self._wait_for(RE_READY, self.boot_timeout):
            self.close()
            raise RuntimeError(f"server did not start within {self.boot_timeout}s")
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.write("stop\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.proc.kill()
        self.proc = None

    # -- plumbing ----------------------------------------------------------
    def _pump(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.verbose:
                print("   |", line, file=sys.stderr)
            self.lines.put(line)

    def _wait_for(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=0.5)
            except Empty:
                if self.proc.poll() is not None:
                    return False
                continue
            if pattern.search(line):
                return True
        return False

    def _write_properties(self):
        props = {
            "server-name": "seedprobe",
            "level-name": LEVEL,
            "level-seed": self.seed,
            "gamemode": "creative",
            "difficulty": "peaceful",
            "allow-cheats": "true",
            "max-players": "1",
            "online-mode": "false",
            "server-port": "19140",
            "server-portv6": "19141",
            "player-idle-timeout": "0",
            "view-distance": "4",       # nothing renders; keep generation small
            "tick-distance": "4",
        }
        (SERVER / "server.properties").write_text(
            "\n".join(f"{k}={v}" for k, v in props.items()) + "\n",
            encoding="utf-8")

    def _wipe_world(self):
        # The seed only takes effect on a NEW world -- reusing the directory
        # would silently answer every query from the first seed ever run.
        shutil.rmtree(SERVER / "worlds" / LEVEL, ignore_errors=True)

    # -- queries -----------------------------------------------------------
    # A block probe answers with "The block at ..." / "Successfully found the
    # block", which matches none of the locate patterns -- without a stop
    # condition of its own every probe would pay the idle timeout below, and a
    # few hundred probes would take minutes instead of seconds.
    RE_BLOCK = re.compile(r"The block at|Successfully found the block|out of range|outside of the world", re.I)

    def command(self, cmd, timeout=60, stop=None):
        """Run one console command; return the lines it produced.

        `stop` is an extra pattern that ends the wait as soon as it appears.
        """
        # Drain anything still queued so a slow earlier answer is not mistaken
        # for this command's.
        while True:
            try:
                self.lines.get_nowait()
            except Empty:
                break
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        out, end = [], time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=0.5)
            except Empty:
                if out:
                    break               # answered, and nothing more is coming
                continue
            out.append(line)
            if RE_FOUND.search(line) or RE_NONE.search(line):
                break
            if stop is not None and stop.search(line):
                break
        return out

    def locate(self, kind, name):
        """kind is "structure" or "biome". Returns (x, z) or None.

        Console commands run from the world origin, which is what we want: the
        Java side is asked the same "nearest to (0,0)" question. `locate biome`
        insists on the namespaced id; `locate structure` accepts either.
        """
        if kind == "biome" and ":" not in name:
            name = "minecraft:" + name
        lines = self.command(f"locate {kind} {name}")
        for line in lines:
            m = RE_FOUND.search(line)
            if m:
                return int(m.group("x")), int(m.group("z"))
            if RE_NONE.search(line):
                return None
        return {"unparsed": lines} if lines else None


def probe(seed, structures, biomes, verbose=False):
    result = {"seed": seed, "structures": {}, "biomes": {}}
    with Bds(seed, verbose=verbose) as bds:
        for s in structures:
            result["structures"][s] = bds.locate("structure", s)
        for b in biomes:
            result["biomes"][b] = bds.locate("biome", b)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, help="one seed to probe")
    ap.add_argument("--seeds", help="comma-separated seeds")
    ap.add_argument("--structures", default="village",
                    help="comma-separated structure ids for /locate structure")
    ap.add_argument("--biomes", default="", help="comma-separated biome ids")
    ap.add_argument("--json", help="write results here as JSON lines")
    ap.add_argument("--verbose", action="store_true", help="echo the server log")
    a = ap.parse_args()

    if not EXE.exists():
        sys.exit(f"Bedrock Dedicated Server not found at {EXE}.\n"
                 f"Download bedrock-server-<version>.zip and unzip it into {SERVER}.")

    seeds = []
    if a.seed is not None:
        seeds.append(a.seed)
    if a.seeds:
        seeds += [int(s) for s in a.seeds.split(",") if s.strip()]
    if not seeds:
        seeds = [1]
    structures = [s for s in a.structures.split(",") if s.strip()]
    biomes = [b for b in a.biomes.split(",") if b.strip()]

    out = open(a.json, "w", encoding="utf-8") if a.json else None
    for seed in seeds:
        t0 = time.time()
        r = probe(seed, structures, biomes, a.verbose)
        r["seconds"] = round(time.time() - t0, 1)
        print(json.dumps(r))
        if out:
            out.write(json.dumps(r) + "\n"); out.flush()
    if out:
        out.close()


if __name__ == "__main__":
    main()
