# Seed Cracker

A Minecraft Java Edition **seed finder**: describe the world features you want, get seeds and coordinates.

> Seed *finding* (search for a seed that **has** features you describe) is a different problem from seed *cracking* (recover the seed of a world you have data from). This is a finder. Consequently **the Minecraft version is an input, not an output** — a finder cannot infer a version it was told to generate for.

## Build

```sh
./setup.sh                 # clone + pin the engine (first time only)
./build.sh tools/find.c    # library + build/find.exe
./test.sh                  # 16-check regression suite
```

Requires `clang`, `git`, and a JDK (for the verifiers). No `make`/`ninja` needed.

`setup.sh` pins cubiomes at a specific commit (`b12a532`, MC 26.2) rather than
tracking `main`. Engine currency is the whole reason this project uses
`xpple/cubiomes`, so the version it generates against should change on purpose —
bump the pin, then re-run `./test.sh`.

## Run

Describe what you want in English:

```sh
python ask.py "a mansion near spawn with a village next to it"
python ask.py --version 1.16 "jungle temple in a jungle near an ocean monument"
python ask.py --explain "quad huts"      # estimate only, don't search
python ask.py --dry-run "..."            # print the query JSON and stop
```

`ask.py` needs `ANTHROPIC_API_KEY` (or an `ant auth login` profile) and
`python -m pip install anthropic pydantic`. Everything below works without it.

Or write the query yourself:

```sh
./build/find.exe queries/mansion-village.json 3000000 16
#                <query>                      <seeds>  <threads>

./build/find.exe queries/mansion-village.json --explain    # will this ever finish?
./build/find.exe queries/mansion-village.json --bias       # is scanning from 0 representative?
```

Query format:

```json
{
  "version": "1.21",
  "conditions": [
    { "id": "mansion", "structure": "mansion", "within": 300, "of": "spawn"   },
    { "id": "village", "structure": "village", "within": 400, "of": "mansion" },
    { "id": "jungle",  "biome":     "jungle",  "within": 400, "of": "mansion" }
  ]
}
```

`of` refers to another condition's `id` (coordinates are measured from that
condition's matched position) or `"spawn"`. Forward references are fine — the
planner resolves and orders them.

## Why the planner exists

A structure geometry check costs **~9 ns**. A biome check costs **~31 µs**. That
**~3,650× ratio** decides everything: a full 2^48 sweep is ~3–4 days with
structure filters and ~17.8 years with biome filters.

cubiomes does **no** cost-based ordering — it evaluates conditions in whatever
order you hand it. So ordering is *our* job, and natural language produces almost
exactly the wrong order ("a jungle village near a mansion" states the expensive
condition first).

Every structure condition therefore splits into two tasks:

| task | pass | needs | cost |
|---|---|---|---|
| geometry (`getStructurePos`) | 1 | low 48 bits only | ~9 ns |
| viability (`isViableStructurePos`) | 2 | full 64-bit seed + biome gen | ~42 µs |

Pass 1 runs first and rejects most seeds for ~9 ns. Because structure layout
depends only on the low 48 bits, **one pass-1 rejection kills all 65,536 world
seeds sharing that structure seed**.

`find` prints the plan it chose before searching, so the ordering is auditable:

```
plan (reordered by cost, NOT by the order you wrote them)
  pass 1  48-bit geometry -- no biome generator exists yet
    1. mansion within 500 of spawn                ~29 ns
  pass 2  64-bit, biome-dependent -- only for pass-1 survivors
    1. mansion within 500 of spawn                ~42000 ns
    2. jungle within 400 of mansion               ~419175 ns
```

That query was *written* biome-first (`jungle`, then `mansion`, with `jungle`
referencing a condition declared after it). Natural language states the
expensive condition first; the planner inverts it. `ask.py` therefore tells the
model **not** to worry about ordering — that decision belongs to the planner,
which has the measured costs.

## Estimate before searching

`--explain` samples the query and predicts the funnel in a couple of seconds,
rather than discovering after an hour that it will never finish:

```
phase A  2000000 structure seeds, geometry only, 0.02s
  pass 1 survival : 2.32625%   (46525/2000000)
phase B  39936 upper-bit probes across 512 survivors (78 each), 0.14s
  pass 2 survival : 0.05759%   (23/39936)  per upper-bit variant

model (mirrors the real searcher: cap 64 upper probes, stop at first hit)
  structure seeds per hit       : 1188
  predicted search rate         : 181 k seeds/s on 16 threads

time to first hit : 6 ms
VERDICT: very common. Instant results.
```

The estimator is calibrated against real runs — on the query above it predicted
`181 k seeds/s` and `62.9` upper probes per survivor; the real search measured
`190 k seeds/s` and `62.8`. On a rare query it predicted 0.0370% pass-1 survival
against an actual 0.0360%. Timing estimates run ~2x conservative.

