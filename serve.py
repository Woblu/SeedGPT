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
    POST /api/gemini      -> natural language -> query JSON via Google Gemini
                             (key from the request body or GEMINI_API_KEY)
"""

import json
import os
import re
import subprocess
import sys
import threading
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
    try:
        proc.terminate()
    except OSError:
        pass
    return {"cancelled": True}


def run(args, timeout, stdin=None):
    """Run a tool. No shell, so query text can never become a command."""
    try:
        p = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                           timeout=timeout, input=stdin, cwd=str(ROOT))
    except subprocess.TimeoutExpired:
        raise Failure(f"timed out after {timeout}s -- try a smaller range")
    return p.returncode, p.stdout, p.stderr


def check_version(v: str) -> str:
    if not VERSION_RE.match(v or ""):
        raise Failure(f"bad version {v!r}")
    return v


def write_query(body) -> Path:
    """Persist the query where find.exe can read it."""
    q = body.get("query")
    if not isinstance(q, dict):
        raise Failure("missing 'query' object")
    check_version(str(q.get("version", "")))
    path = ROOT / "queries" / "_ui.json"
    path.parent.mkdir(exist_ok=True)
    path.write_text(json.dumps(q, indent=2), encoding="utf-8")
    return path


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
        m = re.match(r"^\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+([a-z_]+), (\d+) blocks away,"
                     r" (\d+) overlap", line)
        if m and cur and cur.get("overlap"):
            cur["overlap"][-1].update(
                {"x2": int(m.group(1)), "z2": int(m.group(2)), "structure2": m.group(3),
                 "gap": int(m.group(4)), "area": int(m.group(5))})
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
    rc, out, err = run([tool("find"), path, *extra], timeout or TIMEOUTS[key])
    # find.exe prints "query error:" / "plan error:" and exits non-zero.
    for marker in ("query error:", "plan error:"):
        if marker in out or marker in err:
            msg = (out + err).split(marker, 1)[1].strip().splitlines()[0]
            raise Failure(msg)
    return rc, out, err


def api_plan(body) -> dict:
    path = write_query(body)
    # range=1 thread=1: prints the plan, searches a single seed. Cheap and safe.
    _, out, _ = _plan_or_fail(path, ["1", "1"])
    return {"plan": parse_plan(out), "notes": parse_notes(out), "raw": out}


def api_explain(body) -> dict:
    path = write_query(body)
    samples = int(body.get("samples", 2_000_000))
    threads = int(body.get("threads", 16))
    _, out, _ = _plan_or_fail(path, ["--explain", str(samples), str(threads)],
                              TIMEOUTS["explain"], "explain")
    return {"plan": parse_plan(out), "explain": parse_explain(out),
            "notes": parse_notes(out), "raw": out}


def api_search(body) -> dict:
    path = write_query(body)
    rng = int(body.get("range", 3_000_000))
    threads = int(body.get("threads", 16))
    _, out, _ = _plan_or_fail(path, [str(rng), str(threads)],
                              TIMEOUTS["search"], "search")
    return {"plan": parse_plan(out), "seeds": parse_seeds(out),
            "funnel": parse_funnel(out), "notes": parse_notes(out), "raw": out}


def api_describe(body) -> dict:
    seed = str(body.get("seed", "")).strip()
    if not re.match(r"^-?\d+$", seed):
        raise Failure("seed must be an integer")
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
    seed = (qs.get("seed") or [""])[0].strip()
    if not re.match(r"^-?\d+$", seed):
        raise Failure("seed must be an integer")
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
(optionally with a "rank" key -- see Leaderboard below)

Each condition is one object with a short lowercase "id". Distances ("within")
are in BLOCKS (a chunk is 16). "of" is "origin", "spawn", or another condition's
id (forming a tree). If no distance is given, choose a sensible one and say so in
notes. Do NOT order conditions for speed -- a cost planner reorders them.

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
- Biome present:  {"id","biome":<name>,"within","of"}  (of may be another biome:
    "biome B within D of biome A" = the two biomes are adjacent.)
- Biome area:     {"id","biome_area":<name>,"pct":P,"within","of"}  P = min %% of
    the disc that is this biome (a huge mushroom island, sprawling mesa).
- Island spawn:   {"id","island":true,"of":"spawn","pct":P,"within":R}  the spawn
    (default "of":"spawn") is on LAND and ocean covers >= P%% (default 75) of the
    disc of radius R (default 256) -- a small survival island. Higher pct / larger
    within = smaller, more isolated island. Use for "island / survival island /
    stranded / surrounded by ocean" spawns.
- Terrain height: {"id","height":Y,"within","of"}  APPROXIMATE peak >= Y (tall
    mountains; small radius + of a structure = "tall structure"). Overworld only.
- Terrain relief: {"id","relief":D,"within","of"[,"exact":true]}  peak-minus-valley
    drop >= D blocks in the disc = STEEP terrain. Without "exact" it uses the
    SMOOTHED estimate (cheap, but blind to sharp cliffs). With "exact":true it uses
    Minecraft's REAL block-level terrain (accurate but SLOW, 1.18+ only).
    NOTE: there is no "glitched / tall-cobblestone pillager outpost" in Java --
    Java never builds a support pillar down to the ground (that is Bedrock). If
    asked for one, say so in notes and emit a plain outpost condition, optionally
    with height/relief for a dramatically-placed one.
- Ore density:    {"id","ore":<material>,"count":N,"within","of"}  materials:
    diamond iron gold emerald redstone lapis copper coal quartz ancient_debris nether_gold
    "vein":N  -> at least N of that ore in ONE connected blob ("a big diamond
    vein", "diamonds in one spot"), which is far rarer than a scattered total.
    "exposed":true -> only count ore with a non-solid neighbour in real terrain,
    i.e. ore visible in a cave wall. 1.18+ overworld only, SLOW, radius <= 64.
- Structure overlap: {"id","overlap":[<structure A>,<structure B>],"within","of"[,"pad":P]}
    the two structures' footprints MEET -- "a ruined portal inside a village", "a
    village generated on top of a shipwreck", "two structures colliding". "pad"
    adds slack in blocks (use ~16 for "practically touching"). Footprints are
    nominal boxes, so a hit is a strong candidate, not a proof.
- Over a cave:    {"id","structure":<name>,"cave_below":N,"within","of"}  the
    structure stands on a crust above an open cavern at least N blocks tall.
    Use for "village over/above/on top of a cave", "a village in a cave", "a
    structure over a huge cavern". 1.18+ overworld, SLOW (real block terrain);
    N of 20-30 is a big cave, 40+ is dramatic. NOTE a village is never INSIDE a
    cave -- villages generate on the surface -- so this is the honest reading of
    that request, and worth saying in notes.
- Geode shape:    {"id","structure":"geode","size":N,"cracked":false,"within","of"}
    size is 3 or 4 (4 = the big one); "cracked":false = the RARE SEALED geode
    (1 in 20), "cracked":true = broken open (19 in 20, barely a filter).
- Slime chunks:   {"id","slime":N,"within","of"}  >=N slime chunks (farm site).
- Chest loot:     {"id","within","loot":{"structure":<s>,"item":<i>,"count":N}}
    loot structures: desert_pyramid jungle_temple igloo outpost shipwreck ruined_portal
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
"the BIGGEST diamond vein", "the most slime chunks", "the largest mushroom
island" -- add a sibling key next to "conditions":
  "rank": {"of": <condition id>, "by": <metric>, "top": K}
  metric: auto|height|relief|vein|count|pct|size|area  (auto = that condition's
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


def api_gemini(body) -> dict:
    """Natural language -> query JSON via Google's Gemini API.

    The key comes from the request (the UI stores it in the browser) or the
    GEMINI_API_KEY env var. We call Gemini server-side so the browser never has
    to deal with CORS, and force JSON output via responseMimeType.
    """
    text = str(body.get("text", "")).strip()
    if not text:
        raise Failure("say what you're looking for")
    key = str(body.get("key", "")).strip() or os.environ.get("GEMINI_API_KEY", "")
    if not key:
        raise Failure("no Gemini API key yet. Open settings (the dot by the search "
                      "box) and paste one -- get a free key at "
                      "https://aistudio.google.com/apikey")
    v = check_version(str(body.get("version", "1.21")))
    vocab = _vocab_for(v)
    model = re.sub(r"[^A-Za-z0-9._-]", "", str(body.get("model", "")) or GEMINI_MODEL)
    system = (AI_SCHEMA + "\n\nMinecraft version: " + vocab["version"]
              + "\n\nValid structures: " + ", ".join(vocab["structures"])
              + "\n\nValid biomes: " + ", ".join(vocab["biomes"]))
    payload = {
        "systemInstruction": {"parts": [{"text": system}]},
        "contents": [{"role": "user", "parts": [{"text": text}]}],
        "generationConfig": {"responseMimeType": "application/json", "temperature": 0.2},
    }
    url = (f"https://generativelanguage.googleapis.com/v1beta/models/{model}"
           f":generateContent?key={quote(key, safe='')}")
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            data = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        msg = raw
        try:
            msg = json.loads(raw)["error"]["message"]
        except Exception:  # noqa: BLE001
            pass
        if e.code in (400, 403) and re.search(r"api.?key|permission|invalid", msg, re.I):
            raise Failure(f"Gemini rejected the key ({e.code}). Check it in settings. [{msg}]")
        raise Failure(f"Gemini error {e.code}: {msg}")
    except urllib.error.URLError as e:
        raise Failure(f"could not reach Gemini ({e.reason}). Check your connection.")

    try:
        cand = data["candidates"][0]
        parts = cand["content"]["parts"]
        answer = "".join(p.get("text", "") for p in parts)
    except (KeyError, IndexError):
        reason = ""
        try:
            reason = data["candidates"][0].get("finishReason", "")
        except Exception:  # noqa: BLE001
            reason = (data.get("promptFeedback") or {}).get("blockReason", "")
        raise Failure(f"Gemini returned no query{f' ({reason})' if reason else ''}. Try rephrasing.")

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
    return {"query": q, "notes": obj.get("notes", "")}


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
    hits, summary = [], {}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if obj.get("summary"):
            summary = obj
        else:
            hits.append(obj)
    return {"hits": hits, "summary": summary}


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
    return {"hits": hits, "summary": summary}


ROUTES = {"/api/plan": api_plan, "/api/explain": api_explain,
          "/api/search": api_search, "/api/describe": api_describe,
          "/api/ask": api_ask, "/api/gemini": api_gemini,
          "/api/villagesmiths": api_villagesmiths,
          "/api/exposedtreasure": api_exposedtreasure,
          "/api/cancel": api_cancel}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))

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
        try:
            if u.path in ("/", "/index.html"):
                if not UI.exists():
                    raise Failure("ui/index.html missing")
                self._send(200, UI.read_bytes(), "text/html; charset=utf-8")
            elif u.path == "/api/vocab":
                self._send(200, api_vocab(parse_qs(u.query)))
            elif u.path == "/api/map":
                self._send(200, api_map(parse_qs(u.query)), "image/png")
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
        threads = str(max(1, min(64, int(body.get("threads", 16)))))
        env = dict(os.environ); env["FIND_STREAM"] = "1"
        # A time budget lets the caller say "look for ten minutes" instead of
        # guessing a seed count, which is the honest unit for a leaderboard:
        # how far a range gets you depends entirely on the query.
        seconds = float(body.get("seconds", 0) or 0)
        seconds = max(0.0, min(seconds, MAX_SEARCH_SECONDS))
        if seconds:
            env["FIND_SECONDS"] = str(seconds)

        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        p = subprocess.Popen([str(find), str(path), rng, threads],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, cwd=str(ROOT), env=env, bufsize=1)
        job = str(body.get("job", "")).strip()
        job_register(job, p)
        # The watchdog has to outlast the budget, or it would kill the search
        # just as it was about to report what it found.
        watchdog = threading.Timer(
            (seconds + 60) if seconds else TIMEOUTS["search"], p.terminate)
        watchdog.start()
        buf = []
        try:
            for line in p.stdout:
                if line.startswith("@TICK "):
                    parts = line[6:].split()
                    msg = {"t": parts[0]}
                    if len(parts) > 1:
                        msg["n"] = parts[1]   # total seeds scanned so far
                    self._write_line(msg)
                else:
                    buf.append(line)
        except (BrokenPipeError, ConnectionError, OSError):
            p.terminate(); return
        finally:
            watchdog.cancel()
            p.wait()
            job_done(job)
        # A cancelled search was killed mid-scan, so whatever it printed is a
        # partial funnel, not a result. Say it was stopped rather than reporting
        # an empty search that looks like "no seeds exist".
        if getattr(p, "sc_cancelled", False):
            try:
                self._write_line({"cancelled": True})
            except (BrokenPipeError, ConnectionError, OSError):
                pass
            return
        out = "".join(buf)
        low = out.lower()
        try:
            if "query error" in low or "plan error" in low:
                tail = out.strip().splitlines()
                self._write_line({"error": tail[-1] if tail else "search failed"})
            else:
                self._write_line({"done": {
                    "plan": parse_plan(out), "seeds": parse_seeds(out),
                    "funnel": parse_funnel(out), "notes": parse_notes(out),
                    "raw": out}})
        except (BrokenPipeError, ConnectionError, OSError):
            return

    def do_POST(self):
        u = urlparse(self.path)
        try:
            if u.path == "/api/stream":
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                self._stream_search(body)
                return
            fn = ROUTES.get(u.path)
            if fn is None:
                self._send(404, {"error": "not found"}); return
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            self._send(200, fn(body))
        except Failure as e:
            self._send(400, {"error": str(e)})
        except Exception as e:  # noqa: BLE001
            self._send(500, {"error": f"{type(e).__name__}: {e}"})


def main():
    for name in ("find", "vocab", "describe", "map", "lootitems"):
        try:
            tool(name)
        except Failure as e:
            print(f"! {e}", file=sys.stderr)
            print("  run: ./build.sh tools/find.c && ./build.sh tools/vocab.c "
                  "&& ./build.sh tools/describe.c && ./build.sh tools/map.c",
                  file=sys.stderr)
            return 1

    url = f"http://127.0.0.1:{PORT}"
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)   # loopback only
    print(f"seed finder UI -> {url}")
    print("  ctrl-c to stop\n")
    threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
