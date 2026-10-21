#!/usr/bin/env python3
"""Local web UI for the seed finder.

    python serve.py            # then open http://127.0.0.1:8777

Binds to loopback only. The search itself is a native binary, so this cannot be
a hosted page -- the server exists to bridge the browser to build/find.exe.

Endpoints (all POST bodies are JSON):
    GET  /                -> the UI
    GET  /api/vocab?v=..  -> valid structures/biomes for a version, with dimension
    POST /api/plan        -> parse + plan only, no search (fast, always safe)
    POST /api/explain     -> sampled rarity estimate
    POST /api/search      -> the real search (batch)
    POST /api/stream      -> the real search, streamed: {"t":seed} heartbeats
                             while scanning, then {"done":{...}} -- lets the UI
                             show the seeds scroll past live
    POST /api/describe    -> what is actually in a given seed
    GET  /api/map?...     -> biome map PNG for a seed
    GET  /api/assets      -> lists which structure/item icons the user has added
    GET  /assets/<path>   -> static image files (user-provided graphics)
    POST /api/ask         -> natural language -> query JSON (needs ANTHROPIC_API_KEY)
    POST /api/gemini      -> natural language -> query JSON. "provider":"gemini"
                             (default, key from the body or GEMINI_API_KEY) or
                             "compatible" for any OpenAI chat-completions API --
                             Groq, OpenAI, OpenRouter, DeepSeek, local Ollama --
                             with "base" and "model" (key: body or AI_API_KEY).
                             The route keeps its name so saved links still work.
    POST /api/hunt        -> open-ended search: batch after batch, advancing
                             FIND_OFFSET, until satisfied or stopped. Returns a
                             job id immediately; poll /api/hunt/status, end with
                             /api/hunt/stop. The batch /api/search can only ever
                             answer "not in the first N seeds".
    POST /api/confirmcasing -> settle ONE casing lead against a real server.
                             The fast path is 88% right, so its hits are leads;
                             this is what turns one into a result.
    POST /api/fortressoverlap -> rank seeds by how far neighbouring nether
                             fortresses GROW THROUGH each other (intersecting
                             piece pairs), not by how close their starts are.
    POST /api/casingtreasure -> buried treasure ENCASED in a given block (magma,
                             ore...). The main search cannot express this: it
                             runs on cubiomes, which has no surface rules and no
                             decoration, so the sand column and every ore and
                             magma block are simply absent from that engine.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs, quote

ROOT = Path(__file__).parent
ASSETS = ROOT / "assets"
BUILD = ROOT / "build"
UI = ROOT / "ui" / "index.html"
PORT = int(os.environ.get("SEED_UI_PORT", "8777"))

# Guard against a stray query pinning all cores for minutes.
TIMEOUTS = {"plan": 15, "explain": 90, "search": 180, "describe": 30, "map": 60,
            "villagesmiths": 900}
VERSION_RE = re.compile(r"^[0-9][0-9A-Za-z._-]{0,15}$")
# Longest time budget a single search may request. Generous on purpose -- the
# whole point is being able to leave it running -- but bounded so a typo cannot
# pin every core until the server is restarted.
MAX_SEARCH_SECONDS = 6 * 60 * 60


class Failure(Exception):
    """A user-facing error: bad input, missing binary, tool failure."""


def tool(name: str) -> Path:
    exe = BUILD / f"{name}.exe"
    if not exe.exists():
        exe = BUILD / name          # non-Windows
    if not exe.exists():
        raise Failure(f"{exe.name} not built. Run: ./build.sh tools/{name}.c")
    return exe


# Running searches, keyed by the job id the browser sends. A streaming search
# blocks a handler thread inside find.exe, so "stop" cannot be a local decision:
# the browser has to be able to reach in and kill the process. Aborting the
# fetch alone is not enough -- the pipe only breaks on the next heartbeat write,
# and a slow query can sit for seconds between those.
JOBS = {}
JOBS_LOCK = threading.Lock()


def job_register(job, proc):
    if not job:
        return
    with JOBS_LOCK:
        JOBS[str(job)] = proc


def job_done(job):
    if not job:
        return
    with JOBS_LOCK:
        JOBS.pop(str(job), None)


def api_cancel(body) -> dict:
    """Kill a running search by job id. Idempotent: cancelling a job that has
    already finished is a no-op, not an error -- the browser cannot know which
    it is, and a spurious error on a Stop button is worse than silence."""
    job = str(body.get("job", "")).strip()
    with JOBS_LOCK:
        proc = JOBS.pop(job, None)
    if proc is None:
        return {"cancelled": False}
    # Mark it before killing: on Windows terminate() sets exit code 1, which is
    # indistinguishable from a normal failure, so the stream handler needs an
    # explicit flag to know the empty result was deliberate.
    proc.sc_cancelled = True
    _kill_tree(proc)
    return {"cancelled": True}


def _kill_tree(proc):
    """Kill the process AND its children.

    A casing hunt is python -> treasure_search.py -> find.exe. Terminating only
    the process we registered leaves find.exe running: it then holds an open
    handle on build/find.exe, and the next build fails with "permission denied"
    -- which is how a stopped hunt broke the test suite rather than the hunt.
    """
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True, timeout=30)
        else:
            proc.terminate()
    except Exception:
        try:
            proc.terminate()
        except OSError:
            pass


def find_env(extra=None):
    """Environment for running find.exe: always ask for @JSON hit lines.

    The JSON is built from the engine's own result structs, so a hit cannot
    lose a field because its human-readable wording changed -- which is how
    stronghold rows never reached the browser and overlap partners stopped
    doing so. The text output is unchanged for people and other scripts.
    """
    env = dict(os.environ)
    env["FIND_JSON"] = "1"
    if extra:
        env.update(extra)
    return env


def parse_hits(out: str) -> list:
    """Hits from find output: the @JSON lines when present (any current
    find.exe), else the regex parser below (an old binary)."""
    hits = []
    for line in out.splitlines():
        if line.startswith("@JSON "):
            try:
                hits.append(json.loads(line[6:]))
            except ValueError:
                pass
    return hits if hits else parse_seeds(out)


def human(out: str) -> str:
    """find output as a person should see it: without the @JSON twins."""
    return "\n".join(l for l in out.splitlines() if not l.startswith("@JSON "))


def run(args, timeout, stdin=None, env=None):
    """Run a tool. No shell, so query text can never become a command.

    On timeout the whole process TREE is killed. subprocess.run kills only the
    direct child, and several tools are python -> JVM or python -> Minecraft
    server chains: the python died and the server it started kept running,
    holding gigabytes and a port, with nothing left that knew about it.
    """
    p = subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, stdin=subprocess.PIPE if stdin else None,
                         text=True, encoding="utf-8", errors="replace", cwd=str(ROOT),
                         env=env)
    try:
        out, err = p.communicate(input=stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        _kill_tree(p)
        try:
            p.communicate(timeout=10)
        except Exception:  # noqa: BLE001
            pass
        raise Failure(f"timed out after {timeout}s -- try a smaller range")
    return p.returncode, out, err


def check_version(v: str) -> str:
    if not VERSION_RE.match(v or ""):
        raise Failure(f"bad version {v!r}")
    return v


def java_seed(text) -> str:
    """The world seed Minecraft would use for what the player typed, as text.

    Java's rule (WorldOptions.parseSeed): trim; if it parses as a signed 64-bit
    long that is the seed, otherwise the seed is the text's String.hashCode().
    The tools parse with strtoll, which CLAMPS an out-of-range number -- so
    "18446744073709551615" quietly became 9223372036854775807, a different world
    from the one the game makes (it hashes the text). Normalising here means
    every tool receives a plain in-range long. Mirrors src/seed.h.
    """
    s = str(text).strip()
    if not s:
        raise Failure("enter a seed")
    if re.fullmatch(r"[+-]?\d+", s):
        v = int(s)
        if -(1 << 63) <= v < (1 << 63):
            return str(v)
    # String.hashCode runs over UTF-16 code units, surrogate pairs included.
    data, h = s.encode("utf-16-le"), 0
    for i in range(0, len(data), 2):
        h = (31 * h + (data[i] | (data[i + 1] << 8))) & 0xFFFFFFFF
    return str(h - (1 << 32) if h >= (1 << 31) else h)


def clamp_threads(body, default=16) -> int:
    """Worker threads for a native search: at least 1, at most this machine.

    These were passed through unclamped -- 0 divided by zero inside find.exe,
    and anything above 64 broke its thread wait -- and a page could ask for
    more threads than the CPU has on a machine someone is using.
    """
    try:
        n = int(body.get("threads", default))
    except (TypeError, ValueError):
        n = default
    return max(1, min(n, os.cpu_count() or 16, 64))


QUERY_DIR = BUILD / "tmp" / "queries"


def write_query(body) -> Path:
    """Persist the query where find.exe can read it -- ONE FILE PER REQUEST.

    This used to be a single queries/_ui.json shared by everything. A hunt
    re-reads its query every batch, so clicking Plan with a different query in
    the middle of a hunt silently changed what the hunt was searching for, and
    two searches running at once raced on the same file. Callers remove theirs
    with drop_query(); a hunt keeps its own until it ends.

    The last query is still mirrored to queries/_ui.json, never read back, so it
    stays a single command away for running find.exe by hand.
    """
    q = body.get("query")
    if not isinstance(q, dict):
        raise Failure("missing 'query' object")
    check_version(str(q.get("version", "")))
    text = json.dumps(q, indent=2)
    QUERY_DIR.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix="q_", suffix=".json", dir=str(QUERY_DIR))
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
    try:
        last = ROOT / "queries" / "_ui.json"
        last.parent.mkdir(exist_ok=True)
        last.write_text(text, encoding="utf-8")
    except OSError:
        pass
    return Path(name)


def drop_query(path):
    try:
        if path and Path(path).parent == QUERY_DIR:
            Path(path).unlink()
    except OSError:
        pass


# ------------------------------------------------------------------ parsing
# find.exe is built for humans reading a terminal; the UI wants structure.
# These parsers are deliberately tolerant: anything unrecognised falls through
# to `raw`, which the UI always shows, so a format change degrades rather than
# silently dropping information.

def parse_plan(out: str) -> dict:
    plan = {"pass1": [], "pass2": [], "conditions": []}
    section = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("pass 1"):
            section = "pass1"; continue
        if s.startswith("pass 2"):
            section = "pass2"; continue
        if s.startswith("est."):
            section = None
            m = re.search(r"([\d.]+) ns/seed", s)
            if m:
                plan["est_ns"] = float(m.group(1))
            continue
        m = re.match(r"^\[(\w+)\]\s+(.*)$", s)
        if m:
            plan["conditions"].append({"id": m.group(1), "desc": m.group(2)})
            continue
        m = re.match(r"^\d+\.\s+(.*?)\s+~([\d.]+) ns$", s)
        if m and section:
            plan[section].append({"desc": m.group(1).strip(), "ns": float(m.group(2))})
    return plan


def parse_funnel(out: str) -> dict:
    f = {}
    # Either funnel shape: the two-pass one ("structure seeds") or the direct
    # world-seed walk a query with no structure conditions uses.
    m = re.search(r"scanned\s*:\s*(\d+) (structure|world) seeds in ([\d.]+)s", out)
    if m:
        f["scanned"], f["seconds"] = int(m.group(1)), float(m.group(3))
        f["whole_seed"] = m.group(2) == "world"
    m = re.search(r"matched\s*:\s*(\d+)", out)
    if m:
        f["pass2"] = int(m.group(1))
    m = re.search(r"pass1 \(48b\):\s*(\d+)\s*\(([\d.]+)% survive\)", out)
    if m:
        f["pass1"], f["pass1_pct"] = int(m.group(1)), float(m.group(2))
    m = re.search(r"pass2 \(64b\):\s*(\d+)", out)
    if m:
        f["pass2"] = int(m.group(1))
    m = re.search(r"throughput\s*:\s*([\d.]+) M structure-seeds/s", out)
    if m:
        f["mseeds_s"] = float(m.group(1))
    f["too_loose"] = "Tighten" in out
    # A budgeted run that ran out of time scanned fewer seeds than asked for,
    # and any "best of N" claim has to say so.
    f["timed_out"] = "time budget expired" in out
    return f


def parse_notes(out: str) -> list:
    """NOTE blocks: structures the engine cannot actually verify."""
    notes, cur = [], None
    for line in out.splitlines():
        if line.startswith("NOTE:"):
            cur = [line[5:].strip()]
            notes.append(cur)
        elif cur is not None and line.startswith("      "):
            cur.append(line.strip())
        elif cur is not None:
            cur = None
    return [" ".join(n) for n in notes]


def parse_seeds(out: str) -> list:
    seeds, cur = [], None
    for line in out.splitlines():
        # A ranked (leaderboard) run appends the score: "SEED n   relief = 173".
        m = re.match(r"^SEED (-?\d+)(?:\s{2,}(.+?) = (-?\d+))?\s*$", line)
        if m:
            cur = {"seed": m.group(1), "places": []}
            if m.group(2):
                cur["score"] = int(m.group(3))
                cur["metric"] = m.group(2)
            seeds.append(cur)
            continue
        # find.exe now labels the reference: "from spawn" or "from origin".
        # Matching only one silently drops every coordinate for the other.
        m = re.match(r"^\s+\(spawn\)\s+x=\s*(-?\d+) z=\s*(-?\d+)", line)
        if m and cur:
            cur["spawn"] = {"x": int(m.group(1)), "z": int(m.group(2))}
            continue
        m = re.match(r"^\s+\(end portal\)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+)/", line)
        if m and cur:
            cur["portal"] = {"x": int(m.group(1)), "z": int(m.group(2)),
                             "eyes": int(m.group(3))}
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) (\w+)( exposed)? ore"
                     r" within (\d+)(?:, biggest vein (\d+))?", line)
        if m and cur:
            o = {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "material": m.group(5),
                 "within": int(m.group(7))}
            if m.group(6):
                o["exposed"] = True
            if m.group(8):
                o["vein"] = int(m.group(8))
            cur.setdefault("ore", []).append(o)
            continue
        # Overlap prints the pair across two lines: the first structure with the
        # condition id, then the second indented with no id.
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+([a-z_]+)\s*$", line)
        if m and cur:
            cur.setdefault("overlap", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "structure": m.group(4)})
            continue
        # Both wordings: find once said "N blocks away, M overlap" and now says
        # "N blocks origin-to-origin (...), M blocks of footprint overlap".
        m = re.match(r"^\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+([a-z_]+), (\d+) blocks "
                     r"(?:away|origin-to-origin[^,]*), (\d+) (?:blocks of footprint )?overlap",
                     line)
        if m and cur and cur.get("overlap"):
            cur["overlap"][-1].update(
                {"x2": int(m.group(1)), "z2": int(m.group(2)), "structure2": m.group(3),
                 "gap": int(m.group(4)), "area": int(m.group(5))})
            continue
        # A negated condition ("not": true) has no position: it reports what
        # was checked and from where.
        m = re.match(r"^\s+(\w+)\s+none within (\d+) of (\S+)\s*$", line)
        if m and cur:
            cur.setdefault("absent", []).append(
                {"id": m.group(1), "within": int(m.group(2)), "of": m.group(3)})
            continue
        # A stronghold reports its roof, then (indented, no id) the teleport to
        # its shallowest piece. Unparsed, these rows fell through to "raw" and
        # a stronghold hit showed no coordinates at all.
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+stronghold, roof y=(-?\d+), (.+)$", line)
        if m and cur:
            cur.setdefault("stronghold", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "roof": int(m.group(4)), "state": m.group(5).strip()})
            continue
        m = re.match(r"^\s+/tp (-?\d+) (-?\d+) (-?\d+) -- the shallowest part, (\d+) "
                     r"block\(s\) of ground over it, (.+)$", line)
        if m and cur and cur.get("stronghold"):
            sh = cur["stronghold"][-1]
            sh["tp"] = {"x": int(m.group(1)), "y": int(m.group(2)), "z": int(m.group(3))}
            sh["depth"] = int(m.group(4))
            sh["where"] = m.group(5).strip()
            continue
        if cur and cur.get("stronghold") and "NOTE: tall stronghold" in line:
            cur["stronghold"][-1]["tall"] = True
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+)-block cactus, base y=(-?\d+)", line)
        if m and cur:
            cur.setdefault("cactus", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "height": int(m.group(4)), "base_y": int(m.group(5))})
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) slime chunks within (\d+)", line)
        if m and cur:
            cur.setdefault("slime", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "within": int(m.group(5))})
            continue
        # The leading "~" marks the smoothed estimate; exact terrain prints the
        # number bare. Dropping that distinction would present an estimate as a
        # measurement, so it is carried through.
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(~?)(-?\d+) peak"
                     r"(?:, ~?(-?\d+) relief)? within (\d+)", line)
        if m and cur:
            h = {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "peak": int(m.group(5)), "within": int(m.group(7)),
                 "exact": m.group(4) != "~"}
            if m.group(6) is not None:
                h["relief"] = int(m.group(6))
            cur.setdefault("height", []).append(h)
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+island, (\d+)% ocean within (\d+)", line)
        if m and cur:
            cur.setdefault("island", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "pct": int(m.group(4)), "within": int(m.group(5))})
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+)% (\w+) within (\d+)", line)
        if m and cur:
            cur.setdefault("area", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "pct": int(m.group(4)), "biome": m.group(5), "within": int(m.group(6))})
            continue
        # Biome match: "<id>  x=.. z=..  <biome> <dist> from <ref>". A biome name
        # sits before the distance, so this must be tried before the structure
        # "places" rule (which expects the number right after the coordinates).
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\w+) (\d+) from (\w+)", line)
        if m and cur:
            cur.setdefault("biome", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "biome": m.group(4), "dist": int(m.group(5)), "ref": m.group(6)})
            continue
        # Tight cluster: "<id>  x=.. z=..  <N> <structure> within <T> tight".
        # The trailing "tight" marks a spread cluster; try it before the anchored
        # cluster rule (which would otherwise swallow the same shape).
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) (\w+) within (\d+) tight", line)
        if m and cur:
            cur.setdefault("cluster", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "structure": m.group(5),
                 "spread": int(m.group(6)), "tight": True})
            continue
        # Anchored cluster: "<id>  x=.. z=..  <N> <structure> within <R>".
        # Distinct from the ore/slime/area rows (those carry "ore"/"chunks"/"%").
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) (\w+) within (\d+)", line)
        if m and cur:
            cur.setdefault("cluster", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "structure": m.group(5), "within": int(m.group(6))})
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) (\w+)$", line)
        if m and cur and m.group(5) not in ("from",):
            cur.setdefault("loot", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "item": m.group(5)})
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) from (\w+)"
                     r"(?:, size (\d+))?", line)
        if m and cur:
            pl = {"id": m.group(1), "x": int(m.group(2)),
                  "z": int(m.group(3)), "dist": int(m.group(4)),
                  "ref": m.group(5)}
            if m.group(6):
                pl["size"] = int(m.group(6))
            cur["places"].append(pl)
    return seeds


def parse_explain(out: str) -> dict:
    e = {}
    pats = {
        "pass1_pct": r"pass 1 survival\s*:\s*([\d.]+)%",
        "pass2_pct": r"pass 2 survival\s*:\s*([\d.]+)%",
        "seeds_per_hit": r"structure seeds per hit\s*:\s*([\d.]+)",
        "pass1_share": r"time split\s*:\s*pass 1 ([\d.]+)%",
    }
    for k, p in pats.items():
        m = re.search(p, out)
        if m:
            e[k] = float(m.group(1))
    for k, p in {"rate": r"predicted search rate\s*:\s*(.+?) on",
                 "first_hit": r"time to first hit\s*:\s*(.+)",
                 "twelve": r"time to 12 hits\s*:\s*(.+)"}.items():
        m = re.search(p, out)
        if m:
            e[k] = m.group(1).strip()
    m = re.search(r"VERDICT:\s*(.+)", out)
    if m:
        e["verdict"] = m.group(1).strip()
    e["too_loose"] = "not filtering" in out or "too" in out and "loose" in out
    return e


# ----------------------------------------------------------------- handlers

def api_vocab(qs) -> dict:
    v = check_version((qs.get("v") or ["1.21"])[0])
    rc, out, err = run([tool("vocab"), v], TIMEOUTS["plan"])
    if rc != 0:
        raise Failure(err.strip() or f"vocab failed for {v}")
    return json.loads(out)


def api_assets() -> dict:
    """Report which optional graphics the user has dropped in, so the UI only
    references images that exist (a missing <img> flickers a broken icon)."""
    def names(sub):
        d = ASSETS / sub
        if not d.is_dir():
            return []
        return sorted(f.stem for f in d.glob("*.png"))
    # The centre logo: a file named seed.* takes priority (that's what people
    # reach for), else logo.*.
    imgext = (".png", ".webp", ".gif", ".jpg", ".jpeg")
    logo = None
    if ASSETS.is_dir():
        for stem in ("seed", "logo"):
            logo = next((f.name for f in ASSETS.glob(stem + ".*")
                         if f.suffix.lower() in imgext), None)
            if logo:
                break
    return {"structures": names("structures"), "items": names("items"),
            "backdrop": (ASSETS / "backdrop.png").is_file(), "logo": logo}


def api_lootitems(qs) -> dict:
    v = check_version((qs.get("v") or ["1.21"])[0])
    rc, out, err = run([tool("lootitems"), v], TIMEOUTS["plan"])
    if rc != 0:
        raise Failure(err.strip() or f"lootitems failed for {v}")
    return json.loads(out)


def _plan_or_fail(path, extra=(), timeout=None, key="plan"):
    rc, out, err = run([tool("find"), path, *extra], timeout or TIMEOUTS[key],
                       env=find_env())
    # find.exe prints "query error:" / "plan error:" and exits non-zero.
    for marker in ("query error:", "plan error:"):
        if marker in out or marker in err:
            msg = (out + err).split(marker, 1)[1].strip().splitlines()[0]
            raise Failure(msg)
    return rc, out, err


def api_plan(body) -> dict:
    path = write_query(body)
    try:
        # range=1 thread=1: prints the plan, searches a single seed. Cheap and safe.
        _, out, _ = _plan_or_fail(path, ["1", "1"])
    finally:
        drop_query(path)
    return {"plan": parse_plan(out), "notes": parse_notes(out), "raw": human(out)}


def api_explain(body) -> dict:
    path = write_query(body)
    samples = max(1000, min(50_000_000, int(body.get("samples", 2_000_000))))
    try:
        _, out, _ = _plan_or_fail(path, ["--explain", str(samples), str(clamp_threads(body))],
                                  TIMEOUTS["explain"], "explain")
    finally:
        drop_query(path)
    return {"plan": parse_plan(out), "explain": parse_explain(out),
            "notes": parse_notes(out), "raw": human(out)}


def api_search(body) -> dict:
    path = write_query(body)
    rng = max(1, min(500_000_000, int(body.get("range", 3_000_000))))
    try:
        _, out, _ = _plan_or_fail(path, [str(rng), str(clamp_threads(body))],
                                  TIMEOUTS["search"], "search")
    finally:
        drop_query(path)
    return {"plan": parse_plan(out), "seeds": parse_hits(out),
            "funnel": parse_funnel(out), "notes": parse_notes(out), "raw": human(out)}


def api_check(body) -> dict:
    """Does each of these seeds satisfy the query? (find --seed, per seed)

    The question most of verifying a known find comes down to -- "does THIS
    seed have it" -- had no endpoint; the only path was to search for it. Seeds
    may be numbers or text, read the way Minecraft reads them.
    """
    raw = body.get("seeds")
    if isinstance(raw, str):
        raw = re.split(r"[\s,;]+", raw.strip())
    if not isinstance(raw, list):
        raise Failure("'seeds' must be a list (or a comma/space separated string)")
    seeds = [str(s).strip() for s in raw if str(s).strip()][:200]
    if not seeds:
        raise Failure("give at least one seed")
    path = write_query(body)
    results = []
    try:
        for s in seeds:
            ws = java_seed(s)
            rc, out, err = run([tool("find"), path, "--seed", ws], TIMEOUTS["search"],
                               env=find_env())
            for marker in ("query error:", "plan error:"):
                if marker in out or marker in err:
                    raise Failure((out + err).split(marker, 1)[1].strip().splitlines()[0])
            verdict = "yes" if rc == 0 else ("no" if rc == 3 else "error")
            why = ""
            m = re.search(r"^NO -- (.+)$", out, re.M)
            if m:
                why = m.group(1).strip()
            hit = (parse_hits("SEED " + ws + "\n" + out.split("YES", 1)[1])
                   if verdict == "yes" and "YES" in out else [])
            results.append({"input": s, "seed": ws, "verdict": verdict,
                            "why": why, "hit": hit[0] if hit else None})
    finally:
        drop_query(path)
    return {"results": results,
            "matched": sum(1 for r in results if r["verdict"] == "yes")}


def api_describe(body) -> dict:
    seed = java_seed(body.get("seed", ""))
    v = check_version(str(body.get("version", "1.21")))
    radius = int(body.get("radius", 2000))
    # The world preset travels with the seed: describing a Large Biomes result
    # against the default generator would list biomes that are not there.
    args = [tool("describe"), seed, v, str(radius)]
    if body.get("large_biomes"):
        args.append("large")
    rc, out, err = run(args, TIMEOUTS["describe"])
    if rc != 0:
        raise Failure(err.strip() or "describe failed")
    return {"raw": out}


def api_map(qs) -> bytes:
    """Render a seed's biome map. Returns raw PNG bytes."""
    seed = java_seed((qs.get("seed") or [""])[0])
    v = check_version((qs.get("v") or ["1.21"])[0])
    radius = max(200, min(20000, int((qs.get("r") or ["2000"])[0])))
    px = max(64, min(1024, int((qs.get("px") or ["560"])[0])))
    cx = max(-30000000, min(30000000, int((qs.get("cx") or ["0"])[0])))
    cz = max(-30000000, min(30000000, int((qs.get("cz") or ["0"])[0])))
    large = (qs.get("large") or ["0"])[0] in ("1", "true")
    dim = (qs.get("dim") or ["overworld"])[0]
    if dim not in ("overworld", "nether", "end"):
        raise Failure(f"unknown dimension {dim!r}")
    out = ROOT / "build" / "tmp"
    out.mkdir(parents=True, exist_ok=True)
    # Write to a file rather than piping: PNG bytes through a pipe are easy to
    # corrupt, and a path keeps the failure mode obvious. Unique name per tile so
    # concurrent pan/zoom requests don't clobber each other.
    dest = out / f"map_{abs(hash((seed, v, radius, px, cx, cz, large, dim))):x}.png"
    margs = [tool("map"), seed, v, str(radius), str(px), dest, str(cx), str(cz)]
    # Positional up to cz; the tail flags are order-independent keywords, so a
    # nether map without large biomes still needs a placeholder in that slot.
    margs.append("large" if large else "-")
    if dim != "overworld":
        margs.append(dim)
    rc, _, err = run(margs, TIMEOUTS["map"])
    if rc != 0 or not dest.exists():
        raise Failure(err.strip() or "map render failed")
    data = dest.read_bytes()
    try:
        dest.unlink()
    except OSError:
        pass
    return data


