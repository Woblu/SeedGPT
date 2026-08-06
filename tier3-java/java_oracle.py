#!/usr/bin/env python3
"""A real Java 1.21.1 world as a block oracle.

Everything else in this project is checked against something independent: a JDK
reference, cubiomes, a headless generator, Bedrock Dedicated Server. Java
*features* -- cacti, flowers, anything placed after terrain -- had no such
source, because decoration needs a WorldGenLevel that the headless backend
cannot build. This runs the actual server instead and reads blocks out of the
world it generates.

    from java_oracle import JavaServer
    with JavaServer(seed=12345) as srv:
        print(srv.cactus_height(x, z))

THE TRAP THIS IS BUILT AROUND. `/execute if block` on a chunk that is not loaded
fails exactly like a block that is not there. Folding those together is how the
Bedrock work reported "no structure here" three separate times for chunks it had
simply never loaded. So every probe is preceded by a positive control: y=-64 is
bedrock in every overworld column, so if the control fails the chunk is not
loaded and the answer is None -- unknown -- rather than False.

One server, always cleaned up. Spawning a fleet of these would eat the machine.
"""
import atexit
import os
import queue
import re
import socket
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
JAR = os.path.join(HERE, "server.jar")
JAVA = os.environ.get("JAVA_HOME", r"C:\Program Files\Java\jdk-21") + r"\bin\java.exe"

_LIVE = []


def _kill_all():
    for s in list(_LIVE):
        try:
            s.stop()
        except Exception:
            pass


atexit.register(_kill_all)

LINE = re.compile(r"^\[[\d:]+\] \[(?P<who>[^\]]+)\]: (?P<msg>.*)$")


