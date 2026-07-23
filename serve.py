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
    POST /api/search      -> the real search
    POST /api/describe    -> what is actually in a given seed
    GET  /api/map?...     -> biome map PNG for a seed
    GET  /api/assets      -> lists which structure/item icons the user has added
    GET  /assets/<path>   -> static image files (user-provided graphics)
    POST /api/ask         -> natural language -> query JSON (needs ANTHROPIC_API_KEY)
"""

import json
import os
import re
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).parent
ASSETS = ROOT / "assets"
BUILD = ROOT / "build"
UI = ROOT / "ui" / "index.html"
PORT = int(os.environ.get("SEED_UI_PORT", "8777"))

# Guard against a stray query pinning all cores for minutes.
TIMEOUTS = {"plan": 15, "explain": 90, "search": 180, "describe": 30, "map": 60,
            "villagesmiths": 900}
VERSION_RE = re.compile(r"^[0-9][0-9A-Za-z._-]{0,15}$")


class Failure(Exception):
    """A user-facing error: bad input, missing binary, tool failure."""


def tool(name: str) -> Path:
    exe = BUILD / f"{name}.exe"
    if not exe.exists():
        exe = BUILD / name          # non-Windows
    if not exe.exists():
        raise Failure(f"{exe.name} not built. Run: ./build.sh tools/{name}.c")
    return exe


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
    m = re.search(r"scanned\s*:\s*(\d+) structure seeds in ([\d.]+)s", out)
    if m:
        f["scanned"], f["seconds"] = int(m.group(1)), float(m.group(2))
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
        m = re.match(r"^SEED (-?\d+)", line)
        if m:
            cur = {"seed": m.group(1), "places": []}
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
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) (\w+) ore within (\d+)", line)
        if m and cur:
            cur.setdefault("ore", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "material": m.group(5), "within": int(m.group(6))})
            continue
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) slime chunks within (\d+)", line)
        if m and cur:
            cur.setdefault("slime", []).append(
                {"id": m.group(1), "x": int(m.group(2)), "z": int(m.group(3)),
                 "count": int(m.group(4)), "within": int(m.group(5))})
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
        # Structure cluster: "<id>  x=.. z=..  <N> <structure> within <R>".
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
        m = re.match(r"^\s+(\w+)\s+x=\s*(-?\d+) z=\s*(-?\d+)\s+(\d+) from (\w+)", line)
        if m and cur:
            cur["places"].append({"id": m.group(1), "x": int(m.group(2)),
                                  "z": int(m.group(3)), "dist": int(m.group(4)),
                                  "ref": m.group(5)})
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
    logo = next((f.name for f in (ASSETS.glob("logo.*") if ASSETS.is_dir() else [])
                 if f.suffix.lower() in (".png", ".webp", ".gif", ".jpg", ".jpeg")), None)
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
    rc, out, err = run([tool("describe"), seed, v, str(radius)], TIMEOUTS["describe"])
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
    out = ROOT / "build" / "tmp"
    out.mkdir(parents=True, exist_ok=True)
    # Write to a file rather than piping: PNG bytes through a pipe are easy to
    # corrupt, and a path keeps the failure mode obvious.
    dest = out / "map.png"
    rc, _, err = run([tool("map"), seed, v, str(radius), str(px), dest],
                     TIMEOUTS["map"])
    if rc != 0 or not dest.exists():
        raise Failure(err.strip() or "map render failed")
    return dest.read_bytes()


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


ROUTES = {"/api/plan": api_plan, "/api/explain": api_explain,
          "/api/search": api_search, "/api/describe": api_describe,
          "/api/ask": api_ask, "/api/villagesmiths": api_villagesmiths}


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

    def do_POST(self):
        u = urlparse(self.path)
        try:
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