def api_markers(qs) -> dict:
    """The structures a map view marks, as data: {"markers":[{type,x,z}], "spawn"}.

    The PNG bakes its markers in, so they could be seen but not clicked or
    read. This is the same list -- produced by the same code, through the same
    "does it really generate" gate as a search -- for the browser to overlay.
    """
    seed = java_seed((qs.get("seed") or [""])[0])
    v = check_version((qs.get("v") or ["1.21"])[0])
    radius = max(200, min(20000, int((qs.get("r") or ["2000"])[0])))
    cx = max(-30000000, min(30000000, int((qs.get("cx") or ["0"])[0])))
    cz = max(-30000000, min(30000000, int((qs.get("cz") or ["0"])[0])))
    large = (qs.get("large") or ["0"])[0] in ("1", "true")
    dim = (qs.get("dim") or ["overworld"])[0]
    if dim not in ("overworld", "nether", "end"):
        raise Failure(f"unknown dimension {dim!r}")
    args = [tool("map"), seed, v, str(radius), "64", "markers", str(cx), str(cz),
            "large" if large else "-"]
    if dim != "overworld":
        args.append(dim)
    rc, out, err = run(args, TIMEOUTS["map"])
    if rc != 0:
        raise Failure(err.strip() or "marker lookup failed")
    return json.loads(out)


def api_ask(body) -> dict:
    """Natural language -> query JSON, via ask.py --dry-run."""
    text = str(body.get("text", "")).strip()
    if not text:
        raise Failure("say what you're looking for")
    v = check_version(str(body.get("version", "1.21")))
    rc, out, err = run([sys.executable, str(ROOT / "ask.py"),
                        "--version", v, "--dry-run", text], 120)
    if rc != 0:
        detail = (err or out).strip().splitlines()
        hint = detail[-1] if detail else "ask.py failed"
        if "ANTHROPIC_API_KEY" in (err + out) or "authentication" in (err + out).lower():
            hint = ("no Anthropic credentials. Set ANTHROPIC_API_KEY, or run "
                    "`ant auth login`. The query builder below works without it.")
        raise Failure(hint)
    m = re.search(r"\{.*\}", out, re.S)
    if not m:
        raise Failure("ask.py returned no query")
    notes = re.search(r"notes\s*:\s*(.+)", out)
    return {"query": json.loads(m.group(0)),
            "notes": notes.group(1).strip() if notes else ""}


GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-flash-latest")