class JavaServer:
    def __init__(self, seed, world="world", ram="2G", quiet=True, fresh=False):
        # `fresh` deletes any world left from a previous run first. Anything that
        # probes destructively -- /fill ... replace, the only cheap way to count
        # blocks -- permanently alters the world, and a later run against the
        # same directory then sees an empty desert and blames the simulation.
        # That cost an hour once; it should not cost anyone a second.
        self.seed = seed
        self.dir = os.path.join(HERE, f"run-{seed}")
        if fresh:
            shutil.rmtree(self.dir, ignore_errors=True)
        self.world = world
        self.ram = ram
        self.quiet = quiet
        self.proc = None
        self.q = queue.Queue()
        self._n = 0
        self._forced = set()
        self.port = self._free_port()

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    @staticmethod
    def _free_port():
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        return port

    def _prepare(self):
        os.makedirs(self.dir, exist_ok=True)
        # Accepting the EULA is a deliberate act, recorded here rather than
        # buried, because it is what makes running this legal.
        with open(os.path.join(self.dir, "eula.txt"), "w") as f:
            f.write("eula=true\n")
        props = {
            "level-seed": str(self.seed),
            "level-name": self.world,
            "level-type": "minecraft:normal",
            "online-mode": "false",
            "spawn-protection": "0",
            # The watchdog kills the server mid-probe on a slow chunk gen, and
            # a killed server looks exactly like a world with nothing in it.
            "max-tick-time": "-1",
            # Nothing here needs a populated world beyond the chunks we ask for.
            "view-distance": "2",
            "simulation-distance": "2",
            "spawn-npcs": "false",
            "spawn-animals": "false",
            "spawn-monsters": "false",
            "sync-chunk-writes": "false",
            "enable-jmx-monitoring": "false",
            "allow-nether": "false",
            # Its own port per instance. The default 25565 lingers in TIME_WAIT
            # after a stop, so back-to-back worlds -- exactly what a hunt does --
            # hit "FAILED TO BIND TO PORT" and the run quietly checks a fraction
            # of what it was asked to. That failed 31 seeds out of 40 once, and
            # still printed a tidy "0 found".
            "server-port": str(self.port),
        }
        with open(os.path.join(self.dir, "server.properties"), "w") as f:
            for k, v in props.items():
                f.write(f"{k}={v}\n")

    def start(self, timeout=300, tries=3):
        if not os.path.exists(JAR):
            raise RuntimeError(f"missing {JAR} -- see tier3-java/README.md")
        # A port can still be taken between choosing it and binding it, so a
        # bind failure retries on a new one rather than killing the whole run.
        for attempt in range(tries):
            try:
                return self._start_once(timeout)
            except RuntimeError as e:
                if "FAILED TO BIND" not in str(e) or attempt == tries - 1:
                    raise
                self.stop()
                self.port = self._free_port()
        raise RuntimeError("unreachable")

    def _start_once(self, timeout):
        self._prepare()
        self.proc = subprocess.Popen(
            [JAVA, f"-Xmx{self.ram}", "-jar", JAR, "nogui"],
            cwd=self.dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        _LIVE.append(self)
        threading.Thread(target=self._pump, daemon=True).start()

        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline(timeout=deadline - time.time())
            if line is None:
                break
            if "Done (" in line:
                return self
            if "FAILED TO BIND" in line or "Failed to start" in line:
                raise RuntimeError(f"server failed to start: {line}")
        raise RuntimeError("server did not report Done within timeout")

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.write("stop\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=60)
        except Exception:
            self.proc.kill()
        finally:
            if self in _LIVE:
                _LIVE.remove(self)
            self.proc = None

    def cleanup(self):
        """Delete the generated world. Each one is tens of megabytes."""
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    # -- plumbing ----------------------------------------------------------

    def _pump(self):
        for line in self.proc.stdout:
            self.q.put(line.rstrip("\n"))
            if not self.quiet:
                sys.stderr.write(line)
        self.q.put(None)

    def _readline(self, timeout=120):
        try:
            return self.q.get(timeout=max(timeout, 1))
        except queue.Empty:
            return None

    def _send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def run(self, commands):
        """Send commands and collect everything printed before they finish.

        Console commands all execute in the same tick, so a batch costs about
        what one costs. Synchronisation is a trailing `say`: when its echo comes
        back, everything before it has run.
        """
        if isinstance(commands, str):
            commands = [commands]
        self._n += 1
        tag = f"SYNCPOINT{self._n}"
        for c in commands:
            self._send(c)
        self._send(f"say {tag}")
        out = []
        while True:
            line = self._readline()
            if line is None:
                raise RuntimeError("server died mid-command")
            if tag in line:
                return out
            m = LINE.match(line)
            out.append(m.group("msg") if m else line)

    # -- probing -----------------------------------------------------------

    def forceload(self, x, z, radius_chunks=1):
        """Keep the chunks around a block position loaded and generated."""
        cx, cz = x >> 4, z >> 4
        key = (cx, cz, radius_chunks)
        if key in self._forced:
            return
        r = radius_chunks * 16
        self.run(f"forceload add {x - r} {z - r} {x + r} {z + r}")
        self._forced.add(key)

    def loaded(self, x, z):
        """Is this column actually generated? y=-64 is bedrock in every one."""
        out = self.run(f"execute if block {x} -64 {z} minecraft:bedrock run say LOADED")
        return any("LOADED" in l for l in out)

    def blocks(self, x, z, y0, y1, block):
        """Which y in [y0, y1] hold `block`? None if the chunk is not loaded.

        None and empty-set mean very different things and are never merged --
        that conflation is the bug this class exists to avoid.
        """
        self.forceload(x, z)
        if not self.loaded(x, z):
            return None
        cmds = [f"execute if block {x} {y} {z} {block} run say Y{y}"
                for y in range(y0, y1 + 1)]
        out = self.run(cmds)
        hits = set()
        for line in out:
            m = re.search(r"\bY(-?\d+)\b", line)
            if m:
                hits.add(int(m.group(1)))
        return hits

    def cactus_height(self, x, z, y0=50, y1=140):
        """Tallest run of cactus in this column, and where it starts.

        Returns (height, base_y), or (0, None) for no cactus, or None if the
        chunk could not be loaded.
        """
        ys = self.blocks(x, z, y0, y1, "minecraft:cactus")
        if ys is None:
            return None
        if not ys:
            return (0, None)
        best, best_base = 0, None
        for y in sorted(ys):
            if y - 1 in ys:
                continue                      # not the bottom of its run
            run = 0
            while y + run in ys:
                run += 1
            if run > best:
                best, best_base = run, y
        return (best, best_base)

    # -- identifying and counting -----------------------------------------

    def identify(self, x, y, z, candidates):
        """Which of `candidates` is at this position? A list, normally 0 or 1.

        `/execute if block` can only ASK about a block, never name one, and
        `/data get block` only speaks for block entities. So identification here
        means testing a list -- which makes "not on the list" a real answer,
        distinct from "nothing there". Callers get the empty list for that and
        must not read it as air.

        None if the chunk is not loaded, which is not the same as no match.
        """
        self.forceload(x, z)
        if not self.loaded(x, z):
            return None
        cmds = [f"execute if block {x} {y} {z} {b} run say HIT{i}"
                for i, b in enumerate(candidates)]
        out = self.run(cmds)
        found = []
        for line in out:
            m = re.search(r"\bHIT(\d+)\b", line)
            if m:
                found.append(candidates[int(m.group(1))])
        return found

    def count_block(self, x0, y0, z0, x1, y1, z1, block, _limit=32):
        """How many `block` are in this box? DESTRUCTIVE -- they become air.

        `/fill ... replace` is the only cheap way to count: probing a village-
        sized volume one position at a time is hundreds of thousands of
        commands, while this is a few dozen. The price is that the blocks are
        gone afterwards, so a run's world is spent once it has been counted --
        which is fine for a throwaway `run-<seed>/`, and ruinous if a later
        probe reuses it. Count last, or pass fresh=True.

        Fill is capped at 32768 blocks per command, so the box is tiled.
        """
        self.forceload((x0 + x1) // 2, (z0 + z1) // 2,
                       radius_chunks=max(2, (max(x1 - x0, z1 - z0) // 16) + 2))
        cmds = []
        for bx in range(x0, x1 + 1, _limit):
            for by in range(y0, y1 + 1, _limit):
                for bz in range(z0, z1 + 1, _limit):
                    cmds.append(
                        f"fill {bx} {by} {bz} "
                        f"{min(bx+_limit-1, x1)} {min(by+_limit-1, y1)} {min(bz+_limit-1, z1)} "
                        f"minecraft:air replace {block}")
        total = 0
        for i in range(0, len(cmds), 200):          # keep batches readable
            for line in self.run(cmds[i:i+200]):
                m = re.search(r"Successfully filled (\d+) block", line)
                if m:
                    total += int(m.group(1))
        return total


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 12345
    t0 = time.time()
    with JavaServer(seed, quiet=True) as srv:
        print(f"started in {time.time() - t0:.1f}s")
        t1 = time.time()
        print("spawn area loaded:", srv.loaded(0, 0))
        print(f"one probe: {time.time() - t1:.2f}s")
