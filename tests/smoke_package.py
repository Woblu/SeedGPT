#!/usr/bin/env python3
"""Does a packaged download work the way a player will use it?

    python tests/smoke_package.py dist/seedsmith-1.0.0-linux-x86_64

Uses the package's own Python and nothing from the source tree, and locks the
folder read-only first: a download can land anywhere, and a program that writes
beside itself works on the machine it was built on and nowhere else.
"""

import json
import os
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PORT = 8795
BASE = f"http://127.0.0.1:{PORT}"


def fail(msg):
    sys.exit(f"FAIL: {msg}")


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=30) as r:
        return r.read(), r.headers.get("Content-Type", "")


def post(path, body):
    req = urllib.request.Request(BASE + path, json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def set_writable(root, on):
    for d, _, files in os.walk(root):
        for name in [d] + [os.path.join(d, f) for f in files]:
            mode = os.stat(name).st_mode
            os.chmod(name, mode | stat.S_IWUSR if on else mode & ~0o222)


def main():
    pkg = Path(sys.argv[1]).resolve()
    if os.name == "nt":
        python = pkg / "python" / "python.exe"
    else:
        python = pkg / "python" / "bin" / "python3"
    if not python.exists():
        fail(f"no bundled Python at {python}")
    # Nothing but the package: no PATH to a system Python or Java, no developer keys.
    env = {k: v for k, v in os.environ.items()
           if k.upper() in ("SYSTEMROOT", "TEMP", "TMP", "TMPDIR", "WINDIR", "HOME", "USERPROFILE")}
    env.update(SEED_UI_PORT=str(PORT), SEED_UI_NO_BROWSER="1", PYTHONDONTWRITEBYTECODE="1",
               PYTHONIOENCODING="utf-8", PATH="")
    set_writable(pkg, False)
    server = subprocess.Popen([str(python), "serve.py"], cwd=str(pkg), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(120):
            if server.poll() is not None:
                fail(f"the server exited ({server.returncode}):\n{server.stdout.read()}")
            try:
                caps = json.loads(get("/api/capabilities")[0])
                break
            except (urllib.error.URLError, OSError):
                time.sleep(0.5)
        else:
            fail("the server never answered")

        print(f"running {caps['app']} {caps['version']}")
        offered = {k for k, v in caps["tools"].items() if v}
        if offered != {"hunt", "fortress_overlap", "struct_overlap", "cool_stuff"}:
            fail(f"tools offered without their engines: {sorted(offered)}")
        for page in ("/", "/ui/app.js", "/ui/app.css", "/assets/logo.svg"):
            if not get(page)[0]:
                fail(f"{page} is empty")

        parsed = json.loads(post("/api/parse", {"text": "a mansion or a monument within 1500 "
                                                        "blocks of spawn", "version": "1.21"}))
        if not parsed.get("query") or parsed.get("ignored"):
            fail(f"the offline prompt failed: {parsed}")

        for label, query in (("from a sentence", parsed["query"]),
                             ("with loot", {"version": "1.21", "conditions": [
                                 {"id": "l", "within": 3000, "loot": {
                                     "structure": "desert_pyramid", "item": "diamond", "count": 1}}]})):
            lines = post("/api/stream", {"job": "s" + label[0], "range": 200000, "threads": 2,
                                         "query": query}).decode().splitlines()
            events = [json.loads(l) for l in lines if l.strip()]
            done = [e["done"] for e in events if "done" in e]
            if not done or not done[0].get("seeds"):
                fail(f"a search {label} found nothing: {lines[-1][:300]}")
            print(f"search {label}: {len(done[0]['seeds'])} seeds")

        png, ctype = get("/api/map?seed=12345&v=1.21&r=1000&px=128")
        if ctype != "image/png" or png[:4] != b"\x89PNG":
            fail("the map did not render")
        seed = json.loads(post("/api/describe", {"seed": "my text seed", "version": "1.21"}))
        if not seed.get("world"):
            fail("describe failed")
        cool = json.loads(post("/api/coolstuff", {"seed": "12345", "version": "1.21", "radius": 1000}))
        if "error" in cool:
            fail(f"cool stuff failed: {cool['error']}")
        print("map, describe and cool stuff work")
    except urllib.error.HTTPError as e:
        fail(f"{e.url}: {e.code} {e.read()[:300]!r}")
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        set_writable(pkg, True)
    print("package OK, from a read-only folder")


if __name__ == "__main__":
    main()