# The full condition schema, taught to Gemini. Kept here (not in the model's
# head) so it always matches what the C planner actually accepts. The vocabulary
# (structures/biomes) is injected per-version so the model can't name something
# the chosen version doesn't have.
AI_SCHEMA = """\
You translate a description of a Minecraft world into a JSON search query for a
seed finder. Output ONLY JSON of the form:
  { "conditions": [ ... ], "notes": "one short sentence on anything you assumed or could not express" }
(optionally with a "rank" key -- see Leaderboard below, and a "tool" key -- see
ROUTING TO OTHER ENGINES. If a request needs another engine you MUST emit "tool";
saying so in "notes" alone does nothing, because notes is prose and the tool
field is what actually runs it.)

Each condition is one object with a short lowercase "id". Distances ("within")
are in BLOCKS (a chunk is 16). "of" is "origin", "spawn", or another condition's
id (forming a tree). If no distance is given, choose a sensible one and say so in
notes. Do NOT order conditions for speed -- a cost planner reorders them.

"of" can also be an explicit position: "of":"1200000,-800000" searches around
that point instead of the middle of the world. Use it when the request names
coordinates, or refers to a known find that sits far out. Origin and spawn are
both near 0,0, so without it every search is confined to the middle of the map --
and many notable finds are hundreds of thousands of blocks away. Cost is the
same as origin; it is the same disc, somewhere else. Limited to +/-30000000
(the world border).

Note this does NOT help you DISCOVER rare things faster: seeds are independent,
so scanning many seeds near origin is just as likely to turn something up, and a
hit near origin is more useful to a player. Explicit positions are for CHECKING
or exploring a place someone already named.

"of":"spawn" measures from WORLD SPAWN, which is an ESTIMATE -- it depends on
grass placement, which is decoration and is not simulated. Structure positions
are exact; their distance from spawn is not. When a request is spawn-relative,
say so in notes rather than promising a spawn-adjacent result. "of":"origin"
(0,0) is exact and is the better default when the user just means "near the
middle of the world".

COOL SPAWNS. "I spawn in/on <thing>" is one of the most-asked shapes and it is
already expressible -- it is just "of":"spawn" with a SMALL within. Recipes:
  spawn in an ancient city   {"id":"ac","structure":"ancient_city","within":48,"of":"spawn"}
  spawn in a village         {"id":"v","structure":"village","within":48,"of":"spawn"}
  spawn inside a stronghold  {"id":"sh","stronghold":true,"within":112,"of":"spawn"}
      112 because a stronghold sprawls that far from its start, so anything
      inside that radius is over some part of it. Do NOT use "structure":
      "stronghold" -- a stronghold has no region position and is rejected.
  spawn on deepslate emerald {"id":"em","ore":"emerald","deepslate":true,
                              "count":4,"within":16,"of":"spawn"}
  village inside an ancient city
      {"id":"vac","overlap":["village","ancient_city"],"within":200,"of":"origin"}
      Verified against 6 published seeds: 5 reproduce exactly. Ancient cities
      need the deep_dark biome, checked at the city's own Y, not the surface.
Keep "within" small (16-112) or the answer stops meaning "at spawn". Say in
notes that spawn is an estimate -- see the paragraph above -- and that a
structure at spawn is UNDER it when the structure is underground (an ancient
city sits near y=-50, so you spawn above it and dig down).

"metric" chooses how "within" measures, and it matters more than it looks:
  omit it (default "euclidean") -- a DISC. Right for plain distance: "a village
      within 200 blocks", "near spawn".
  "metric":"chebyshev" -- a SQUARE, max(|dx|,|dz|). Use it whenever the request
      says ON ANY AXIS, IN A SQUARE, or counts in CHUNKS ("within 3 chunks",
      "in a 5x5 chunk area"). Seed finders mean this by default because
      structure regions are a chunk grid.
The two differ by up to sqrt(2): a village 240 east and 32 north is 240 away by
chebyshev and 242 by euclidean, so "within 241" finds it under one and not the
other. Picking the wrong one silently returns fewer seeds, never an error.

If a request pairs a LOOSE structure condition with a RARE biome or spawn
condition, emit a top-level "uppers": N alongside "conditions". The search tries only 64 of
a structure seed's 65536 sister worlds by default, and a rare biome condition
often lives in one it never tried -- which comes back as "no seeds found" rather
than as an error. 1024 is a reasonable ask for a rare biome or spawn condition;
it costs proportionally more time per structure seed. Do NOT set it for queries
that are only about structures: those do not use sister seeds at all.

Condition types (use EXACTLY these keys):
- Structure:      {"id","structure":<name>,"within","of"}
    variants (only where valid): "surface":true (ruined_portal, skip buried),
    "giant":true (ruined_portal), "abandoned":true (village = zombie village),
    "basement":true (igloo), "ship":true (end_city that contains an end ship =
    guaranteed elytra). For "end city with a ship / elytra" use
    {"structure":"end_city","ship":true}.
    cluster: add "count":N for >=N of that structure within the radius.
    tight cluster (quad-hut style): add "count":N AND "spread":T -- N instances
    within T blocks of EACH OTHER, anywhere within `within` of the reference.
  Add "family":true to a biome condition to mean the FAMILY rather than the one
  biome: {"biome":"badlands","family":true} covers wooded_badlands and
  eroded_badlands too. Families: badlands/mesa, jungle, taiga, forest, savanna,
  swamp, ocean, snowy, mountain, mushroom. Use it whenever the request names a
  landscape rather than an exact biome ("a badlands", "in jungle", "mountains"),
  because most family names are ALSO single biome names and without the flag you
  search only the one.

- Biome present:  {"id","biome":<name>,"within","of"}  (of may be another biome:
    "biome B within D of biome A" = the two biomes are adjacent.)
- ABSENCE: add "not":true to a structure or biome condition to require that
    there is NONE within the radius: "no ocean near spawn" ->
    {"id":"o","biome":"ocean","family":true,"within":300,"of":"spawn","not":true};
    "a mansion with no village within 600" -> the mansion, plus
    {"id":"nv","structure":"village","within":600,"of":"<mansion id>","not":true}.
    A "not" condition has no position: nothing may use it as "of", and it takes
    no count or variant flags. A biome absence scans every 4-block cell, so it
    is slow -- keep its radius modest (a few hundred blocks).
- Biome area:     {"id","biome_area":<name>,"pct":P,"within","of"}  P = min % of
    the disc that is this biome (a huge mushroom island, sprawling mesa).
- Island spawn:   {"id","island":true,"of":"spawn","pct":P,"within":R}  the spawn
    (default "of":"spawn") is on LAND and ocean covers >= P% (default 75) of the
    disc of radius R (default 256) -- a small survival island. Higher pct / larger
    within = smaller, more isolated island. Use for "island / survival island /
    stranded / surrounded by ocean" spawns.
- Terrain height: {"id","height":Y,"within","of"}  APPROXIMATE peak >= Y (tall
    mountains; small radius + of a structure = "tall structure"). Overworld only.
- Terrain relief: {"id","relief":D,"within","of"[,"exact":true]}  peak-minus-valley
    drop >= D blocks in the disc = STEEP terrain. Without "exact" it uses the
    SMOOTHED estimate (cheap, but blind to sharp cliffs). With "exact":true it uses
    Minecraft's REAL block-level terrain (accurate but SLOW, 1.18+ only).
    NOTE: a "glitched / tall-cobblestone pillager outpost" does not exist in
    JAVA -- Java drops a small dirt platform under a structure that lands in air,
    while BEDROCK fills a foundation to the ground out of the structure's own
    material. Do NOT substitute a Java mountain outpost for it: a Java seed does
    not carry over, because Bedrock places structures differently, so the
    substitute answers a question nobody asked. Route it instead:
    "tool": {"name": "bedrock_outpost", "min_drop": N}
- Ore density:    {"id","ore":<material>,"count":N,"within","of"}  materials:
    diamond iron gold emerald redstone lapis copper coal quartz ancient_debris nether_gold
    "vein":N  -> at least N of that ore in ONE connected blob ("a big diamond
    vein", "diamonds in one spot"), which is far rarer than a scattered total.
    "exposed":true -> only count ore with a non-solid neighbour in real terrain,
    i.e. ore visible in a cave wall. 1.18+ overworld only, SLOW, radius <= 64.
    "deepslate":true -> count only blocks below y=0, which is what makes them
    DEEPSLATE ore. Deepslate emerald/diamond/etc is NOT a separate material and
    there is no "deepslate_emerald" to pass as <material>: it is the same ore
    wearing whatever stone it replaced, so the band decides the block form.
    Use it for "deepslate emerald ore", "deepslate diamonds". 1.18+ only.
    "y_min":N,"y_max":M -> an explicit inclusive Y band instead, when the user
    names depths ("diamonds below y=-50").
    SPAWN: `"of":"spawn"` with a small "within" (16 is the spawn chunk) is how
    you answer "I spawn on/next to <ore>" -- e.g. spawning on deepslate emerald:
      {"id":"em","ore":"emerald","deepslate":true,"count":4,"within":16,"of":"spawn"}
    Be honest in notes that this means the ore is in the spawn chunk at that
    depth, not that the player is standing on the block; spawn is a surface
    point and deepslate is below y=0, so it is under them, not beneath a foot.
- Exposed stronghold: {"id","stronghold":"exposed"|"shallow"|true,"within","of"
    [,"max_depth":N][,"land":false]}
    THE KEY IS "stronghold". Writing {"exposed":true} instead is wrong and the
    engine rejects it -- "exposed" is a buried-treasure modifier, not a
    condition. Copy this shape exactly, INCLUDING the version:
      {"version":"1.17","conditions":[
        {"id":"sh","stronghold":"exposed","within":3000,"of":"origin"}]}
    A stronghold whose roof comes up to the surface. Use for "exposed
    stronghold", "surface stronghold", "a stronghold you can see", "a stronghold
    right under the grass". "exposed" = max_depth 0 (it breaks the surface),
    "shallow" = max_depth 4, true = any stronghold at all.
    max_depth is measured PER PIECE against the ground over that piece, not as a
    bare Y: y=60 is a metre down on a plain and buried inside a mountain, and
    only one of those is the find. The reported /tp points at the shallowest
    piece, which can be 100 blocks from the stronghold's centre.
    "land":false also allows sea-floor ones. Dry land is the DEFAULT because
    most strongholds reaching the surface do so on an ocean floor -- verified: a
    hit at roof y=51 had water directly on 466 of its 546 roof columns.
    VERSION: PRE-1.18 ONLY. From 1.18 moveBelowSeaLevel starts the structure 64
    blocks lower, so the roof can never exceed y=52 while the ground stays near
    y=63 -- an exposed stronghold does not exist there, and the query refuses
    the version rather than searching for something impossible. You MUST put
    "version":"1.17" in the query you return -- not merely mention 1.17 in
    notes -- whenever the request is for an exposed/surface/shallow stronghold,
    or for a stronghold inside ANY structure (a mansion, a village, a monument,
    whichever they name), UNLESS the user named a version themselves. The version the system prompt gives you is the one the page has
    selected, and for this request it is the wrong one; overriding it is
    expected here and the server accepts it. Say in notes that you changed it.
    ACCURACY: checked against end portal frames on a real 1.16.5 server, the
    height was exact for 15 of 15 strongholds with roofs at or below y=52, but
    only 2 of 3 above it -- and an exposed one is always above it. Say that a
    hit is a candidate worth checking, not a certainty.
    COST: walks the stronghold rings with the biome generator, ~1 ms per world
    seed, thousands of times slower than a structure condition. There is no
    48-bit prefilter for it, so every seed pays. Keep "within" <= 3000.
- Structure overlap: {"id","overlap":[<structure A>,<structure B>],"within","of"[,"pad":P]}
    the two structures' footprints MEET -- "a ruined portal inside a village", "a
    village generated on top of a shipwreck", "two structures colliding". "pad"
    adds slack in blocks (use ~16 for "practically touching"). Footprints are
    nominal boxes, so a hit is a strong candidate, not a proof.
    STRONGHOLD is allowed here and ONLY here (it has no region position, so it
    cannot be used as a plain "structure" condition). Use it for "a stronghold
    inside a mansion", "a stronghold generated into a village", "an exposed
    stronghold". It is tested against the stronghold's REAL piece boxes, not a
    nominal square, because a stronghold is 112 blocks of corridor in every
    direction and a square that size would match almost anything nearby.
      {"id":"sm","overlap":["stronghold","mansion"],"within":3000,"of":"origin"}
    "max_depth":D is how you say EXPOSED, and it is the one you almost always
    want: it measures the GROUND OVER the piece that meets the partner, so D=0
    means that piece breaks the surface and D=4 means a few blocks of dirt over
    it. Dry land is assumed (add "land":false to allow a river or sea floor).
      {"id":"sv","overlap":["stronghold","village"],"within":3000,"of":"origin",
       "max_depth":0,"version":"1.17"}
    is "an exposed stronghold inside a village" -- use exactly this shape.
    DO NOT reach for min_roof_y to mean exposed. It is only a Y threshold with
    no terrain in it, and the two genuinely disagree: a search for
    min_roof_y=55 in a village returned strongholds sitting under 7 to 13 blocks
    of ground, and the one that got walked to in game was buried under four.
    A roof at y=60 is at the surface on a plain and deep inside a hill.
    "min_roof_y":N additionally requires the piece that meets the partner to
    reach y=N or above -- a cruder filter, useful only to say "somewhere high
    up" when you do not care about the ground. A stronghold
    roof above y=53 only happens when the stronghold is unusually tall, so N in
    the 55-62 range is a genuinely rare find; omit it for "a stronghold under a
    mansion" and use it for "a stronghold exposed in a mansion".
    VERSION, AND THIS IS NOT OPTIONAL: min_roof_y is subject to the SAME 1.18
    cutoff as the standalone stronghold condition above. From 1.18 the roof can
    never exceed y=52, and the engine REFUSES min_roof_y>52 with a hard query
    error -- the search does not run at all and the user sees nothing. So
    whenever you set min_roof_y above 52 you MUST also put "version":"1.17" in
    the query you return, exactly as for the standalone condition, and say in
    notes that you changed it. This applies to EVERY partner, not just mansion:
    "an exposed stronghold inside a village", "a surface stronghold in a
    monument", any of them. Do not lower N to 52 to get past the error instead
    -- at 1.18+ y=52 is the ceiling, only about 1 stronghold in 180 reaches it,
    and it is still ~11 blocks under a y=63 surface, so it is not exposed and
    the search runs essentially forever. Change the version, not the number.
    COST: this walks the stronghold rings with the biome generator, ~1 ms per
    world seed, so it is thousands of times slower than an ordinary structure
    condition. Keep "within" at 3000 or less and expect a long search when
    min_roof_y is set.
    COMPOSING IT: hang other conditions off the STRUCTURE, not off the overlap.
    An overlap reports one position between the two footprints, so
    "outpost within 48 of ov" asks for an outpost near that midpoint and can be
    unsatisfiable even though the outpost is right there. Write the structure
    with its modifiers first and point the overlap AT it:
      {"id":"o","structure":"outpost","cave_below":30,"within":1500,"of":"origin"}
      {"id":"ov","overlap":["outpost","mansion"],"within":64,"of":"o","pad":16}
    That is 1 in ~84k structure seeds -- instant. The same shape composed the
    other way round sampled 0 in 200000 and looked impossible.
    This is how the spectacular compound requests are built ("an outpost on a
    cliff hanging over a mansion", "a village on a crust above a huge cavern
    next to a ruined portal"): one structure carrying the terrain modifiers,
    everything else anchored to it.
- Over a cave:    {"id","structure":<name>,"cave_below":N,"within","of"}  the
    structure stands on a crust above an open cavern at least N blocks tall.
    Use for "village over/above/on top of a cave", "a village in a cave", "a
    structure over a huge cavern". 1.18+ overworld, SLOW (real block terrain);
    N of 20-30 is a big cave, 40+ is dramatic. NOTE a village is never INSIDE a
    cave -- villages generate on the surface -- so this is the honest reading of
    that request, and worth saying in notes.
- On a floating island: {"id","structure":<name>,"floating":N,"within","of"}
    SEARCHES for a structure standing on land with an air gap of at least N
    blocks under it ("gap": how far out the gap is checked, default 24;
    "sides": how many of 8 directions must be open, default 6). 1.18+
    overworld, SLOW. Use this to FIND such seeds; the "floating" TOOL instead
    confirms one spot the user already has.
- Biome scan precision: "precision":"fast"|"fine"|"exact" on a biome or
    biome_area condition. "fine" (default) samples every chunk; "exact" every
    4-block cell -- use it for adjacency within ~32 blocks or very small
    radii, where coarser sampling misses; "fast" for big radii when speed
    matters more than recall.
- Geode shape:    {"id","structure":"geode","size":N,"cracked":false,"within","of"}
    size is 3 or 4 (4 = the big one); "cracked":false = the RARE SEALED geode
    (1 in 20), "cracked":true = broken open (19 in 20, barely a filter).
- Tall cactus:    {"id","cactus":N,"within","of"}  a cactus >= N blocks tall.
    One placement is only 1-3 blocks, so height comes from patches stacking
    on one column: 4-5 is already uncommon, 6 is about the ceiling for a
    single patch, and anything higher needs a slope where several patches
    with different origins pile up. Ask for 10+ only with a leaderboard and
    a real budget, and say in notes that it is a records hunt.
    Cacti are a DESERT/BADLANDS feature and 1.18+ only. SLOW (real terrain).
    Results are simulated and ~80% exact per column, so a record should be
    confirmed with tier3-java/confirm_cactus.py before it is claimed --
    worth mentioning in notes when the user asks for a tall one.
- Slime chunks:   {"id","slime":N,"within","of"}  >=N slime chunks (farm site).
- Chest loot:     {"id","within","loot":{"structure":<s>,"item":<i>,"count":N}}
    loot structures: desert_pyramid jungle_temple igloo outpost shipwreck
                     ruined_portal fortress bastion
    Nether: fortress counts every chest exactly. Bastion counts only the
    chests that ALWAYS generate (the starting piece), so its count is a
    LOWER BOUND -- a hit is real, a miss is not proof of absence. Say so in
    notes when a bastion loot condition is used.
- End portal eyes:{"id":"portal","eyes":N}  the first stronghold's portal has >=N eyes.

World preset. If the request says "large biomes", add "large_biomes": true next
to "conditions". It is a property of the world, not of a condition, and it
changes every biome answer -- so never set it unless it was asked for.

Search budget. If the request says how long or how far to look -- "search for
ten minutes", "scan 50 million seeds", "keep looking for an hour" -- add a
sibling key next to "conditions":
  "budget": {"minutes": M}   or   {"seeds": N}
Minutes is the better unit for a leaderboard, because how many seeds a range
buys depends entirely on how expensive the query is. Only include it if the
request actually mentions an amount of time or seeds.

Leaderboard (records). If the request is superlative -- "the TALLEST mountain",
"the BIGGEST diamond vein", "the TALLEST cactus", "the most slime chunks", "the largest mushroom
island" -- add a sibling key next to "conditions":
  "rank": {"of": <condition id>, "by": <metric>, "top": K}
  metric: auto|height|relief|vein|count|pct|size|area|tall  (auto = that condition's
  own measurement; usually correct). K defaults to 10.
A ranked search scans the whole range and returns the best it saw instead of
stopping at the first match, so set that condition's THRESHOLD LOW (e.g.
{"height":0} ranked by height) -- a tight threshold starves the leaderboard.
Use rank ONLY for superlatives; an ordinary request wants a plain filter.

IMPOSSIBLE COMBINATIONS -- say so, do not emit a search:
- A pillager outpost NEVER generates within 10 chunks (176 blocks) of a village.
  Minecraft's placement data excludes it (pillager_outposts.json exclusion_zone),
  so "an outpost in/on/next to a village", "an outpost on a village house/church",
  and anything pairing them closer than 176 blocks is IMPOSSIBLE. Put that in
  notes and emit the conditions that ARE possible (e.g. the village alone), or
  use 176+ blocks if the user only wanted them nearby.
- Two of the SAME structure are never closer than their grid allows (villages
  ~144 blocks, swamp huts ~144). A tight cluster tighter than that is impossible.
The engine refuses these too, but saying it in notes is faster and kinder than
watching a search that cannot succeed.

INSIDE a structure (which building, which room) is NOT expressible here. Village
buildings ARE searchable, but by a separate real-worldgen tool in the UI ("Village
buildings"), which finds villages containing N of a given building -- including
temple (the church), library, mason, butcher and the smiths. If the request is
about a building inside a village, say so in notes and emit a plain village
condition.

ROUTING TO OTHER ENGINES. This query schema drives ONE engine (cubiomes). Some
requests need a different one, and for those you must set a top-level "tool"
field. The UI runs that tool instead of the plain search, so a routed request
actually EXECUTES rather than being handed back as advice. The conditions you
emit are still used (they populate the builder), but the tool decides the run.

  "tool": {"name": "casing", "casing": "<block>", "radius": <blocks>}
      A buried treasure ENCASED in a block: "surrounded by magma", "generates on
      iron ore", "walled in gravel". radius is how much of EACH world to examine
      (default 10000; 20000 examines ~1431 treasures per world against 6 at
      1500). Runs real Minecraft worldgen WITH decoration, then confirms every
      candidate on an actual server before reporting it.
  "tool": {"name": "village_building", "building": "<b>", "min": N}
      N of a village building: smith, toolsmith, weaponsmith, armorer, library,
      cartographer, mason, fletcher, butcher, shepherd, fisher, tannery, temple,
      farm, animal_pen, stable.
  "tool": {"name": "exposed_treasure"}
      A treasure chest sitting in open air rather than buried.
  "tool": {"name": "fortress_overlap", "seeds": N}
      Nether fortresses that GROW THROUGH each other, ranked by intersecting
      piece pairs -- not by how close their starts are.
  "tool": {"name": "bedrock_outpost", "seeds": "<comma-separated>", "min_drop": N}
      A BEDROCK outpost on a tall cobblestone foundation -- the "glitched" /
      "floating" outpost from screenshots. Runs a real Bedrock Dedicated Server
      and measures the column block by block. min_drop is the terrain drop that
      makes a tall foundation possible at all (20 is a good default; lower finds
      more and shorter). Use this for ANY Bedrock Edition request, since the
      ordinary search is a Java engine and its seeds do not transfer.
  "tool": {"name": "struct_overlap", "a": "<structure>", "b": "<structure>"}

Floating island -- "is this outpost/island actually floating", "island in
the sky", "structure hovering over a void". Needs a seed and a position;
it CONFIRMS one spot rather than searching for them.
  "tool": {"name": "floating"}

Cave spider spawners near a village -- "spider spawners by a village", "cave
spider village", "spawners next to a village". Distance is in CHUNKS.
  "tool": {"name": "spider_village", "min": N, "radius": <chunks>}

Buried treasure encased in BEDROCK -- "treasure in bedrock", "chest surrounded by
bedrock". PRE-1.18 ONLY: from 1.18 the band sits under deepslate, which does not
stop the placement scan, so no chest is placed at all. Set the version to 1.16 or
older and say so in notes.
  "tool": {"name": "bedrock_casing", "seeds": N}

What is worth visiting in a seed the user ALREADY HAS -- "what's good in seed X",
"anything cool in my world", "best chests in this seed", "where are the diamonds
in seed X". This is the opposite of a search: it needs a SEED and reports
places. If they name a specific item, pass it.
  "tool": {"name": "cool_stuff", "seed": "<seed>", "item": "<id>", "min": N}
For an ENCHANTMENT, use "ench" and "ench_min" instead of item/min:
  "tool": {"name": "cool_stuff", "seed": "<seed>", "ench": "looting", "ench_min": 3}
Levels ARE modelled, so "a Looting III golden sword" is a real query.
      "are these two structures REALLY overlapping / inside each other". The
      ordinary overlap condition compares nominal boxes; this intersects real
      piece bounding boxes. Exact for fortress and end_city; for anything else it
      answers UNKNOWN, which must not be reported as "they do not overlap".
  "tool": {"name": "hunt"}
      The user wants it to keep going: "don't stop until you find one", "search
      forever", "keep looking". Use with plain conditions; it scans fresh seeds
      batch after batch instead of restarting on the same ones.

Omit "tool" entirely for anything the ordinary search handles -- most requests.

Why casings need their own engine, if the user asks: this one is cubiomes, which
has exact terrain but NO surface rules and NO decoration, so the sand column,
every ore and every magma block are ABSENT from it rather than slow to find.

How a casing works, if the user asks: the game scans down from the ocean floor
until the block BELOW is sandstone/stone/andesite/granite/diorite, and writes the
block it landed on into every air or water face around the chest. Ore and magma
are not stopping blocks, so the scan falls THROUGH them and lands ON them, which
is exactly why they can be the casing. Measured over 6646 chests: sand 78%,
gravel 18%, dirt 2.7%, stone 0.9%, copper_ore and magma_block ~0.015% EACH.
Bedrock casings need a pre-1.18 world (in 1.18+ deepslate sits above the bedrock
band and is not a stopping block, so the placement fails instead) -- say that
plainly rather than promising a search that cannot deliver.

If the user asks to "keep looking", "search until you find it", or says a search
gave up too early, point at the "Keep looking" tool in the UI: the ordinary
search walks seeds from the same starting point every time, so re-running it
covers the SAME seeds, while that one advances onto fresh seeds each batch.

Rules:
- Use ONLY structure/biome names from the vocabulary below. Never invent one.
- If the request implies something none of these express, leave it out and say so
  plainly in notes. Do not approximate it with an unrelated condition.
- Prefer fewer, tighter conditions -- every extra one makes seeds rarer.
"""


