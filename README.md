# Seed Cracker

A Minecraft Java Edition **seed finder**: describe the world features you want, get seeds and coordinates.

> Seed *finding* (search for a seed that **has** features you describe) is a different problem from seed *cracking* (recover the seed of a world you have data from). This is a finder. Consequently **the Minecraft version is an input, not an output** — a finder cannot infer a version it was told to generate for.

## Build

```sh
./setup.sh                 # clone + pin the engine (first time only)
./build.sh tools/find.c    # library + build/find.exe
./test.sh                  # 45-check regression suite
```

Requires `clang`, `git`, and a JDK (for the verifiers). No `make`/`ninja` needed.

`setup.sh` pins cubiomes at a specific commit (`b12a532`, MC 26.2) rather than
tracking `main`. Engine currency is the whole reason this project uses
`xpple/cubiomes`, so the version it generates against should change on purpose —
bump the pin, then re-run `./test.sh`.

## Run

### Web UI

```sh
python serve.py        # opens http://127.0.0.1:8777
```

Build queries by clicking, see the execution plan, estimate whether a search
will finish, run it, and click any result for a **biome map** of that world —
structures marked, spawn crosshaired, zoomable from 500 to 8,000 blocks.
Binds to loopback only — the search is a native binary, so this cannot be a
hosted page; the server exists to bridge the browser to `build/find.exe`.

The natural-language box needs an Anthropic key; everything else works without
one.

The interface is styled as a Minecraft GUI — beveled stone panels, item-slot
condition cards, and a `☀ / ☾` toggle between a dark **Cave** theme and a
light **Overworld** one. Structures and items show emoji icons out of the box;
drop your own PNGs into [`assets/`](assets/README.md) to replace them (the tool
bundles no Mojang textures). Nothing is required — the built-in look stands
alone.

Three conveniences for real use:

- **Share** (top right) copies a link with the whole query encoded in the URL
  hash. Opening it on any local instance rebuilds the builder exactly — the
  server is loopback-only, so you share the *query*, not a hosted page.
- **Saved searches** keeps named queries in your browser's local storage;
  click one to reload it.
- **Export** on a result set writes the seeds and every coordinate to **CSV**
  or **JSON** (or copies CSV to the clipboard) — one flat row per structure,
  chest, ore count, slime cluster, spawn, and portal.

### Command line

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
    { "id": "mansion", "structure": "mansion", "within": 300, "of": "origin"  },
    { "id": "village", "structure": "village", "within": 400, "of": "mansion" },
    { "id": "jungle",  "biome":     "jungle",  "within": 400, "of": "mansion" }
  ]
}
```

`of` refers to another condition's `id` (coordinates are measured from that
condition's matched position), or one of two **different** reference points:

| `of` | means | cost |
|---|---|---|
| `"origin"` (default) | the point (0, 0) | cheap — pass 1 filters on it directly |
| `"spawn"` | the actual world spawn | slower — see below |

**These are not the same place.** Measured over 400 seeds, the median world
spawn is 22 blocks from origin, but p90 is 520 and the maximum nearly 1000 —
**47% of seeds spawn further than 35 blocks from origin**. A query for
"ruined portal within 35 of spawn" that quietly measured from origin would
return seeds where you land 300 blocks from the portal. Both references are now
supported explicitly and results state which one each distance is measured from.

Spawn is a 64-bit, biome-derived quantity, so pass 1 cannot filter on it: a
spawn-relative condition widens the pass-1 radius by a 1100-block margin (a
conservative over-admit) and pass 2 then recomputes the match around the true
spawn exactly. That turns a 2%-survival query into ~96%, so `origin` is the
default and `spawn` is an informed choice. The End and Nether have no world
spawn — using `"spawn"` there is rejected rather than silently misinterpreted.

Forward references are fine — the planner resolves and orders them.

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
cubiomes — that would be circular). Current status: **3,881/3,881 positions
match**, across all three dimensions, linear and triangular spread, negative
seeds/regions, and MC 26.2. It also replicates the per-site gates, not just
positions: bastion's `chunkGenerateRnd` → `nextInt(5) >= 2`, and end city's
1008-block origin exclusion.

`tools/Verify.java` re-checks individual reported seeds:

```sh
FIND_TSV=/tmp/hits.tsv ./build/find.exe queries/mansion-village.json
while IFS=$'\t' read -r s a mx mz b vx vz; do
  java -cp /tmp/vc Verify "$s" "$mx" "$mz" "$vx" "$vz"
