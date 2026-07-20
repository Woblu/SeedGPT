#!/usr/bin/env python3
"""ask -- describe a world in English, get seeds.

    python ask.py "a mansion near spawn with a village next to it"
    python ask.py --version 1.16 "jungle temple in a jungle near an ocean monument"
    python ask.py --explain "quad huts"          # estimate only, don't search

Pipeline:

    English -> Claude (structured output) -> condition JSON -> C planner -> search

The model's ONLY job is translating English into a typed condition tree. It is
explicitly NOT asked to order the conditions: `find` owns the query planner and
reorders by measured cost (a structure geometry check is ~9 ns, a biome check
~31 us -- a ~3650x gap, and natural language states them in almost exactly the
wrong order). Keeping the model out of that decision is the whole architecture.

The vocabulary is read from `build/vocab.exe` for the requested version rather
than hardcoded here, so the model can only name structures and biomes the engine
actually accepts on that version.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Literal

ROOT = Path(__file__).parent
FIND = ROOT / "build" / "find.exe"
VOCAB = ROOT / "build" / "vocab.exe"

try:
    import anthropic
    from pydantic import BaseModel, Field
except ImportError:
    sys.exit("missing deps: python -m pip install anthropic pydantic")


class Condition(BaseModel):
    """One thing that must be true of the world."""

    id: str = Field(description="short lowercase identifier, e.g. 'mansion' or 'jungle'")
    kind: Literal["structure", "biome"]
    name: str = Field(description="exact name from the provided vocabulary")
    within: int = Field(description="max distance in BLOCKS from `of`")
    of: str = Field(
        description="'spawn', or the id of another condition this is measured from"
    )


class Plan(BaseModel):
    conditions: list[Condition]
    notes: str = Field(
        description="one sentence on anything you assumed or could not express"
    )


SYSTEM = """\
You translate a description of a Minecraft world into a typed condition tree.

Rules:
- Use ONLY names from the vocabulary given below. Never invent one.
- `within` is in BLOCKS. A chunk is 16 blocks. If no distance is stated, choose a
  sensible one and say so in notes. "near" is roughly 200-500 blocks; "at spawn"
  is roughly 100-200.
- `of` is either "spawn" or another condition's id, forming a tree. "a village
  next to a mansion" means the village's `of` is the mansion's id.
- Order does NOT matter. Do not try to put cheap conditions first -- a downstream
  cost-based planner reorders everything. State conditions in whatever order is
  most natural.
- Prefer fewer, tighter conditions. Every extra condition makes the search rarer.
- If the request implies something you cannot express (block types, terrain
  height, structure loot, dimensions other than the overworld), leave it out and
  say so plainly in notes. Do not approximate it with an unrelated condition.
"""


def load_vocab(version: str) -> dict:
    if not VOCAB.exists():
        sys.exit(f"{VOCAB} not found -- run: ./build.sh tools/vocab.c")
    out = subprocess.run(
        [str(VOCAB), version], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        sys.exit(f"vocab failed for version {version!r}: {out.stderr.strip()}")
    return json.loads(out.stdout)


def translate(client, description: str, vocab: dict) -> Plan:
    prompt = (
        f"Minecraft version: {vocab['version']}\n\n"
        f"Valid structures: {', '.join(vocab['structures'])}\n\n"
        f"Valid biomes: {', '.join(vocab['biomes'])}\n\n"
        f"Describe this world as conditions:\n{description}"
    )
    response = client.messages.parse(
        model="claude-opus-4-8",
        max_tokens=4096,
        system=SYSTEM,
        messages=[{"role": "user", "content": prompt}],
        output_format=Plan,
    )
    if response.parsed_output is None:
        sys.exit("model did not return a usable plan")
    return response.parsed_output


def to_query(plan: Plan, version: str) -> dict:
    return {
        "version": version,
        "conditions": [
            {
                "id": c.id,
                ("structure" if c.kind == "structure" else "biome"): c.name,
                "within": c.within,
                "of": c.of,
            }
            for c in plan.conditions
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Describe a Minecraft world, get seeds.")
    ap.add_argument("description", help="what you want, in English")
    ap.add_argument(
        "--version",
        default="1.21",
        help="Minecraft version (default 1.21). A finder cannot infer this -- "
        "it generates for the version you name.",
    )
    ap.add_argument("--range", type=int, default=5_000_000, help="structure seeds to scan")
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument(
        "--explain", action="store_true", help="estimate the funnel, don't search"
    )
    ap.add_argument("--dry-run", action="store_true", help="print the query JSON and stop")
    args = ap.parse_args()

    vocab = load_vocab(args.version)

    if not args.dry_run and not FIND.exists():
        sys.exit(f"{FIND} not found -- run: ./build.sh tools/find.c")

    client = anthropic.Anthropic()  # resolves ANTHROPIC_API_KEY or an `ant auth login` profile
    plan = translate(client, args.description, vocab)

    query = to_query(plan, args.version)
    qpath = ROOT / "queries" / "_ask.json"
    qpath.parent.mkdir(exist_ok=True)
    qpath.write_text(json.dumps(query, indent=2))

    print(f'> "{args.description}"')
    print(f"  version : {vocab['version']}")
    if plan.notes:
        print(f"  notes   : {plan.notes}")
    print(f"  query   : {qpath.relative_to(ROOT)}\n")

    if args.dry_run:
        print(json.dumps(query, indent=2))
        return 0

    cmd = (
        [str(FIND), str(qpath), "--explain", "2000000", str(args.threads)]
        if args.explain
        else [str(FIND), str(qpath), str(args.range), str(args.threads)]
    )
    return subprocess.run(cmd, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