def _vocab_for(version: str) -> dict:
    rc, out, err = run([tool("vocab"), version], TIMEOUTS["plan"])
    if rc != 0:
        raise Failure(err.strip() or f"vocab failed for {version}")
    return json.loads(out)


def _ai_gemini(body, text, system, key):
    """Ask Gemini for the query JSON. Returns the raw model text."""
    # The model is settable from the page now. It was pinned to GEMINI_MODEL,
    # so when that alias stopped resolving the only escape was an environment
    # variable and a server restart -- while the UI kept saying "check your key",
    # which was the one thing that did not need checking.
    model = re.sub(r"[^A-Za-z0-9._-]", "", str(body.get("model", "")) or GEMINI_MODEL)
    payload = {
        "systemInstruction": {"parts": [{"text": system}]},
        "contents": [{"role": "user", "parts": [{"text": text}]}],
        "generationConfig": {"responseMimeType": "application/json", "temperature": 0.2},
    }
    url = (f"https://generativelanguage.googleapis.com/v1beta/models/{model}"
           f":generateContent?key={quote(key, safe='')}")

    # A DEADLINE for the whole call, not a timeout per attempt.
    #
    # Retry-then-try-alternatives multiplied the worst case instead of bounding
    # it: a stalled service could hold the request for a retry plus three
    # fallbacks at 45s each, and the page just sat there. The budget below caps
    # the lot, and each attempt gets whatever is left rather than a fixed slice.
    # 150s for the whole call, and a generous slice for the FIRST attempt.
    #
    # The first cut of this budgeted 55s total and capped each attempt at 30s,
    # which quietly made Gemini stricter than the compatible provider's 120s --
    # so a slow-but-working Gemini was cut off and reported as "the service is
    # struggling" when it had simply not finished. The system prompt is ~5.9k
    # tokens (the schema is what makes the planner accurate, so it stays), and a
    # loaded free tier can chew on that for well over half a minute.
    deadline = time.monotonic() + 150.0

    def _remaining():
        return deadline - time.monotonic()

    def _post():
        left = _remaining()
        if left < 5.0:
            raise Failure("Gemini did not answer within the time budget. The "
                          "service is slow or overloaded right now -- this is "
                          "not your key. Try again, or use the OpenAI-compatible "
                          "provider in settings.")
        req = urllib.request.Request(
            url, data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json",
                     "User-Agent": "seed-finder/1.0"}, method="POST")
        # TimeoutError is NOT a URLError. A read that stalls after the connection
        # is made raises it bare, so the except clauses below never saw it and it
        # escaped as an unhandled 500 reading "TimeoutError: The read operation
        # timed out" -- true, and useless.
        try:
            with urllib.request.urlopen(req, timeout=min(left, 90.0)) as resp:
                return json.loads(resp.read())
        except TimeoutError:
            raise Failure("Gemini did not answer within 90s. Your key is fine "
                          "-- the request reached Google and it is taking too "
                          "long, which the free tier does under load. Try again, "
                          "pin a specific model instead of gemini-flash-latest "
                          "(that alias points at the busiest build), or use the "
                          "OpenAI-compatible provider.")

    used_model = model
    try:
        try:
            data = _post()
        except urllib.error.HTTPError as first:
            # 503 is Google saying the MODEL is busy, not that anything here is
            # wrong -- the request authenticated to get that far. These spikes
            # are short, so one retry turns most of them into a normal answer
            # instead of an error the reader has to interpret.
            if first.code not in (503, 429):
                raise
            time.sleep(min(2.5, max(0.0, _remaining() - 6.0)))
            try:
                data = _post()
            except urllib.error.HTTPError as second:
                # Still saturated. The load is per MODEL, so another model on the
                # same key usually answers immediately -- and the alternative to
                # trying one is telling the user to go and pick one by hand,
                # which is the same work done more slowly and with a dead end in
                # the middle. gemini-flash-latest is an alias onto whichever
                # build is current, which is also the busiest.
                if second.code != 503:
                    raise
                for alt in _gemini_alternatives(key, model)[:3]:
                    if _remaining() < 15.0:
                        raise second
                    url = ("https://generativelanguage.googleapis.com/v1beta/models/"
                           f"{alt}:generateContent?key={quote(key, safe='')}")
                    try:
                        data = _post()
                        used_model = alt
                        break
                    except urllib.error.HTTPError:
                        continue
                else:
                    raise second
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        msg = raw
        try:
            msg = json.loads(raw)["error"]["message"]
        except Exception:  # noqa: BLE001
            pass
        if e.code in (400, 403) and re.search(r"api.?key|permission|invalid", msg, re.I):
            raise Failure(f"Gemini rejected the key ({e.code}). Check it in settings. [{msg}]")
        # A retired model alias fails here too, and reads exactly like a dead
        # key unless the message says which it was.
        if e.code == 404:
            raise Failure(f"Gemini has no model {model!r} ({e.code}). This is NOT a "
                          f"key problem -- set the model in settings, or switch "
                          f"provider. [{msg[:200]}]")
        if e.code == 503:
            raise Failure(f"Gemini's {model!r} is overloaded (503) and a retry did "
                          f"not clear it. YOUR KEY IS FINE -- a 503 means the "
                          f"request got through and Google is out of capacity for "
                          f"that model. Wait a minute, or set a different model in "
                          f"settings (Test connection lists what your key can use).")
        if e.code == 429:
            raise Failure(f"Gemini rate-limited this key (429) and a retry did not "
                          f"clear it. The key is valid -- this is the free tier's "
                          f"per-minute cap. Wait a moment and ask again.")
        raise Failure(f"Gemini error {e.code}: {msg}")
    except urllib.error.URLError as e:
        raise Failure(f"could not reach Gemini ({e.reason}). Check your connection.")

    try:
        cand = data["candidates"][0]
        parts = cand["content"]["parts"]
        # The model that ANSWERED, which after a 503 fallback is not the one
        # that was asked for. Substituting silently would leave the user reading
        # output from a model their settings say they are not using.
        return "".join(p.get("text", "") for p in parts), used_model
    except (KeyError, IndexError):
        reason = ""
        try:
            reason = data["candidates"][0].get("finishReason", "")
        except Exception:  # noqa: BLE001
            reason = (data.get("promptFeedback") or {}).get("blockReason", "")
        raise Failure(f"Gemini returned no query{f' ({reason})' if reason else ''}. Try rephrasing.")


# Base URLs people actually paste, normalised. A provider's docs give the root
# ("https://api.groq.com/openai/v1") but the habit is to paste the full endpoint,
# and ".../v1/chat/completions/chat/completions" is a 404 whose message says
# nothing about the real mistake.
def _chat_url(base: str) -> str:
    base = base.strip().rstrip("/")
    if base.endswith("/chat/completions"):
        return base
    return base + "/chat/completions"


def _ai_base(body) -> str:
    """The OpenAI-compatible base URL: the request's, else AI_API_BASE."""
    return (str(body.get("base", "")).strip()
            or os.environ.get("AI_API_BASE", "").strip())


def _ai_key(body, provider) -> str:
    """The API key to send -- and the reason it depends on WHERE it goes.

    A key typed into the browser goes wherever that browser says. A key from
    the server's environment must not: the compatible provider takes its base
    URL from the request, so "else fall back to AI_API_KEY" sent the server's
    key to any host a request named -- and since any web page can make a
    request to this server, that was a way to steal it. An environment key now
    goes only to the environment's own base (AI_API_BASE), or with no base in
    the request at all. Gemini's endpoint is fixed, so its key cannot move.
    """
    key = str(body.get("key", "")).strip()
    if key:
        return key
    if provider == "gemini":
        return os.environ.get("GEMINI_API_KEY", "")
    req_base = str(body.get("base", "")).strip().rstrip("/")
    env_base = os.environ.get("AI_API_BASE", "").strip().rstrip("/")
    if req_base and req_base != env_base:
        return ""
    return os.environ.get("AI_API_KEY", "")


def _ai_compatible(body, text, system, key):
    """Ask an OpenAI-compatible endpoint. Returns the raw model text.

    Groq, OpenAI, OpenRouter, DeepSeek, Together and a local Ollama all take the
    same POST, so the provider is only a base URL and a model name.
    """
    base = _ai_base(body)
    if not base:
        raise Failure("no API base URL. Open settings and set one -- "
                      "https://api.groq.com/openai/v1 for Groq, "
                      "https://api.openai.com/v1 for OpenAI, or "
                      "http://localhost:11434/v1 for a local Ollama.")
    if not base.startswith(("http://", "https://")):
        raise Failure(f"the API base URL must start with http:// or https:// (got {base!r})")
    model = str(body.get("model", "")).strip()
    if not model:
        raise Failure("no model name. Open settings and set one -- e.g. "
                      "llama-3.3-70b-versatile on Groq, gpt-4o-mini on OpenAI.")
    payload = {
        "model": model,
        "messages": [{"role": "system", "content": system},
                     {"role": "user", "content": text}],
        "temperature": 0.2,
        "response_format": {"type": "json_object"},
    }
    # A real User-Agent, because urllib's default is not one.
    #
    # Groq sits behind Cloudflare, which answers "Python-urllib/3.x" with error
    # code 1010 and a 403 -- indistinguishable, from here, from a rejected key.
    # Tested: with the default UA a bogus key returns "error code: 1010"; with
    # this one the same request comes back as a proper 401 and a JSON message
    # about the key. Without it a VALID key would have failed too, and the
    # message would have sent the user to re-check the one thing that was fine.
    headers = {"Content-Type": "application/json",
               "User-Agent": "seed-finder/1.0"}
    if key:
        headers["Authorization"] = "Bearer " + key     # a local Ollama needs none

    def post(pl):
        req = urllib.request.Request(_chat_url(base), data=json.dumps(pl).encode(),
                                     headers=headers, method="POST")
        # Same trap as the Gemini path: a stalled read raises TimeoutError, which
        # is not a URLError and would otherwise surface as a raw 500.
        # 120s, because FREE models queue.
        #
        # This was 45s, which is a reasonable number for an API and the wrong one
        # for this. Measured on OpenRouter's free tier: a query that returns
        # correctly took 44.3 seconds end to end -- inside the limit by seven
        # tenths of a second. The same request a minute later, with the queue
        # slightly longer, is a read timeout and looks like a broken key.
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                return json.loads(resp.read())
        except TimeoutError:
            raise Failure(f"{base} did not answer within 120s. Free-tier models "
                          f"queue and can take a minute or more; a paid or "
                          f"smaller model responds in seconds. Your key is fine "
                          f"-- nothing here is misconfigured.")

    try:
        data = post(payload)
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        msg = raw
        try:
            msg = json.loads(raw)["error"]["message"]
        except Exception:  # noqa: BLE001
            pass
        # response_format is not universal; several compatible servers reject it.
        # The answer is pulled out with a regex either way, so losing the whole
        # request over a formatting hint would be silly.
        if e.code in (400, 404, 422) and re.search(r"response_format|json_object", msg, re.I):
            payload.pop("response_format", None)
            try:
                data = post(payload)
            except urllib.error.HTTPError as e2:
                raise Failure(f"AI error {e2.code}: "
                              f"{e2.read().decode(errors='replace')[:300]}")
        elif e.code in (401, 403):
            raise Failure(f"the endpoint rejected the key ({e.code}). Check it in "
                          f"settings. [{msg[:200]}]")
        elif e.code == 404:
            raise Failure(f"404 from {_chat_url(base)} -- check the base URL, and "
                          f"that the model {model!r} exists there. [{msg[:200]}]")
        elif e.code == 429:
            raise Failure(f"the provider rate-limited this key (429). [{msg[:200]}]")
        else:
            raise Failure(f"AI error {e.code}: {msg[:300]}")
    except urllib.error.URLError as e:
        raise Failure(f"could not reach {base} ({e.reason}). Check the URL and "
                      f"your connection.")

    try:
        return data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise Failure("the endpoint returned no message -- check the model name.")