Two details worth knowing:

- Samples are drawn **uniformly across 2^48** via splitmix64, not sequentially
  from 0. `--bias` checks whether scanning from 0 (what the searcher actually
  does) is representative — currently 1.2 sigma, i.e. it is.
- Phase A samples geometry only (~9 ns/seed) so it can afford a huge N and still
  measure a tiny pass-1 rate; phase B then spends a fixed probe budget on the
  survivors it kept, so pass-2 gets a usable sample size even when pass-1 is
  minuscule. A flat "N probes per survivor" gives 8 probes when 1 seed survives.

## Read the funnel

The **pass-1 survival rate** is the number that matters:

```
pass1 (48b): 69957  (2.3319% survive)
```

If it approaches 100%, the query is too loose to filter and every seed pays full
biome cost — throughput collapses from ~116M/s to ~32k/s/core. `find` warns when
this happens. Loose proximity constraints are weak filters: "mansion within 2000
blocks" is true of nearly every seed, because mansion regions are only 1280
blocks wide.

## Verification

Nothing here is trusted without an independent check.

```sh
./build.sh tools/xval.c && ./build/xval.exe   # cubiomes
cd tools && javac XVal.java && java XVal      # independent JDK reference
```

`tools/XVal.java` reimplements structure placement from the JDK spec using real
`java.util.Random`, with salts sourced from the Minecraft Wiki (**not** read from
cubiomes — that would be circular). Current status: **2,940/2,940 positions
match**, across linear and triangular spread, negative seeds/regions, and MC 26.2.

`tools/Verify.java` re-checks individual reported seeds:

```sh
FIND_TSV=/tmp/hits.tsv ./build/find.exe queries/mansion-village.json
while IFS=$'\t' read -r s a mx mz b vx vz; do
  java -cp /tmp/vc Verify "$s" "$mx" "$mz" "$vx" "$vz"
done < /tmp/hits.tsv
```

`tools/biomecheck.c` shows what "viable" actually resolved to for a seed.

`./test.sh` runs the whole thing as a regression suite (16 checks): the
cross-validation above, planner reordering *and* cost-based ordering, all four
error paths, a real search whose every seed is re-verified under the JDK
reference, the loose-query warning, estimator calibration, and `describe`
round-tripping a found seed. Each assertion has been confirmed to fail when the
behaviour it checks is broken — a green run means something.

## Known limitations

- **Biome conditions sample on a 64-block lattice.** Small biome patches between
  sample points can be missed. This yields false *negatives* (missed seeds),
  never false positives — reported seeds are always correct.
- **`UPPER_SAMPLES = 64`** — only 64 of the 65,536 upper-bit variants are tried
  per surviving structure seed, so most valid world seeds are never enumerated.
  Fine for finding *a* seed; wrong for exhaustive search.
- **cubiomes omits `nextInt`'s rejection loop** (vanilla has it). Divergence
  ~3.7e-9 per call. Real, negligible.
- **1.18+ false positives**: per cubiomes' README, desert pyramids, jungle
  temples and mansions can fail to generate based on surface height, which
  `isViableStructurePos` does not model.
- Overworld only. No nether/end conditions yet.

## Engine

[`xpple/cubiomes`](https://github.com/xpple/cubiomes) — MIT, actively maintained,
supports through MC 26.2. **Not** `Cubitect/cubiomes` (dormant since 2024-11-10,
caps at 1.21 — it cannot generate current worlds), and **not** `cubiomes-viewer`
(GPL-3.0; linking it would force this project GPL).

`cubiomes/compat/sys/time.h` is a local shim — clang here targets
`x86_64-pc-windows-msvc`, which has no POSIX headers.

> Note: `cubiomes/tests.c` is **not** a test suite. Its `main()` is entirely
> commented out and it exits 0 with no output. Don't mistake it for verification.

## Layout

```
ask.py              English -> Claude -> condition JSON -> find
test.sh             regression suite (16 checks; ./test.sh -v to see commands)
build.sh            build everything
src/query.{h,c}     condition tree, cost model, planner, evaluator
src/explain.{h,c}   selectivity sampling; predicts the funnel before searching
tools/find.c        CLI: parse -> plan -> search | --explain | --bias
tools/describe.c    given a seed, print spawn + nearby structures & their biomes
tools/vocab.c       dumps valid structures/biomes per version (ask.py reads this)
tools/xval.c        cubiomes side of cross-validation
tools/XVal.java     independent JDK reference
tools/Verify.java   re-verify individual seeds
tools/biomecheck.c  inspect what "viable" meant
queries/*.json      example queries
cubiomes/           the engine (git clone)
```