done < /tmp/hits.tsv
```

`tools/biomecheck.c` shows what "viable" actually resolved to for a seed.

`./test.sh` runs the whole thing as a regression suite (45 checks, ~85s): the
cross-validation above, planner reordering *and* cost-based ordering, six error
paths, same-type distinctness, nether/end queries including the end-city
exclusion zone, biome-precision plumbing, a real search whose every seed is
re-verified under the JDK reference, the loose-query warning, estimator
calibration, `describe` round-tripping a found seed, the UI server's endpoints,
and the hand-rolled PNG encoder (signature, chunk CRCs, zlib length). Each
assertion has been
confirmed to fail when the behaviour it checks is broken — a green run means
something.

## Chest loot

Search for a structure whose chests hold a rare item:

```json
{ "id": "chest",
  "loot": { "structure": "desert_pyramid", "item": "diamond", "count": 1 },
  "within": 3000 }
```

Supported structures: `desert_pyramid`, `jungle_temple`, `igloo`, `outpost`,
`shipwreck`, and `ruined_portal`. The item list per structure is exposed at
`/api/lootitems` and in the UI dropdown, so you can only ask for something that
can actually appear there. `count` aggregates across every chest in an instance,
and the search scans **all** instances within the radius — a farther pyramid
with the diamond still counts.

Rolling a chest is ~5 us, cheap next to the ~42 us structure viability check,
so a loot search costs about the same as a plain structure search. What makes a
specific item rare is the loot table, not the tool: a diamond in a desert
pyramid is roughly 1 in a few hundred pyramids, which is why the default radius
is small (a stray large radius rolls loot for thousands of instances per seed).

### Ruined portals — computed, not enumerated

The first five come straight from `getStructurePieces`. Ruined portals have **no**
`getStructurePieces` chest case, so their single chest is computed directly
([`src/loot.c`](src/loot.c) `rpChest*`), and every step is pinned to an
independent reference so it earns the same trust:

- **Chest offset** per template (portal_1…10, giant_portal_1…3) from
  KaptainWutax/FeatureUtils; its template **sizes** match cubiomes' own table
  12/13 exactly (portal_5 differs only in Y, which doesn't affect the chest's
  x/z), confirming the variant indexing lines up.
- **World position** via Minecraft's template transform — verified **identical
  to MCUtils' `BPos.transform` for all 104** variant/rotation/mirror cases.
- **Loot seed** = the decoration seed at the chest's chunk, salt **40005** (equal
  in cubiomes and FeatureUtils), with **no** pre-consumption — ruined portal's
  `getSpecificCalls()` is null (unlike igloo, which discards a `nextLong`, or
  desert pyramid, which draws a `nextInt(3)`; both cross-checked here).

`tools/checkrpchest` re-derives the chest and loot independently, and the suite
confirms every search result reproduces at the matched chest. (The buried/
fail-to-generate caveat above still applies to the *portal* — but the chest
contents, if it generates, are exact. Add `"surface": true` to a structure
condition to skip the buried ones.)

### Loot generation is single-threaded

cubiomes' loot contexts are **singletons** — `init_*()` returns `&staticContext`,
so every worker thread shares one context per table and `generate_loot` mutates
its RNG state and output buffer in place. Concurrent rolls therefore race, which
silently made loot counts depend on thread timing. `src/loot.c` serialises the
roll+read with a small spinlock; the ~5 µs roll is a tiny fraction of the
per-seed cost and the biome/structure work stays parallel, so throughput is
unaffected in practice. Two structures were investigated and left out for hard
reasons: **fortress** (`getFortressPieces` never enforces its buffer bound, so a
large fortress overflows and crashes) and **bastion** (the engine simulates only
some pieces and its reported loot did not reproduce under verification).

Every supported structure has its loot re-derived from scratch in the test suite
(`tools/checkloot`, `tools/checkrpchest`) — fewer structures, but each one you
can trust.

## Ore density

Find seeds rich in a material near a point:

```json
{ "id": "dia", "ore": "diamond", "count": 1400, "within": 64, "of": "origin" }
```

This counts **every ore block of that material at all depths** within the
radius, deduplicated across the generation features that place it — a richness
proxy, not the handful you would actually mine. cubiomes reproduces Minecraft's
ore placement exactly (`getOreConfig` → `generateOres`), and matching on the
*placed block* rather than a hardcoded feature list means "diamond" captures the
regular, buried, large, and medium diamond features in whatever version you
pick. Materials: `diamond`, `iron`, `gold`, `emerald`, `redstone`, `lapis`,
`copper`, `coal` (overworld); `quartz`, `ancient_debris`, `nether_gold`
(nether). `of` may be `origin`, `spawn`, or another condition's structure.

It is the **most expensive** condition — a per-chunk cost over the whole search
disc — so the planner always runs it last, only for seeds that already cleared
the cheap filters. Radius is capped at 256 and defaults to 64; as the *only*
condition it will crawl, so pair it with a structure. `tools/checkore` re-counts
independently, and the test suite confirms every reported count reproduces
exactly (`find` == `checkore`) and clears the requested threshold.

## Slime chunks

Find a dense cluster of slime chunks near a point — the site for a slime farm:

```json
{ "id": "cl", "slime": 8, "within": 128, "of": "origin" }
```

This counts the slime chunks whose centre falls inside the disc and keeps the
seed if that count is `>= slime`. A slime chunk is a **per-chunk RNG check on the
world seed** (`isSlimeChunk`: mix the chunk coords into the seed → `setSeed` →
`nextInt(10) == 0`), so the mechanic is **version-independent** — the condition
carries across every MC version unchanged, and switching versions in the UI never
drops it. `of` may be `origin`, `spawn`, or another condition's structure.

A radius-128 disc holds ~200 chunks and averages ~20 slime, so ask for a tight
cluster to find a farm-worthy spot. Radius is capped at 2048 and defaults to 128.
It is a cheap pass-2 check (no biome generator needed), but as the *only*
condition it still scans every seed, so pairing it with a structure narrows the
field first. `tools/checkslime` re-derives the count from scratch, and the test
suite confirms every reported count reproduces exactly (`find` == `checkslime`,
origin- and spawn-relative) and clears the requested threshold.

## Versions

The engine (xpple/cubiomes) generates up to **MC 26.2**, and the UI defaults to
it — pick any version from 26.2 back to 1.12. A finder generates for exactly the
version you choose; it cannot infer one, and structure/biome availability and
ore placement all shift between versions, so the choice is load-bearing.

Every reported count is re-derived independently by `tools/checkloot.c` in the
suite, so the numbers aren't a plumbing artefact.

## Village buildings — real Minecraft worldgen (tier 2)

cubiomes stops at *where* a structure is; it has no block-level terrain, so it
cannot see *inside* a 1.14+ jigsaw village. Neither can any seedfinding library —
FeatureUtils and its `mc_feature` fork both ship an **unfinished** village
generator (the jigsaw assembler was never completed). The only correct way to
count a village's buildings is to run Minecraft's **actual** world generator.

So there's a second, heavier engine in [`tier2/`](tier2/README.md): a headless
Minecraft (via Fabric Loom, which downloads a deobfuscated jar — nothing Mojang
is committed) that boots the registries, builds the overworld generator for a
seed, runs the real jigsaw assembler, and counts buildings by their template names.
The UI's **Village buildings** panel drives it: pick a **building type**
(any smith, or a specific one — toolsmith/weaponsmith/armorer/library/
cartographer/mason/fletcher/butcher/shepherd/fisher/tannery/temple/farm/…), a
count, and how many seeds to scan; it returns seeds whose village holds that
many.

```
seed 21  x=1128 z=-488  taiga  smiths=6  {taiga_armorer=2, taiga_weaponsmith=4}
seed 29  x=-440 z=-856  snowy  smiths=5  {snowy_armorer_house=2, snowy_tool_smith=2, snowy_weapon_smith=1}
```

Architecture: a persistent Java worker bootstraps Minecraft **once** (~5 s) then
streams seeds over stdin; [`tier2/village_search.py`](tier2/village_search.py)
fans out across several workers and `serve.py` exposes `/api/villagesmiths`.
Caveats: it is **slow** (real jigsaw generation, seconds per seed — a tier-2
search, not a brute force), targets **MC 1.16.5** (1.16.1 predates data-driven
worldgen; village *composition* is identical across 1.16.x but *positions*
differ), and requires the backend to be built (`tier2/README.md`). Village
*composition* here is the game's own output — as authoritative as it gets.

## End portal eyes

An end portal has 12 frames, each independently 10% likely to already hold an
eye. `{"id": "portal", "eyes": 6}` searches for a stronghold whose portal has at
least that many. Results report the stronghold position and the count.

Locating a stronghold costs ~10 ms (biome checks), so the search runs at
**~98 seeds/s**, which sets hard expectations:

| eyes | probability | seeds needed | time on one machine |
|---|---|---|---|
| 6 | 4.9e-04 | 2,036 | ~20 s |
| 7 | 4.7e-05 | 21,382 | ~4 min |
| 8 | 3.2e-06 | 307,910 | ~50 min |
| 9 | 1.6e-07 | 6,235,191 | ~18 hours |
| 10 | 5.3e-09 | 187,055,742 | ~22 days |
| 11 | 1.1e-10 | 9,259,259,259 | ~3 years |
| **12** | **1.0e-12** | **~1e12** | **~320 years** |

So 6–8 is a normal search and 9–10 is a commitment. **A 12-eyed portal is a
distributed-compute target** — the kind of thing Minecraft@home exists for — not
something one machine finds. The UI shows the estimate as you change the number.

Every reported count is re-derived independently by `tools/checkeyes.c` in the
test suite, so the numbers are not an artefact of the search plumbing.

## Ruined portals: what can and cannot be known

Ruined portals are the one structure the engine cannot fully model, and it is
worth being precise about why.

The *position* is exact — cross-validated against the JDK reference like every
other structure. What is not modelled is whether the game actually builds one
there. From cubiomes' own source:

> Ruined portals ... have no terrain restrictions, so a ruined portal *should*
> always generate in each region. However, in locations with underground
> biomes, a ruined portal can fail to generate ... because the biome check is
> done after selecting the portal type and generation height. **Testing for this
> case requires the surface height and is therefore not supported.**

Surface height means block-level world generation, which cubiomes does not do.
So a perfectly reliable ruined portal search is not achievable on this engine.

What *is* now available: `describe` reports each portal's variant, because
**about half of ruined portals in plains- and mountain-category biomes generate
underground**. A portal marked `BURIED` is there — you just cannot see it from
the surface:

```
ruined_portal   x=   144 z=     0    144 away   savanna    BURIED, air pocket
ruined_portal   x=   304 z=   288    192 away   forest     surface
```

### Filtering out buried portals

You can require a **surface** portal and drop the buried ones as candidates.
Add `"surface": true` to a ruined-portal condition (or tick *surface only* in the
UI):

```json
{"id": "rp", "structure": "ruined_portal", "within": 800, "of": "origin", "surface": true}
```

This is an **exact** filter, not an approximation. Minecraft decides burial with
a single coin flip — `nextFloat() < 0.5` on the chunk RNG, only for
plains/mountain-category portals — and `getVariant` reads that exact draw. The
independent verifier `checkportal` re-derives the same bit from scratch and the
test suite confirms every surface-filtered result really is a surface portal:

```sh
./build/checkportal.exe <seed> 1.21 --at <x> <z>
#  -> ... underground=0 airpocket=0 giant=0 (reref=0 AGREE)
```

It does **not** yet catch the rarer "sunk into low terrain" case — that needs
the approximate surface height (`mapApproxHeight`), a planned follow-up. For
results you can reliably walk to today, `surface: true` removes the ~50% that
are buried by design; for maximum certainty prefer a biome-checked structure
(see the table below).

## Structure variants

Some structures generate in distinct variants, and the engine can identify which
one a given instance is — exactly, because `getVariant` reads the same RNG draw
the game does. Require a variant by adding a flag to its condition:

```json
{ "id": "zv", "structure": "village",       "abandoned": true }   // zombie village
{ "id": "ig", "structure": "igloo",         "basement": true  }   // has the lab/basement
{ "id": "gp", "structure": "ruined_portal", "giant": true     }   // giant portal
{ "id": "rp", "structure": "ruined_portal", "surface": true   }   // not buried (above)
```

Each flag is gated to the structure that has it (asking for a `basement` village
is an error, not a silent no-op), and each is checked in pass 2 on the exact
matched instance. `tools/checkvariant` re-reads the flags independently, and the
test suite confirms every zombie-village result really is abandoned. (Geodes
aren't in this engine's structure list, so its `cracked`/`size` variant fields
aren't exposed.)

## Not every structure is verified

A structure position comes from two things: a generation **attempt** (exact
LCG math, cross-validated against the JDK reference) and a **placement check**
(does the game actually build one there). The second is where the engine's
coverage varies. Measured pass rate of `isViableStructurePos`
(`tools/confidence.c`, MC 1.21):

| structure | attempts passing | meaning |
|---|---|---|
| swamp hut, desert pyramid, mansion | 0.9–2.6% | biome-checked |
| ancient city, igloo, monument | 3–9% | biome-checked |
| trail ruins, outpost, village | 12–23% | biome-checked |
| ocean ruin, shipwreck | 28–32% | biome-checked |
| trial chambers | 95.5% | weak (generates almost everywhere) |
| **ruined portal** | **100.0%** | **no check at all** |

For ruined portals, cubiomes' `isViableFeatureBiome` is literally
`return mc >= MC_1_16_1;` — every attempt is reported and false positives
cannot be filtered. A reported ruined portal may not exist in your world.
`find` and the UI now print a warning when a query uses one.

This is a limitation of the engine's world model, not of the search: the
coordinates are right, but whether the game populates that spot is unmodelled.
Prefer biome-checked structures when you intend to visit the result.

## Known limitations

- **Biome conditions sample a lattice, so recall is below 100%.** Measured
  against an exhaustive quart-resolution scan (`tools/biomerecall.c`): at radius
  400, `fast` (64-block) recovers 90.4% of matching seeds and `fine` (16-block,
  the default) 96.2%; both fall at smaller radii. `exact` is 100% but costs
  200×. Set `"precision": "fast"|"fine"|"exact"` per biome condition. Run
  `biomerecall` for the radius and biome you care about rather than trusting a
  single number.
- **Same-type matching is greedy, nearest-first.** Two conditions of the same
  structure type are guaranteed to match distinct instances, but the assignment
  is greedy: if an earlier condition claims the instance a later one needed, the
  seed is rejected even though some other assignment would have worked. Another
  false negative, never a false positive.
- **`UPPER_SAMPLES = 64`** — only 64 of the 65,536 upper-bit variants are tried
  per surviving structure seed, so most valid world seeds are never enumerated.
  Fine for finding *a* seed; wrong for exhaustive search.
- **cubiomes omits `nextInt`'s rejection loop** (vanilla has it). Divergence
  ~3.7e-9 per call. Real, negligible.
- **1.18+ false positives**: per cubiomes' README, desert pyramids, jungle
  temples and mansions can fail to generate based on surface height, which
  `isViableStructurePos` does not model.
- **No GPU acceleration, deliberately.** Pass 1 is pure integer math and would
  port to CUDA cleanly — but `--explain` reports the pass-1/pass-2 time split,
  and pass 1 is **0.0–0.2%** of runtime on every query that actually returns
  results. Pass 2 (biome generation) is the real cost and does not port. Amdahl
  caps the whole exercise at a fraction of a percent.

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
serve.py            local web UI server (loopback only) + /assets static route
ui/index.html       the UI (Minecraft GUI theme, emoji/PNG icon system)
assets/             optional user-provided structure/item PNGs (see its README)
ask.py              English -> Claude -> condition JSON -> find
test.sh             regression suite (16 checks; ./test.sh -v to see commands)
build.sh            build everything
src/query.{h,c}     condition tree, cost model, planner, evaluator
src/explain.{h,c}   selectivity sampling; predicts the funnel before searching
tools/find.c        CLI: parse -> plan -> search | --explain | --bias
tools/describe.c    given a seed, print spawn + nearby structures & their biomes
tools/map.c         render a seed's biome map as a PNG (no image library)
tools/checkpng.py   validates that PNG is spec-correct, not just non-empty
tools/checkspawn.py re-verifies spawn-relative hits against describe.exe
tools/confidence.c  measures how much the engine actually verifies each structure
tools/checkeyes.c   recomputes an end portal's eye count independently
tools/checkloot.c   recomputes a structure's chest loot independently
tools/checkportal.c recomputes a ruined portal's buried/surface variant independently
tools/checkore.c    recounts a material's ore blocks around a point independently
tools/checkvariant.c re-reads a structure's variant flags (zombie/basement/giant)
tools/checkrpchest.c re-derives a ruined portal's chest position + loot independently
src/ore.{h,c}       ore-density counting (generateOres), material-by-block matching
tools/lootitems.c   dumps the items each structure's loot tables can produce
src/loot.{h,c}      per-thread loot-table cache + item counting
tools/vocab.c       dumps valid structures/biomes per version (ask.py reads this)
tools/xval.c        cubiomes side of cross-validation
tools/XVal.java     independent JDK reference
tools/Verify.java   re-verify individual seeds
tools/biomecheck.c  inspect what "viable" meant
tools/biomerecall.c measures biome-scan recall vs an exhaustive scan
queries/*.json      example queries
cubiomes/           the engine (git clone)
```