def _gemini_models(key):
    """Model ids this key may call generateContent on. [] if the list fails.

    Shared by the diagnostic and the 503 fallback so the two can never disagree
    about what is available -- a "try one of these" list that does not match
    what the retry actually tries would be worse than no list.
    """
    url = ("https://generativelanguage.googleapis.com/v1beta/models?pageSize=200&key="
           + quote(key, safe=""))
    req = urllib.request.Request(url, headers={"User-Agent": "seed-finder/1.0"},
                                 method="GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read())
    except Exception:  # noqa: BLE001
        return []
    out = []
    for m in data.get("models", []):
        if "generateContent" not in (m.get("supportedGenerationMethods") or []):
            continue
        name = m.get("name", "").split("/")[-1]
        if name:
            out.append(name)
    return out


def _gemini_alternatives(key, failed):
    """Models worth retrying after a 503, best first.

    Preferring flash keeps the planner cheap and fast, which is what this job
    wants; the rest follow so a fallback still exists when flash as a whole is
    the thing under load. Experimental and preview builds sort last -- they are
    the most likely to be overloaded in the first place.
    """
    names = [n for n in _gemini_models(key) if n != failed]

    def rank(n):
        return (0 if "flash" in n else 1,
                1 if ("exp" in n or "preview" in n) else 0,
                0 if "latest" not in n else 1,
                len(n))
    return sorted(names, key=rank)


def api_aitest(body) -> dict:
    """Is the key good, and does the model exist? Answered separately.

    "My key stopped working" covers at least three different faults, and the
    generateContent call cannot tell them apart: a revoked key, an exhausted
    quota and a RETIRED MODEL ALIAS all surface as a failed request. The last
    one is the cruel case -- the key is perfect and no amount of re-signing up
    will help.

    Listing models answers both questions at once. The list call is free, it
    needs only the key, and a model missing from the result is proof rather
    than a hypothesis.
    """
    provider = str(body.get("provider", "")).strip().lower() or "gemini"
    key = _ai_key(body, provider)
    if provider == "gemini":
        if not key:
            raise Failure("no key to test -- paste one first")
        # Google answers a key from another service with "API key not valid",
        # the same words it uses for a revoked one -- so the advice that follows
        # is "make a new key", and a new key cannot fix the wrong key being sent.
        # The prefix settles it before the request leaves.
        if key.startswith("sk-"):
            return {"ok": False, "key_ok": False,
                    "verdict": "That is an OpenAI/OpenRouter key (it starts "
                               "'sk-'), not a Google one, and Google will reject "
                               "it as invalid however many times you re-issue it. "
                               "Either switch the provider to OpenAI-compatible, "
                               "or paste a Google key -- those start 'AIza'.",
                    "detail": ""}
        # Deliberately NOT checking for an "AIza" prefix.
        #
        # That check was here and it rejected a perfectly good key: AI Studio
        # also issues keys beginning "AQ.", and hardcoding one vendor's format
        # meant the diagnostic confidently told a user their working key was not
        # a Gemini key at all. Only "sk-" is worth refusing, because that prefix
        # belongs to a different vendor and cannot be a Google key. Anything
        # else goes to Google, which is the authority on whether it is valid.
        want = str(body.get("model", "")).strip() or GEMINI_MODEL
        url = ("https://generativelanguage.googleapis.com/v1beta/models?pageSize=200&key="
               + quote(key, safe=""))
        headers = {"User-Agent": "seed-finder/1.0"}
    else:
        base = _ai_base(body)
        if not base:
            raise Failure("set the API base URL first")
        if not base.startswith(("http://", "https://")):
            raise Failure(f"the API base URL must start with http:// or https:// (got {base!r})")
        want = str(body.get("model", "")).strip()
        url = base.rstrip("/") + "/models"
        headers = {"User-Agent": "seed-finder/1.0"}
        if key:
            headers["Authorization"] = "Bearer " + key

    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read())
    except TimeoutError:
        raise Failure("the provider did not answer in 30s. That is the service "
                      "being slow, not a bad key -- try the test again.")
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        msg = raw
        try:
            msg = json.loads(raw)["error"]["message"]
        except Exception:  # noqa: BLE001
            pass
        if e.code in (400, 401, 403):
            return {"ok": False, "key_ok": False,
                    "verdict": f"The key was REJECTED ({e.code}). It is revoked, "
                               f"restricted, or from a different project. A new key "
                               f"will fix this.",
                    "detail": msg[:300]}
        if e.code == 429:
            return {"ok": False, "key_ok": True,
                    "verdict": "The key is VALID but rate-limited or out of quota "
                               "(429). Nothing is broken -- wait for the window to "
                               "reset, or use a key from another project.",
                    "detail": msg[:300]}
        return {"ok": False, "key_ok": None,
                "verdict": f"Unexpected {e.code} from the provider.", "detail": msg[:300]}
    except urllib.error.URLError as e:
        raise Failure(f"could not reach the provider ({e.reason}). Check your connection.")

    # Gemini returns models/<id>; an OpenAI-compatible list returns data[].id
    if provider == "gemini":
        names = [m.get("name", "").split("/")[-1] for m in data.get("models", [])
                 if "generateContent" in (m.get("supportedGenerationMethods") or [])]
    else:
        names = [m.get("id", "") for m in (data.get("data") or [])]
    names = [n for n in names if n]

    if want and want not in names:
        near = [n for n in names if "flash" in n or "mini" in n][:6] or names[:6]
        return {"ok": False, "key_ok": True, "models": names[:60], "suggest": near,
                "verdict": f"The key WORKS -- this is not a key problem. But the "
                           f"model {want!r} is not in the {len(names)} this key can "
                           f"use, so every request 404s. Pick one of the suggested "
                           f"models below and save it.",
                "detail": ""}
    return {"ok": True, "key_ok": True, "models": names[:60], "suggest": [],
            "verdict": f"Key works and {want!r} is available "
                       f"({len(names)} models usable).", "detail": ""}


def api_gemini(body) -> dict:
    """Natural language -> query JSON, via Gemini or any OpenAI-compatible API.

    The key comes from the request (the UI stores it in the browser) or the
    GEMINI_API_KEY env var. We call Gemini server-side so the browser never has
    to deal with CORS, and force JSON output via responseMimeType.
    """
    text = str(body.get("text", "")).strip()
    if not text:
        raise Failure("say what you're looking for")
    # Which service turns the sentence into a query.
    #
    # This was Gemini and nothing else, with the endpoint written into the
    # function -- so one signup page and one quota stood between the chat box
    # and working at all, and a dead key left no second option. Everything
    # except Gemini speaks the OpenAI chat-completions shape, so a single
    # alternative branch covers Groq, OpenAI, OpenRouter, DeepSeek, Together and
    # a local Ollama. The provider is just a base URL.
    provider = str(body.get("provider", "")).strip().lower() or "gemini"
    if provider not in ("gemini", "compatible"):
        raise Failure(f"unknown provider {provider!r} -- use 'gemini' or 'compatible'")
    key = _ai_key(body, provider)
    if not key and provider == "gemini":
        raise Failure("no Gemini API key yet. Open settings (the dot by the search "
                      "box) and paste one -- get a free key at "
                      "https://aistudio.google.com/apikey")
    v = check_version(str(body.get("version", "1.21")))
    vocab = _vocab_for(v)
    system = (AI_SCHEMA + "\n\nMinecraft version: " + vocab["version"]
              + "\n\nValid structures: " + ", ".join(vocab["structures"])
              + "\n\nValid biomes: " + ", ".join(vocab["biomes"]))

    # Both providers return TEXT, and everything after this point -- the JSON
    # extraction, the condition whitelist, the version override -- is shared.
    # Giving each provider its own parser would mean two places for a malformed
    # answer to slip through, and only one of them ever exercised.
    switched = ""
    if provider == "compatible":
        answer = _ai_compatible(body, text, system, key)
    else:
        answer, used_model = _ai_gemini(body, text, system, key)
        if used_model and used_model != (str(body.get("model", "")).strip() or GEMINI_MODEL):
            switched = used_model

    m = re.search(r"\{.*\}", answer, re.S)
    if not m:
        raise Failure("Gemini did not return a usable query. Try rephrasing.")
    try:
        obj = json.loads(m.group(0))
    except ValueError:
        raise Failure("Gemini returned malformed JSON. Try rephrasing.")
    conds = obj.get("conditions") or []
    if not isinstance(conds, list) or not conds:
        raise Failure(obj.get("notes") or "couldn't turn that into any conditions -- try being more specific.")
    # The model may override the version (e.g. a request that only makes sense on
    # an older release, like a pre-1.18 terrain feature). Validate it.
    ver = v
    ov = obj.get("version")
    if isinstance(ov, str):
        try:
            ver = check_version(ov)
        except Exception:  # noqa: BLE001
            ver = v
    q = {"version": ver, "conditions": conds}
    # The world preset. The prompt teaches the model to emit it and the engine
    # parses it, but the rebuild dropped it -- so "large biomes" requests ran on
    # the default generator and described a different world.
    if obj.get("large_biomes") is True:
        q["large_biomes"] = True
    # Sister-seed depth, if the planner judged the query needs it. Whitelisted
    # like everything else: a value that is not copied here vanishes silently,
    # which is exactly how "tool" once went missing while its prose survived.
    ou = obj.get("uppers")
    if isinstance(ou, (int, float)) and ou > 0:
        q["uppers"] = max(1, min(65536, int(ou)))
    # The query is rebuilt from a whitelist, so anything not copied here is
    # silently discarded. "tool" was not, which is exactly how a coal-ore casing
    # request came back as a plain buried-treasure search WITH a note claiming it
    # had been routed to the casing tool -- the prose survived and the thing that
    # actually routes did not. Validated rather than passed through: an unknown
    # name would send the UI to a panel that does not exist.
    TOOLS = {"casing", "village_building", "exposed_treasure",
             "fortress_overlap", "hunt", "bedrock_outpost", "struct_overlap",
             "floating", "spider_village", "bedrock_casing", "cool_stuff"}
    t = obj.get("tool")
    if isinstance(t, dict) and t.get("name") in TOOLS:
        keep = {"name": t["name"]}
        if isinstance(t.get("casing"), str) and re.match(
                r"^(minecraft:)?[a-z0-9_]{2,40}$", t["casing"]):
            keep["casing"] = t["casing"].replace("minecraft:", "")
        if isinstance(t.get("building"), str) and re.match(r"^[a-z_]{3,20}$", t["building"]):
            keep["building"] = t["building"]
        for side in ("a", "b"):
            if isinstance(t.get(side), str) and re.match(r"^[a-z_]{3,30}$", t[side]):
                keep[side] = t[side]
        if isinstance(t.get("seed"), str) and re.match(r"^-?\d{1,20}$", t["seed"]):
            keep["seed"] = t["seed"]
        if isinstance(t.get("item"), str) and re.match(
                r"^(minecraft:)?[a-z0-9_]{2,40}$", t["item"]):
            keep["item"] = t["item"].replace("minecraft:", "")
        if isinstance(t.get("ench"), str) and re.match(r"^[a-z_]{3,40}$", t["ench"]):
            keep["ench"] = t["ench"]
        if isinstance(t.get("ench_min"), (int, float)):
            keep["ench_min"] = max(1, min(10, int(t["ench_min"])))
        if isinstance(t.get("seeds"), str) and re.match(
                r"^-?\d{1,20}(,-?\d{1,20})*$", t["seeds"]):
            keep["seed_list"] = t["seeds"]      # bedrock takes explicit seeds
        # spider_village measures its radius in CHUNKS, not blocks, so it
        # cannot share the block-radius clamp below.
        if t["name"] == "spider_village" and isinstance(t.get("radius"), (int, float)):
            keep["radius"] = max(1, min(16, int(t["radius"])))
            t = dict(t); t.pop("radius", None)
        for k, lo, hi in (("radius", 1000, 60000), ("min", 1, 12),
                          ("want", 1, 50), ("seeds", 1000, 5_000_000),
                          ("min_drop", 4, 120)):
            if isinstance(t.get(k), (int, float)):
                keep[k] = max(lo, min(hi, int(t[k])))
        q["tool"] = keep
    # A leaderboard request survives only if it names a condition that exists;
    # a dangling "rank" would be a hard planner error rather than a search.
    # A budget the model emitted travels back to the UI, which owns the
    # controls -- the planner itself has no notion of wall-clock time.
    budget = obj.get("budget")
    if isinstance(budget, dict):
        b = {}
        if isinstance(budget.get("minutes"), (int, float)) and budget["minutes"] > 0:
            b["minutes"] = min(float(budget["minutes"]), 360.0)
        if isinstance(budget.get("seeds"), (int, float)) and budget["seeds"] > 0:
            b["seeds"] = min(int(budget["seeds"]), 500_000_000)
        if b:
            q["budget"] = b
    rank = obj.get("rank")
    if isinstance(rank, dict) and any(c.get("id") == rank.get("of") for c in conds):
        q["rank"] = {"of": rank["of"],
                     "by": rank.get("by", "auto"),
                     "top": int(rank.get("top", 10) or 10)}
    notes = obj.get("notes", "")
    if switched:
        # Say it in the line the user actually reads, not only the server log.
        notes = (f"[the model you set was overloaded, so {switched} answered "
                 f"instead] " + notes).strip()
    return {"query": q, "notes": notes}


def _jsonl(out: str):
    """(hits, summary) from a tier worker's JSON-lines output.

    Seeds are made strings here, once, for every worker: several emit them as
    JSON numbers, and the browser's JSON.parse rounds anything past 2^53 -- a
    64-bit seed then arrives as a different seed that does not have the find.
    """
    hits, summary = [], {}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if not isinstance(obj, dict):
            continue
        if "seed" in obj and obj["seed"] is not None:
            obj["seed"] = str(obj["seed"])
        if obj.get("summary"):
            summary = obj
        else:
            hits.append(obj)
    return hits, summary


def _worker_ok(rc, err, hits, what, ok=(0,)):
    """Raise when a worker CRASHED rather than finished empty.

    A JVM that died on startup and a search that honestly found nothing both
    return no hits; reporting the first as the second tells the user the seeds
    do not exist when the tool never looked. `ok` is the worker's own set of
    "finished" exit codes (the hunt scripts use 1 for "found nothing"); a
    Python traceback is a crash whatever the code, since an uncaught exception
    also exits 1.
    """
    if hits:
        return
    if rc in ok and "Traceback (most recent call last)" not in (err or ""):
        return
    tail = " / ".join((err or "").strip().splitlines()[-3:])
    raise Failure(f"{what} failed (exit {rc})" + (f": {tail}" if tail else ""))


def api_villagesmiths(body) -> dict:
    """Tier-2 search: run the real-Minecraft worldgen backend to find seeds with
    a village holding >= N smith buildings. Slow (real jigsaw generation), and
    MC 1.16.5 only -- a different engine from the cubiomes finder above."""
    tier2 = ROOT / "tier2"
    if not (tier2 / "out" / "VillageWorldgen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("village worldgen backend not built -- see tier2/README.md "
                      "(gradle printcp -> cp.txt, then javac the worker)")
    mn = max(1, min(12, int(body.get("min", 5))))
    radius = max(200, min(4000, int(body.get("radius", 1200))))
    seeds = max(1, min(3000, int(body.get("seeds", 200))))
    workers = max(1, min(8, int(body.get("workers", 4))))
    limit = max(1, min(100, int(body.get("limit", 20))))
    start = int(body.get("start", 1))
    building = re.sub(r"[^a-z_]", "", str(body.get("building", "smith")).lower()) or "smith"
    args = [sys.executable, str(tier2 / "village_search.py"),
            "--min", str(mn), "--radius", str(radius), "--seeds", str(seeds),
            "--start", str(start), "--workers", str(workers), "--limit", str(limit),
            "--building", building]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits, summary = _jsonl(out)
    _worker_ok(rc, err, hits, "the worldgen worker")
    return {"hits": hits, "summary": summary}


def api_confirmcasing(body) -> dict:
    """Settle one casing lead against a REAL server. Slow (~60s), and the point.

    The fast path agrees with a real server on 88% of chests, so its hits are
    leads. Without a way to close that loop the caveat is just a disclaimer; with
    one, a reported find is actually checked.
    """
    seed = str(body.get("seed", "")).strip()
    if not re.match(r"^-?\d{1,20}$", seed):
        raise Failure("confirm needs a seed")
    try:
        x, z = int(body.get("x")), int(body.get("z"))
    except (TypeError, ValueError):
        raise Failure("confirm needs x and z")
    expect = str(body.get("expect", "")).strip()
    if expect and not re.match(r"^(minecraft:)?[a-z0-9_]{2,40}$", expect):
        raise Failure("bad block id")
    script = ROOT / "tier3-java" / "confirm_casing.py"
    if not (ROOT / "tier3-java" / "server.jar").exists():
        raise Failure("tier3-java/server.jar not present -- see tier3-java/README.md")
    args = [sys.executable, str(script), seed, str(x), str(z)]
    if expect:
        args += ["--expect", expect]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    faces = [ln.strip() for ln in out.splitlines()
             if re.match(r"^\s+(west|east|down|up|north|south)\s", ln)]
    m = re.search(r"^casing:\s*(\S+)\s*\((\d+) of 5", out, re.M)
    return {"confirmed": rc == 0 and bool(expect),
            "checked": rc in (0, 1),          # 2 = unknown, not a verdict
            "casing": m.group(1) if m else None,
            "agree": int(m.group(2)) if m else 0,
            "faces": faces, "raw": out.strip(), "error": err.strip() or None}


def api_confirmfloating(body) -> dict:
    """Is (x,z) a genuinely FLOATING island, from the game's own terrain?

    No server. tier2-outpost runs Minecraft's NoiseBasedChunkGenerator, so the
    column is an array in memory and `OreGen floatserver` reads it directly.
    That matters because the three earlier attempts all went through a running
    server's chat commands and two of them were WRONG in the dangerous
    direction: a batched sweep silently lost results, and a binary search is
    structurally invalid on a column that goes solid/air/solid, which is exactly
    what an island is.

    The condition needs confirming rather than trusting: cubiomes' density
    function called 18-24% of outposts "floating" under two earlier definitions,
    because on a hillside the ground really is absent at those heights. So this
    checks a real cap, a real void beneath it, and ground that drops away on
    nearly every side.
    """
    seed = str(body.get("seed", "")).strip()
    if not re.match(r"^-?\d{1,20}$", seed):
        raise Failure("floating check needs a seed")
    try:
        x, z = int(body.get("x")), int(body.get("z"))
    except (TypeError, ValueError):
        raise Failure("floating check needs x and z")
    gap = max(4, min(64, int(body.get("gap", 24))))
    tier2 = ROOT / "tier2-outpost"
    if not (tier2 / "out" / "OreGen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("tier2-outpost not built -- OreGen.class missing, see "
                      "tier2-outpost/README")
    script = ROOT / "tier3-java" / "verify_floating.py"
    rc, out, err = run([sys.executable, str(script), seed, str(x), str(z),
                        "--gap", str(gap)], TIMEOUTS["villagesmiths"])
    top = re.search(r"island top\s+y=(-?\d+)", out)
    cap = re.search(r"island underside y=(-?\d+)", out)
    void = re.search(r"void beneath\s+(\d+) blocks", out)
    lower = re.search(r"(\d+)/8 directions", out)
    ring = [ln.strip() for ln in out.splitlines() if re.match(r"^\s+\([+-]\d", ln)]
    # rc 2 is UNKNOWN and is NOT a "no". Reporting it as one is the exact bug
    # this whole path was rebuilt to stop doing.
    return {"floating": rc == 0,
            "checked": rc in (0, 1),
            "unknown": rc == 2,
            "top": int(top.group(1)) if top else None,
            "cap": int(cap.group(1)) if cap else None,
            "void": int(void.group(1)) if void else 0,
            "lower": int(lower.group(1)) if lower else 0,
            "ring": ring,
            "output": out.strip()[-4000:]}


def api_bedrockcasing(body) -> dict:
    """Buried treasure encased in BEDROCK. Pre-1.18 worlds only.

    Two ordinary rules meeting: the placement scan stops only where the block
    below is sandstone/stone/andesite/granite/diorite, so bedrock does NOT stop
    it -- the chest falls THROUGH the band until it finds stone, and the block at
    its own position is then bedrock, which becomes the fill and is written into
    every air face. Confirmed on a real 1.16.5 server, 5 of 6 faces bedrock.

    The rare part is the COLUMN, not the casing: a ravine has to reach from the
    ocean floor into the bedrock band at exactly the treasure's (9,9). Measured
    rarity: 0 in 202 treasures across 25 seeds. Expect to scan.

    1.18+ CANNOT do this and the version is rejected rather than searched: the
    band moved to y -64..-59 and everything above it is deepslate, which is not a
    stopping block, so the scan never terminates and no chest is placed at all.
    """
    version = check_version(str(body.get("version", "1.16")))
    if not re.match(r"^1\.(1[0-7]|[0-9])(\.|$)", version):
        raise Failure(
            f"bedrock-cased treasure is a pre-1.18 phenomenon and {version} is "
            f"not pre-1.18. From 1.18 the bedrock band is at y -64..-59 under "
            f"deepslate, which does not stop the placement scan, so no chest is "
            f"placed at all. Ask for 1.16 or older.")
    tier2 = ROOT / "tier2"
    if not (tier2 / "out" / "VillageWorldgen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("tier2 (1.16 backend) not built -- VillageWorldgen.class missing")
    seeds = max(1, min(2000, int(body.get("seeds", 40))))
    start = int(body.get("start", 1))
    radius = max(200, min(20000, int(body.get("radius", 2000))))
    args = [sys.executable, str(ROOT / "tier3-java" / "hunt_bedrock_casing.py"),
            "--seeds", str(seeds), "--start", str(start),
            "--radius", str(radius), "--version", version]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits = []
    for line in out.splitlines():
        m = re.match(r"\s+BEDROCK BAND\s+seed (-?\d+) \((-?\d+),(-?\d+)\).*?y=(-?\d+).*?at y=(-?\d+)", line)
        if m:
            hits.append({"seed": m.group(1), "x": int(m.group(2)),
                         "z": int(m.group(3)), "open_to": int(m.group(4)),
                         "chest_y": int(m.group(5)) + 1})
    _worker_ok(rc, err, hits, "the bedrock-casing hunt", ok=(0, 1))
    tot = re.search(r"(\d+) treasures in", out)
    return {"hits": hits, "checked": int(tot.group(1)) if tot else 0,
            "note": ("A hit means the COLUMN reaches the bedrock band. Whether "
                     "that chest is cased in bedrock or in the hardcoded SAND "
                     "depends on its own column's bedrock pattern -- about 1 in "
                     "5 for y=4. Confirm on a server.")}


def api_coolstuff(body) -> dict:
    """What is worth going to in a seed the user ALREADY has.

    Every other search here answers "find me a seed". This answers "I have this
    world, where is the good stuff" -- the direction Kludwisz's CoolStuffFinder
    takes. It reads the chests of every lootable structure in range and reports
    the ones clearing a per-structure notability bar, or a specific item if one
    is named.

    Enchantments and their LEVELS are read: item="golden_sword" with
    ench="looting", ench_min=3 finds a Looting III golden sword. cubiomes has
    carried these on ItemStack all along and nothing here read them until now.
    """
    seed = java_seed(body.get("seed", ""))
    version = check_version(str(body.get("version", "1.21")))
    radius = max(200, min(20000, int(body.get("radius", 3000))))
    args = [sys.executable, str(ROOT / "tier3-java" / "find_cool_stuff.py"),
            seed, "--version", version, "--radius", str(radius)]
    item = str(body.get("item", "")).strip()
    ench = str(body.get("ench", "")).strip()
    if ench:
        if not re.match(r"^[a-z_]{3,40}$", ench):
            raise Failure("enchantment must be a name, e.g. looting or sharpness")
        args += ["--ench", ench, str(max(1, min(10, int(body.get("ench_min", 1)))))]
    elif item:
        if not re.match(r"^(minecraft:)?[a-z0-9_]{2,40}$", item):
            raise Failure("item must be a block/item id, e.g. diamond")
        args += ["--item", item, "--min", str(max(1, min(64, int(body.get("min", 1)))))]
    rc, out, err = run(args, TIMEOUTS["describe"])
    finds, cur, skipped = [], None, []
    for line in out.splitlines():
        m = re.match(r"\s+([a-z_]+)\s+x=(-?\d+)\s+z=(-?\d+)\s+(.+)$", line)
        if m:
            cur = {"structure": m.group(1), "x": int(m.group(2)),
                   "z": int(m.group(3)), "why": m.group(4).strip(), "chest": ""}
            finds.append(cur)
            continue
        c = re.match(r"\s+chest: (.+)$", line)
        if c and cur is not None:
            cur["chest"] = c.group(1)
        k = re.match(r"NOT SCANNED[^:]*: (.+)$", line)
        if k:
            skipped = [x.strip() for x in k.group(1).split(",")]
    _worker_ok(rc, err, finds, "the cool-stuff finder", ok=(0, 1))
    n = re.search(r"(\d+) chests read", out)
    return {"finds": finds, "chests": int(n.group(1)) if n else 0,
            "skipped": skipped,
            "note": ("Enchantments and their levels are read -- ask for "
                     "\"looting\" at level 3 to find a Looting III sword.")}


def api_spidervillage(body) -> dict:
    """Villages with cave spider spawners beside them.

    The spawners come from mineshaft SPIDER CORRIDORS -- structure pieces, not
    decoration -- so the real 1.21.1 generator enumerates them and
    MineShaftCorridor.spiderCorridor says which corridors get a spawner.

    Distance is CHEBYSHEV in CHUNKS from the village origin, because that is how
    structure grids are actually measured.

    Corridors the game would SKIP are not counted: a mineshaft piece whose
    bounding box touches liquid is dropped outright by the generator. And only
    one mineshaft variant can exist at a place -- the biome decides -- so both
    are asked and the wrong one answers NOHIT.
    """
    tier2 = ROOT / "tier2-outpost"
    if not (tier2 / "out" / "OutpostWorldgen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("tier2-outpost not built -- OutpostWorldgen.class missing")
    mn = max(1, min(12, int(body.get("min", 2))))
    radius = max(1, min(16, int(body.get("radius", 3))))
    seeds = max(1, min(500, int(body.get("seeds", 20))))
    start = int(body.get("start", 1))
    version = check_version(str(body.get("version", "1.21")))
    args = [sys.executable, str(ROOT / "tier3-java" / "hunt_spider_village.py"),
            "--min", str(mn), "--radius", str(radius), "--seeds", str(seeds),
            "--start", str(start), "--version", version]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits, cur = [], None
    for line in out.splitlines():
        m = re.match(r"\s+SPIDER VILLAGE\s+seed (-?\d+)\s+village \((-?\d+),(-?\d+)\)\s+(\d+)", line)
        if m:
            cur = {"seed": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                   "spawners": int(m.group(4)), "boxes": []}
            hits.append(cur)
            continue
        b = re.match(r"\s+(mineshaft\S*)\s+box (-?\d+),(-?\d+),(-?\d+) \.\. (-?\d+),(-?\d+),(-?\d+)", line)
        if b and cur is not None:
            cur["boxes"].append({"variant": b.group(1),
                                 "min": [int(b.group(i)) for i in (2, 3, 4)],
                                 "max": [int(b.group(i)) for i in (5, 6, 7)]})
    _worker_ok(rc, err, hits, "the spider-village hunt", ok=(0, 1))
    tot = re.search(r"(\d+) villages checked", out)
    return {"hits": hits, "checked": int(tot.group(1)) if tot else 0,
            "note": ("Spawners that EXIST. Whether one is reachable or "
                     "surface-exposed is a terrain question this does not "
                     "answer -- ordinary mineshafts sit near y -30.")}


def api_fortressoverlap(body) -> dict:
    """Rank seeds by how much neighbouring nether fortresses GROW THROUGH each other.

    "Four fortresses close together" is the wrong measure and it is the one a
    cluster search gives. A fortress is up to 257 pieces sprawling 112 blocks
    from its start, and each is generated without knowledge of the others -- so
    two starts 80 blocks apart do not sit near each other, they interpenetrate.
    This measures intersecting piece pairs, which is what "inside each other"
    actually means.
    """
    seed = str(body.get("seed", "")).strip()
    version = check_version(str(body.get("version", "1.21")))
    if seed:
        if not re.match(r"^-?\d{1,20}$", seed):
            raise Failure("bad seed")
        rc, out, err = run([tool("fortoverlap"), seed, version], TIMEOUTS["describe"])
    else:
        seeds = max(1000, min(5_000_000, int(body.get("seeds", 200000))))
        rc, out, err = run([tool("fortoverlap"), "0", version, "--rank", str(seeds)],
                           TIMEOUTS["search"])
    if rc != 0 and not out.strip():
        raise Failure(err.strip() or "fortoverlap failed")
    pairs, ranked = [], []
    for ln in out.splitlines():
        m = re.search(r"fortress (\d+) x (\d+)\s*:\s*(\d+) piece pairs intersect, (\d+) blocks", ln)
        if m:
            pairs.append({"a": int(m.group(1)), "b": int(m.group(2)),
                          "pairs": int(m.group(3)), "blocks": int(m.group(4))})
        # Written against the tool's real output rather than an assumed column
        # layout: "seed 15500  3 fortresses  83 intersecting piece pairs  5254
        # shared blocks". A guessed format parsed to an empty list and would have
        # rendered as "no results" rather than as a parse failure.
        m = re.match(r"^seed\s+(-?\d+)\s+(\d+) fortresses\s+(\d+) intersecting "
                     r"piece pairs\s+(\d+) shared blocks", ln.strip())
        if m:
            ranked.append({"seed": m.group(1), "fortresses": int(m.group(2)),
                           "pairs": int(m.group(3)), "blocks": int(m.group(4))})
    m = re.search(r"(\d+) real fortresses, (\d+) intersecting piece pairs, (\d+) blocks", out)
    summary = {"fortresses": int(m.group(1)), "pairs": int(m.group(2)),
               "blocks": int(m.group(3))} if m else {}
    return {"pairs": pairs, "ranked": ranked, "summary": summary, "raw": out.strip()}


def api_bedrockoutpost(body) -> dict:
    """Bedrock outposts standing on a tall COBBLESTONE foundation.

    This is the thing the prompt used to call impossible. It is impossible in
    JAVA -- Java drops a small dirt platform under a structure that lands in air
    -- but Bedrock fills a foundation down to the ground out of the structure's
    own material, so a watchtower on a cliff edge grows a cobblestone column
    beneath it. Those are the screenshots, and tier3-bedrock finds them.

    Saying "not in Java, here is a mountain outpost instead" was worse than
    unhelpful: a Java seed does not transfer, because Bedrock places structures
    differently, so the substitute answered a question nobody asked.
    """
    tier = ROOT / "tier3-bedrock"
    if not (tier / "server" / "bedrock_server.exe").exists():
        raise Failure("Bedrock Dedicated Server not unpacked -- see "
                      "tier3-bedrock (unzip bds.zip into tier3-bedrock/server). "
                      "This search runs a real Bedrock server; nothing else can "
                      "answer it.")
    seeds = str(body.get("seeds", "")).strip() or "12345"
    if not re.match(r"^-?\d{1,20}(,-?\d{1,20})*$", seeds):
        raise Failure("seeds must be a comma-separated list of numbers")
    min_drop = max(4, min(120, int(body.get("min_drop", 20))))
    reach = max(200, min(10000, int(body.get("reach", 1500))))
    verify = max(1, min(10, int(body.get("verify_top", 2))))
    args = [sys.executable, str(tier / "glitched_outposts.py"),
            "--seeds", seeds, "--min-drop", str(min_drop),
            "--reach", str(reach), "--verify-top", str(verify)]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits = []
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            hits.append(json.loads(line))
        except ValueError:
            continue
    if rc != 0 and not hits:
        raise Failure(err.strip() or "bedrock outpost search failed")
    # cobblestone_height is measured block by block in the real world, so a hit
    # is confirmed rather than predicted -- unlike predicted_drop, which is only
    # the terrain prefilter that decided the candidate was worth probing.
    hits.sort(key=lambda h: -(h.get("cobblestone_height") or 0))
    return {"hits": hits, "raw": out.strip()[-4000:],
            "note": "cobblestone_height is measured in the real Bedrock world, "
                    "block by block. predicted_drop is only the terrain "
                    "prefilter that chose the candidate."}


def api_structoverlap(body) -> dict:
    """Do two structures ACTUALLY share space, piece by piece?

    The `overlap` condition compares nominal boxes -- a good candidate filter and
    a poor answer. This intersects the structures' real piece bounding boxes, so
    it is arithmetic rather than inference. It is exact where cubiomes can
    assemble a structure (fortresses, end cities) and returns UNKNOWN where it
    cannot (mansions and most overworld builds), which is deliberately not the
    same as reporting that they do not overlap.
    """
    seed = str(body.get("seed", "")).strip()
    if not re.match(r"^-?\d{1,20}$", seed):
        raise Failure("structoverlap needs a seed")
    version = check_version(str(body.get("version", "1.21")))
    try:
        a = str(body["a"]); ax = int(body["ax"]); az = int(body["az"])
        b = str(body["b"]); bx = int(body["bx"]); bz = int(body["bz"])
    except (KeyError, TypeError, ValueError):
        raise Failure("needs a/ax/az and b/bx/bz")
    for n in (a, b):
        if not re.match(r"^[a-z_]{3,30}$", n):
            raise Failure(f"bad structure name {n!r}")
    rc, out, err = run([tool("structoverlap"), seed, version,
                        a, str(ax), str(az), b, str(bx), str(bz)],
                       TIMEOUTS["describe"])
    txt = out.strip()
    engine = "cubiomes"
    # cubiomes only ships piece generators for fortress and end_city, so it
    # answers UNKNOWN for mansion -- which is the case that motivated this whole
    # tool, since a mansion sprawls far past its origin and origin-to-origin
    # distance says nothing useful about it. Rather than reimplement mansion
    # layout, fall through to the REAL 1.21.1 generator, which is right by
    # construction and works for every structure instead of one more special
    # case. Slower (a JVM per structure), so it is a fallback and not the path.
    if ("UNKNOWN" in txt or "ERROR" in txt) and version.startswith("1.21"):
        alt = ROOT / "tier3-java" / "piece_overlap.py"
        if (ROOT / "tier2-outpost" / "out" / "OutpostWorldgen.class").exists():
            rc2, out2, err2 = run([sys.executable, str(alt), seed,
                                   a, str(ax), str(az), b, str(bx), str(bz)],
                                  TIMEOUTS["villagesmiths"])
            if out2.strip():
                txt = out2.strip()
                engine = "real 1.21.1 generator"
    m = re.search(r"OVERLAP: (\d+) piece pairs intersect, (\d+) blocks", txt)
    gap = re.search(r"nearest\s+pieces are (\d+) blocks apart", txt)
    pcs = re.findall(r"^\s+\S+: (-?\d+) pieces$", txt, re.M)
    return {
        "verdict": ("overlap" if m else
                    "unknown" if ("UNKNOWN" in txt or "ERROR" in txt) else
                    "separate"),
        "pairs": int(m.group(1)) if m else 0,
        "blocks": int(m.group(2)) if m else 0,
        "gap": int(gap.group(1)) if gap else None,
        "pieces": [int(x) for x in pcs],
        "raw": txt,
        "engine": engine,
        # An engine that cannot assemble a structure must not be read as one
        # that checked and found nothing.
        "note": ("Piece-exact. cubiomes can assemble fortresses and end cities; "
                 "for other structures this returns UNKNOWN rather than a "
                 "verdict, which is not the same as 'they do not overlap'."),
    }


def api_casingtreasure(body) -> dict:
    """Find a buried treasure ENCASED in a given block (magma_block, iron_ore...).

    This is the one thing the main search genuinely cannot do. find.exe runs on
    cubiomes, which has exact terrain but no surface rules and no decoration --
    so it has no sand column, and no ore or magma anywhere. The casing is a
    property of the finished world, so it is not slow to evaluate there, it is
    absent. Tier 1 streams treasure candidates and OreGen decides.

    Rates re-measured over 8057 chests, scoring all six FINAL faces and running
    placement before decoration as the game does:

        sand 27390 faces   sandstone 10396   gravel 5904   stone 2769
        dirt 1065          diorite 247       andesite 209  granite 208
        copper_ore 39      coal_ore 30       iron_ore 5    lapis_ore 4

    60 of 8057 chests carry ore on at least one face -- 0.74%, not the 0.015%
    quoted here before. That old figure came from a census that scored ONE block
    (the fill) against a decorator that placed no ore at all; both were bugs.

    Note what the corrected numbers imply for this search: with placement before
    decoration, the fill can never be ore, so matching on `fill` returns nothing
    forever. Ore reaches a chest only as standing terrain the piece never
    touched, or as decoration writing over the casing afterwards -- both of
    which are only visible in the six final faces.
    """
    tier2 = ROOT / "tier2-outpost"
    if not (tier2 / "out" / "OreGen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("OreGen not built -- see tier2-outpost "
                      "(gradle printcp, then javac OreGen.java)")
    casing = str(body.get("casing", "")).strip()
    if not re.match(r"^(minecraft:)?[a-z0-9_]{2,40}$", casing):
        raise Failure("casing must be a block id, e.g. magma_block or iron_ore")
    rng = max(100000, min(200_000_000, int(body.get("range", 3_000_000))))
    limit = max(1, min(30, int(body.get("limit", 3))))
    version = check_version(str(body.get("version", "1.21")))
    # How far out in EACH world to look. find.exe reports only the first
    # treasure per seed, so without expanding a seed the search examines one
    # chest per world -- which for a 0.015% casing is glancing, not searching.
    radius = max(1000, min(60000, int(body.get("radius", 10000))))
    args = [sys.executable, str(tier2 / "treasure_search.py"),
            "--version", version, "--range", str(rng), "--limit", str(limit),
            "--casing", casing, "--radius", str(radius),
            "--workers", str(max(1, min(8, int(body.get("workers", 4)))))]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits, summary = [], {}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        (summary.update(obj) if obj.get("summary") else hits.append(obj))
    if rc != 0 and not hits:
        raise Failure(err.strip() or "casing search failed")
    return {"hits": hits, "summary": summary,
            # Never presented as settled. OreGen agrees with the real server on
            # 88% of chests, so a hit is a lead worth confirming and a miss is
            # not evidence that none exists.
            "confirm": "OreGen agrees with a real server on 88% of chests -- "
                       "confirm a hit with tier3-java/treasure_casing.py before "
                       "trusting it, and read an empty result as 'not found in "
                       "what was scanned', not as 'none exists'."}


def api_exposedtreasure(body) -> dict:
    """Tier-2 search: find buried treasures whose chest is TOUCHING AIR (on land,
    not underwater). Tier 1 (cubiomes) can't see water, so the headless MC 1.21.1
    worker reads the real block above each chest. Slow (JVM bootstrap)."""
    tier2 = ROOT / "tier2-outpost"
    if not (tier2 / "out" / "OutpostWorldgen.class").exists() or not (tier2 / "cp.txt").exists():
        raise Failure("outpost/treasure worldgen backend not built -- see tier2-outpost "
                      "(gradle printcp, then javac OutpostWorldgen.java)")
    rng = max(100000, min(200_000_000, int(body.get("range", 3_000_000))))
    limit = max(1, min(30, int(body.get("limit", 8))))
    version = check_version(str(body.get("version", "1.21")))
    args = [sys.executable, str(tier2 / "treasure_search.py"),
            "--version", version, "--range", str(rng), "--limit", str(limit),
            "--workers", "6"]
    rc, out, err = run(args, TIMEOUTS["villagesmiths"])
    hits, summary = _jsonl(out)
    _worker_ok(rc, err, hits, "the treasure worker")
    return {"hits": hits, "summary": summary}


# Open-ended hunts, keyed by job id. A batch search answers "is it in the first
# N seeds", which is a different question from "find me one", and for anything
# rare the answer to the first is no. These keep going.
HUNTS = {}
HUNTS_LOCK = threading.Lock()


def _hunt_state(job):
    with HUNTS_LOCK:
        return dict(HUNTS.get(job) or {})


HUNT_IDLE_SECONDS = 600


def _hunt_loop(job, path, batch, want, threads, secs, casing="", radius=10000,
               uppers=0):
    """Run a hunt; whatever way it ends, mark it stopped and drop its query."""
    try:
        _hunt_batches(job, path, batch, want, threads, secs, casing, radius, uppers)
    finally:
        with HUNTS_LOCK:
            if job in HUNTS:
                HUNTS[job]["running"] = False
        if not casing:
            drop_query(path)


def _hunt_batches(job, path, batch, want, threads, secs, casing="", radius=10000,
                  uppers=0):
    """Scan batch after batch, advancing FIND_OFFSET, until told to stop.

    Every batch covers seeds the previous ones did not: find.exe walks an index
    through mix64, and FIND_OFFSET moves where that walk starts. Without it each
    pass re-scanned indices 0..range, so "search longer" bought the same seeds
    over again -- which is the actual reason rare things were never found rather
    than any shortage of patience.
    """
    off = 0
    while True:
        with HUNTS_LOCK:
            st = HUNTS.get(job)
            if not st or st.get("stop"):
                break
            if want and len(st["hits"]) >= want:
                break
            # Nobody has asked about this hunt in a while: the tab was closed,
            # or the browser crashed. A hunt with want=0 never ends on its own,
            # and an unwatched one would otherwise run until the server dies,
            # on a machine someone is working on.
            if time.time() - st.get("last_poll", time.time()) > HUNT_IDLE_SECONDS:
                st["error"] = (f"stopped: no one checked on this hunt for "
                               f"{HUNT_IDLE_SECONDS // 60} minutes")
                break
        env = find_env()
        env["FIND_OFFSET"] = str(off)
        env["FIND_SECONDS"] = str(secs)
        if uppers:
            env["FIND_UPPERS"] = str(uppers)
        try:
            if casing:
                # Same loop, different worker. A rare casing is the case that
                # NEEDS resuming: at ~0.015% one batch is nowhere near enough,
                # and without an offset a second batch would rescan the first.
                cmd = [sys.executable, str(ROOT / "tier2-outpost" / "treasure_search.py"),
                       "--casing", casing, "--range", str(batch),
                       "--offset", str(off), "--limit", str(max(1, want)),
                       "--radius", str(radius), "--workers", "4"]
            else:
                cmd = [str(tool("find")), str(path), str(batch), str(threads)]
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                encoding="utf-8", errors="replace", cwd=str(ROOT), env=env)
            job_register(job, proc)
            try:
                out, _ = proc.communicate(timeout=secs + 900 if casing else secs + 120)
            except subprocess.TimeoutExpired:
                # communicate() does not kill on timeout, and job_done below
                # forgets the process -- so it would run on, unreachable.
                _kill_tree(proc)
                raise
        except Exception as e:
            with HUNTS_LOCK:
                if job in HUNTS:
                    HUNTS[job]["error"] = str(e)
            return
        finally:
            job_done(job)

        if casing:
            # treasure_search emits one JSON object per hit, plus a summary.
            found = []
            for ln in out.splitlines():
                ln = ln.strip()
                if not ln:
                    continue
                try:
                    o = json.loads(ln)
                except ValueError:
                    continue
                if not o.get("summary"):
                    found.append(o)
            # EVERY casing candidate is confirmed on a real server before it is
            # reported. cubiomes can place a treasure the game does not -- a
            # magma "hit" at (-2103,20,6969) had no chest in the column at all --
            # so an unconfirmed hit is a claim the tool cannot back. Candidates
            # are rare, so this costs a server boot per hit rather than per chest.
            confirmed, rejected, unknown = [], 0, 0
            for h in found:
                with HUNTS_LOCK:
                    if (HUNTS.get(job) or {}).get("stop"):
                        break               # a confirm can take 20 min; honour Stop
                try:
                    rc, cout, _ = run(
                        [sys.executable, str(ROOT / "tier3-java" / "confirm_casing.py"),
                         str(h.get("seed")), str(h.get("x")), str(h.get("z")),
                         "--expect", casing], 1200)
                    if rc == 0:
                        h["confirmed"] = True
                        m = re.search(r"^casing:\s*(\S+)\s*\((\d+) of 5", cout, re.M)
                        if m:
                            h["faces"] = int(m.group(2))
                        confirmed.append(h)
                    elif rc == 1:
                        rejected += 1       # the server looked and said no
                    else:
                        unknown += 1        # 2 = chunk would not load: UNKNOWN
                except Exception:  # noqa: BLE001
                    unknown += 1            # the confirmer itself failed
            # Unconfirmed candidates are dropped, never reported -- but "the real
            # server said no" and "we could not ask" are counted apart. Lumping
            # them together made a missing server.jar look like 40 rejections.
            with HUNTS_LOCK:
                st = HUNTS.get(job)
                if st:
                    st["candidates"] = st.get("candidates", 0) + len(found)
                    st["rejected"] = st.get("rejected", 0) + rejected
                    st["unknown"] = st.get("unknown", 0) + unknown
            found = confirmed
        else:
            found = parse_hits(out) or []
        with HUNTS_LOCK:
            st = HUNTS.get(job)
            if not st:
                return
            if st.get("stop"):
                st["running"] = False
                return
            seen = st["seen"]
            for h in found:
                h["seed"] = str(h.get("seed"))     # never a JSON number: > 2^53
                s = h["seed"]
                if s not in seen:
                    seen.add(s)
                    st["hits"].append(h)
            st["batches"] += 1
            # Advance to where find says it actually got to. Under a time budget
            # it stops part-way through the batch, and advancing by the whole
            # batch skipped every seed it never reached -- while "scanned"
            # claimed them. find.exe prints the gap-free resume point; a casing
            # batch (a different worker) still advances by the batch.
            m = re.search(r"resume with FIND_OFFSET=(\d+)", out or "")
            nxt = int(m.group(1)) if (m and not casing) else off + batch
            if nxt <= off:
                nxt = off + 1          # never loop on the same range forever
            st["offset"] = nxt
            # Reported so a quiet hunt is legible: "nothing yet" and "not
            # actually scanning" look identical from outside otherwise.
            st["scanned"] += nxt - off
        off = nxt


def api_hunt(body) -> dict:
    """Start an open-ended hunt. Returns immediately; poll /api/hunt/status.

    The batch search behind /api/search is bounded twice over -- by `range` and
    by an HTTP timeout -- so it can only ever report "not in the first N". For
    anything at the rates actually measured here (an ore-cased buried treasure
    is ~0.015%) that bound is the whole reason nothing turns up.
    """
    casing = str(body.get("casing", "")).strip()
    if casing and not re.match(r"^(minecraft:)?[a-z0-9_]{2,40}$", casing):
        raise Failure("casing must be a block id, e.g. magma_block or iron_ore")
    job = str(body.get("job", "")).strip()
    if not job:
        raise Failure("hunt needs a 'job' id so it can be polled and stopped")
    batch = max(10_000, min(50_000_000, int(body.get("batch", 2_000_000))))
    want = max(0, int(body.get("want", 1)))
    threads = clamp_threads(body)
    secs = max(5, min(600, int(body.get("secs", 60))))
    radius = max(1000, min(60000, int(body.get("radius", 10000))))
    uppers = max(0, min(65536, int(body.get("uppers", 0) or 0)))
    if casing and not (ROOT / "tier3-java" / "server.jar").exists():
        # Every casing candidate is confirmed on a real server. Without one,
        # each would fail to confirm and the hunt would report a long run of
        # "rejections" that were really "could not ask".
        raise Failure("a casing hunt confirms every hit on a real Minecraft "
                      "server, and tier3-java/server.jar is missing -- see "
                      "tier3-java/README.md for the download")
    with HUNTS_LOCK:
        if job in HUNTS and HUNTS[job].get("running"):
            raise Failure(f"hunt {job} is already running")
    # A casing hunt supplies its own query (buried treasure near origin), so the
    # builder's query is neither needed nor meaningful for it. Everything else
    # gets its OWN query file, which the hunt re-reads every batch -- no other
    # request can change what it is searching for.
    path = None if casing else write_query(body)
    with HUNTS_LOCK:
        HUNTS[job] = {"hits": [], "seen": set(), "scanned": 0, "offset": 0,
                      "batches": 0, "running": True, "stop": False, "error": None,
                      "candidates": 0, "rejected": 0, "unknown": 0,
                      "last_poll": time.time()}
    threading.Thread(target=_hunt_loop,
                     args=(job, path, batch, want, threads, secs, casing, radius,
                           uppers),
                     daemon=True).start()
    return {"job": job, "started": True, "batch": batch, "want": want,
            "casing": casing or None}


def api_hunt_status(body) -> dict:
    job = str(body.get("job", "")).strip()
    with HUNTS_LOCK:
        if job in HUNTS:
            HUNTS[job]["last_poll"] = time.time()   # someone is still watching
    st = _hunt_state(job)
    if not st:
        return {"job": job, "known": False}
    return {"job": job, "known": True, "running": st.get("running", False),
            "hits": st.get("hits", []), "scanned": st.get("scanned", 0),
            "batches": st.get("batches", 0), "offset": st.get("offset", 0),
            # A casing hunt reports its funnel, because "0 found" after 40
            # candidates all failing confirmation means something very different
            # from "0 found" after no candidates at all.
            "candidates": st.get("candidates", 0),
            "rejected": st.get("rejected", 0),
            "unknown": st.get("unknown", 0),
            "error": st.get("error")}


def api_hunt_stop(body) -> dict:
    job = str(body.get("job", "")).strip()
    with HUNTS_LOCK:
        st = HUNTS.get(job)
        if st:
            st["stop"] = True
    api_cancel({"job": job})       # kill the batch currently in flight
    return {"job": job, "stopping": True}


ROUTES = {"/api/plan": api_plan, "/api/explain": api_explain,
          "/api/search": api_search, "/api/describe": api_describe,
          "/api/ask": api_ask, "/api/gemini": api_gemini,
          "/api/aitest": api_aitest,
          "/api/villagesmiths": api_villagesmiths,
          "/api/exposedtreasure": api_exposedtreasure,
          "/api/casingtreasure": api_casingtreasure,
          "/api/confirmcasing": api_confirmcasing,
          "/api/confirmfloating": api_confirmfloating,
          "/api/spidervillage": api_spidervillage,
          "/api/coolstuff": api_coolstuff,
          "/api/bedrockcasing": api_bedrockcasing,
          "/api/fortressoverlap": api_fortressoverlap,
          "/api/bedrockoutpost": api_bedrockoutpost,
          "/api/structoverlap": api_structoverlap,
          "/api/cancel": api_cancel,
          "/api/check": api_check,
          "/api/hunt": api_hunt,
          "/api/hunt/status": api_hunt_status,
          "/api/hunt/stop": api_hunt_stop}


ALLOWED_HOSTS = {f"127.0.0.1:{PORT}", f"localhost:{PORT}", f"[::1]:{PORT}"}
ALLOWED_ORIGINS = {"http://" + h for h in ALLOWED_HOSTS}
MAX_BODY = 1 << 20      # a query is a few KB; nothing legitimate is near this


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))

    def _foreign(self, post: bool) -> str:
        """Why this request must be refused, or "" if it may go ahead.

        Binding to loopback keeps other MACHINES out -- not other web pages.
        Any site open in the browser can send requests to 127.0.0.1: a
        text/plain POST needs no CORS preflight, so a page could start a
        six-hour search, a hunt, or a real Minecraft server without ever
        reading the reply; and DNS rebinding (a hostile name re-pointed at
        127.0.0.1) can read the replies as well. So:
          - Host must name this server, which defeats rebinding;
          - a cross-site Origin or Sec-Fetch-Site is refused;
          - a POST body must be JSON, which a page on another origin can only
            send after a preflight this server never approves.
        curl and the UI itself pass all three untouched.
        """
        host = (self.headers.get("Host") or "").strip().lower()
        if host not in ALLOWED_HOSTS:
            return f"unexpected Host {host!r}"
        origin = self.headers.get("Origin")
        if origin is not None and origin.strip().lower().rstrip("/") not in ALLOWED_ORIGINS:
            return f"cross-origin request from {origin!r}"
        site = (self.headers.get("Sec-Fetch-Site") or "").strip().lower()
        if site and site not in ("same-origin", "none"):
            return f"cross-site request ({site})"
        if post:
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if ctype != "application/json":
                return "POST bodies must be application/json"
        return ""

    def _read_body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise Failure("bad Content-Length")
        if n < 0 or n > MAX_BODY:
            raise Failure(f"request body must be 0..{MAX_BODY} bytes")
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            raise Failure("request body is not valid JSON")
        if not isinstance(body, dict):
            raise Failure("request body must be a JSON object")
        return body

    def _send(self, code, payload, ctype="application/json"):
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_asset(self, rel):
        # Resolve and confine to ASSETS; reject any traversal.
        try:
            target = (ASSETS / rel).resolve()
            target.relative_to(ASSETS.resolve())
        except (ValueError, OSError):
            self._send(404, {"error": "not found"}); return
        if not target.is_file():
            self._send(404, {"error": "not found"}); return
        ct = {"png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg",
              "gif": "image/gif", "webp": "image/webp"}.get(
              target.suffix.lstrip(".").lower(), "application/octet-stream")
        self._send(200, target.read_bytes(), ct)

    def do_GET(self):
        u = urlparse(self.path)
        # The page and its images are harmless to anyone (and following a link
        # here is a cross-site navigation), so only the API is gated -- except
        # the Host check, which applies everywhere: it is what stops rebinding.
        why = self._foreign(post=False)
        if why and (u.path.startswith("/api/") or why.startswith("unexpected Host")):
            self._send(403, {"error": f"refused: {why}"}); return
        try:
            if u.path in ("/", "/index.html"):
                if not UI.exists():
                    raise Failure("ui/index.html missing")
                self._send(200, UI.read_bytes(), "text/html; charset=utf-8")
            elif u.path == "/api/vocab":
                self._send(200, api_vocab(parse_qs(u.query)))
            elif u.path == "/api/map":
                self._send(200, api_map(parse_qs(u.query)), "image/png")
            elif u.path == "/api/markers":
                self._send(200, api_markers(parse_qs(u.query)))
            elif u.path == "/api/lootitems":
                self._send(200, api_lootitems(parse_qs(u.query)))
            elif u.path == "/api/assets":
                self._send(200, api_assets())
            elif u.path.startswith("/assets/"):
                self._serve_asset(u.path[len("/assets/"):])
            else:
                self._send(404, {"error": "not found"})
        except Failure as e:
            self._send(400, {"error": str(e)})
        except Exception as e:  # noqa: BLE001 - surface anything to the browser
            self._send(500, {"error": f"{type(e).__name__}: {e}"})

    def _write_line(self, obj):
        self.wfile.write((json.dumps(obj) + "\n").encode())
        self.wfile.flush()

    def _stream_search(self, body):
        """Run find.exe in stream mode and forward it to the browser as
        newline-delimited JSON: {"t": seed} per heartbeat while scanning, then a
        final {"done": {...}} (or {"error": ...}). Lets the UI show seeds scroll
        past live. Uses connection-close framing, read by the browser via a
        streaming fetch()."""
        try:
            find = tool("find")
            path = write_query(body)
        except Failure as e:
            self._send(400, {"error": str(e)}); return
        rng = str(max(1, min(500_000_000, int(body.get("range", 3_000_000)))))
        threads = str(clamp_threads(body))
        env = find_env({"FIND_STREAM": "1"})
        # A time budget lets the caller say "look for ten minutes" instead of
        # guessing a seed count, which is the honest unit for a leaderboard:
        # how far a range gets you depends entirely on the query.
        seconds = float(body.get("seconds", 0) or 0)
        seconds = max(0.0, min(seconds, MAX_SEARCH_SECONDS))
        # With no budget the search used to run until the watchdog KILLED it at
        # TIMEOUTS["search"] -- and a killed find.exe prints no results and no
        # funnel, so a slow search that had found seeds was reported as "no
        # seeds in that range". Giving find a budget just inside the watchdog
        # lets it stop itself and report what it has; the funnel then says the
        # budget ran out instead of pretending the range was exhausted.
        budget = seconds or max(5.0, TIMEOUTS["search"] - 20.0)
        env["FIND_SECONDS"] = str(budget)
        # How many of a structure seed's 65536 sister world seeds to try before
        # moving on. The default of 64 is right when the biome half of a query is
        # common and badly wrong when it is rare -- the answer then lives in a
        # sister never tried, and the search reports nothing found. It was
        # reachable only as an environment variable, so the planner could say
        # "you may need more sisters" and a web user could do nothing about it.
        uppers = int(body.get("uppers", 0) or 0)
        if uppers:
            env["FIND_UPPERS"] = str(max(1, min(65536, uppers)))
        # "Continue with the next seeds": start where the last run's coverage
        # ended (find prints it), so nothing is scanned twice and nothing skipped.
        offset = max(0, int(body.get("offset", 0) or 0))
        if offset:
            env["FIND_OFFSET"] = str(offset)

        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        p = subprocess.Popen([str(find), str(path), rng, threads],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace",
                             cwd=str(ROOT), env=env, bufsize=1)
        job = str(body.get("job", "")).strip()
        job_register(job, p)
        # First line: how long find was given, so the progress line can say
        # when the run will end -- and that it will end there if the user set
        # no budget of their own.
        try:
            self._write_line({"budget": budget, "default_budget": not seconds,
                              "offset": offset})
        except (BrokenPipeError, ConnectionError, OSError):
            _kill_tree(p); job_done(job); drop_query(path); return
        # The watchdog is a backstop now (find stops itself at its budget), and
        # it has to outlast that budget or it would kill the search just as it
        # was about to report. If it does fire, the reply says so.
        def _watchdog_fire():
            p.sc_watchdog = True
            _kill_tree(p)
        watchdog = threading.Timer(budget + 60, _watchdog_fire)
        watchdog.start()
        buf, streamed = [], []
        try:
            for line in p.stdout:
                if line.startswith("@TICK "):
                    parts = line[6:].split()
                    msg = {"t": parts[0]}
                    if len(parts) > 1:
                        msg["n"] = parts[1]   # total seeds scanned so far
                    self._write_line(msg)
                    continue
                buf.append(line)
                # find prints each hit the moment it is found, as an @JSON
                # line (FIND_JSON). Forward each one: results appear while the
                # scan runs, and survive a Stop -- before, they existed only
                # in the final report.
                if line.startswith("@JSON "):
                    try:
                        hit = json.loads(line[6:])
                    except ValueError:
                        continue
                    streamed.append(hit)
                    self._write_line({"hit": hit})
        except (BrokenPipeError, ConnectionError, OSError):
            _kill_tree(p); return
        finally:
            watchdog.cancel()
            p.wait()
            job_done(job)
            drop_query(path)
        # A cancelled search was killed mid-scan, so its funnel is partial. Say
        # it was stopped -- and hand back what it had already found, which is
        # real, rather than a result that looks like "no seeds exist".
        if getattr(p, "sc_cancelled", False):
            try:
                self._write_line({"cancelled": True, "seeds": streamed})
            except (BrokenPipeError, ConnectionError, OSError):
                pass
            return
        out = "".join(buf)
        low = out.lower()
        try:
            if "query error" in low or "plan error" in low:
                tail = out.strip().splitlines()
                self._write_line({"error": tail[-1] if tail else "search failed"})
            elif p.returncode not in (0, 3) and not getattr(p, "sc_watchdog", False):
                # 0 = found, 3 = searched and found nothing. Anything else is a
                # crash, and "no seeds" would be a false statement about the seeds.
                tail = out.strip().splitlines()[-3:]
                self._write_line({"error": f"find.exe failed (exit {p.returncode})"
                                           + (": " + " / ".join(tail) if tail else ""),
                                  "seeds": streamed})
            else:
                funnel = parse_funnel(out)
                if getattr(p, "sc_watchdog", False):
                    funnel["killed"] = True
                    funnel["timed_out"] = True
                funnel["budget_s"] = budget
                funnel["budget_was_default"] = not seconds
                m = re.search(r"resume with FIND_OFFSET=(\d+)", out)
                if m:
                    funnel["resume_offset"] = int(m.group(1))
                seeds = parse_hits(out) or streamed
                self._write_line({"done": {
                    "plan": parse_plan(out), "seeds": seeds,
                    "funnel": funnel, "notes": parse_notes(out),
                    "raw": human(out)}})
        except (BrokenPipeError, ConnectionError, OSError):
            return

    def do_POST(self):
        u = urlparse(self.path)
        why = self._foreign(post=True)
        if why:
            # The body was never read; on a kept-alive connection it would be
            # parsed as the next request line. Close instead.
            self.close_connection = True
            self._send(403, {"error": f"refused: {why}"}); return
        try:
            if u.path == "/api/stream":
                body = self._read_body()
                self._stream_search(body)
                return
            fn = ROUTES.get(u.path)
            if fn is None:
                self._send(404, {"error": "not found"}); return
            body = self._read_body()
            result = fn(body)
        except Failure as e:
            self.close_connection = True     # the body may be partly unread
            self._send(400, {"error": str(e)}); return
        except Exception as e:  # noqa: BLE001
            self.close_connection = True
            self._send(500, {"error": f"{type(e).__name__}: {e}"}); return
        # Sent outside the try: a failure while WRITING the reply must not be
        # answered with a second response on the same connection.
        try:
            self._send(200, result)
        except (BrokenPipeError, ConnectionError, OSError):
            pass


def main():
    for name in ("find", "vocab", "describe", "map", "lootitems"):
        try:
            tool(name)
        except Failure as e:
            print(f"! {e}", file=sys.stderr)
            print("  run: ./build.sh all", file=sys.stderr)
            return 1
    stale = _stale_tools()
    if stale:
        # A binary older than its sources still runs -- and silently ignores
        # any query field added since it was built, which reads as "no seeds".
        print(f"! older than their sources: {', '.join(stale)} -- run ./build.sh all",
              file=sys.stderr)

    url = f"http://127.0.0.1:{PORT}"
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)   # loopback only
    srv.daemon_threads = True
    print(f"seed finder UI -> {url}")
    print("  ctrl-c to stop\n")
    # SEED_UI_NO_BROWSER: the test suite starts this server, and every run used
    # to open a browser tab on the developer's screen.
    if not os.environ.get("SEED_UI_NO_BROWSER"):
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        _stop_everything()
    return 0


def _stale_tools():
    """Tools whose binary predates src/ or their own source file."""
    try:
        newest_src = max(p.stat().st_mtime for p in (ROOT / "src").glob("*.[ch]"))
    except ValueError:
        return []
    stale = []
    for name in ("find", "vocab", "describe", "map", "lootitems"):
        exe = BUILD / f"{name}.exe"
        srcf = ROOT / "tools" / f"{name}.c"
        if not exe.exists():
            continue
        newest = max(newest_src, srcf.stat().st_mtime if srcf.exists() else 0)
        if exe.stat().st_mtime < newest:
            stale.append(exe.name)
    return stale


def _stop_everything():
    """Kill every search and hunt still running when the server stops.

    Handler and hunt threads are daemons, so they vanish with the process --
    but the find.exe, JVM and Minecraft-server children they started do not,
    and a six-hour search would carry on pinning the CPU with nothing left that
    knew it existed.
    """
    with HUNTS_LOCK:
        for st in HUNTS.values():
            st["stop"] = True
    with JOBS_LOCK:
        procs = list(JOBS.values())
        JOBS.clear()
    for p in procs:
        p.sc_cancelled = True
        _kill_tree(p)
    if procs:
        print(f"  stopped {len(procs)} running job(s)")


if __name__ == "__main__":
    sys.exit(main())
