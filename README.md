# Seed Cracker

A Minecraft Java Edition **seed finder**: describe the world features you want, get seeds and coordinates.

> Seed *finding* (search for a seed that **has** features you describe) is a different problem from seed *cracking* (recover the seed of a world you have data from). This is a finder. Consequently **the Minecraft version is an input, not an output** — a finder cannot infer a version it was told to generate for.

## Build

```sh
./setup.sh                 # clone + pin the engine (first time only)
./build.sh tools/find.c    # library + build/find.exe
./test.sh                  # 105-check regression suite
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

The main screen is a single rounded prompt box: **describe the world you want**
in plain English and press Enter. Asking replaces what came before, the way a
chat does — the previous results clear and the box empties — and a **stop**
button sits beside the live scan, which kills the search process itself rather
than just walking away from it. While it searches, the **seeds it's testing
scroll past live**; matches appear below, and **every coordinate in a result is
clickable**: it opens the **biome map** centred on that exact spot, in that
structure's own dimension. The map covers **overworld, nether and end**, with
structures marked and spawn crosshaired, zoomable from 200 to 20,000 blocks, and
switching to the nether divides the coordinates by 8 the way a portal would.
Binds to loopback only — the search is a native binary, so this cannot be a
hosted page; the server exists to bridge the browser to `build/find.exe`.

The **⚙ dot** by the prompt opens settings and the full **manual builder** —
click conditions together by hand if you'd rather not use AI (it needs no key).

#### The natural-language box needs a Gemini API key

Getting one is free:

1. Go to **[aistudio.google.com/apikey](https://aistudio.google.com/apikey)** and
   sign in with a Google account.
2. Click **Create API key** and copy it (it looks like `AIza…`).
3. In the finder, click the **⚙ dot** → paste the key under **Gemini API key** →
   **Save key**. It's stored only in your browser (localStorage), never committed.

Prefer not to keep it in the browser? Set it in the environment before starting
the server instead — `export GEMINI_API_KEY=AIza…` (or `$env:GEMINI_API_KEY` in
PowerShell) — and the prompt box uses that. The manual builder needs no key at
all. (The older `ask.py` path still uses Anthropic; the web prompt uses Gemini.)

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
  structure cluster, biome, chest, ore count, slime cluster, biome area, terrain
  height, spawn, and portal.

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

## Leaderboard: records, not matches

Every condition above is a **filter**: pass or fail, and the search stops once it
has enough hits. That answers "find me a seed with a tall mountain". It cannot
answer "find me the *tallest* mountain", which is a different question — and the
question the famous record hunts (Minecraft@Home's tallest cactus, biggest ore
vein) actually ask.

Add a `rank` block next to `conditions` and the finder becomes an optimiser:

```json
{
  "version": "1.21",
  "conditions": [ { "id": "pk", "height": 0, "within": 200, "of": "origin" } ],
  "rank": { "of": "pk", "by": "height", "top": 10 }
}
```

```
ranking the top 10 by peak Y over 2000000 structure seeds (the full range is scanned -- no early stop)

--- leaderboard: top 10 by peak Y ---

SEED -6648438949905690143   peak Y = 251
   peak           x=  -184 z=    72   ~251 peak within 200
```

`by` is optional — `auto` takes the ranked condition's own natural measurement,
which is almost always what you want. The explicit metrics are `height`,
`relief`, `vein`, `count`, `pct`, `size`, and `area` (overlap intersection).
Rankable conditions are the ones that *measure* something: terrain height, ore,
slime chunks, biome area, island, end-portal eyes, structure clusters, geode
size, and overlap.

### Budget it by time, not by seeds

A seed count is a poor way to say "keep looking while I make coffee" -- how far
a range gets you depends entirely on how expensive the query is, and the
interesting queries are the slow ones. So a search can be given a **time
budget** instead: the UI's "search for (minutes)" box, or `FIND_SECONDS` on
`find.exe` directly. The scan stops when the clock runs out and reports
whatever it found, and both the funnel and the UI say the budget expired --
because "best of N seeds" has to name the N actually reached, not the one that
was requested. Leave the seed count huge and let the clock decide. Asking for it
in plain English works too ("search for ten minutes", "scan 50 million seeds").

Two things to know, because they change how you write the query:

- **The whole range is scanned.** There is no early stop, because "the best of
  the first twelve hits" is not a record. The range therefore sets the runtime
  directly, and "best" means best *of exactly that many seeds* — the header line
  says so, and you should quote it alongside any result. A query with **no
  structure conditions** (terrain, ore, slime, biome area) has no 48-bit
  geometry to filter on, so the finder walks **world seeds** directly instead of
  structure seeds: one seed scanned is then one world evaluated, spread across
  the whole 64-bit space by a bijection rather than counting up from zero. The
  funnel says which mode ran.
- **Set the ranked condition's threshold LOW.** `{"height": 0}` ranked by height
  beats `{"height": 200}` ranked by height: the threshold still filters, so a
  tight one just starves the leaderboard of candidates.

Ranked results carry their score into the UI and the CSV/JSON export, and the
score is reproducible — the suite re-derives the top entry's value with
`tools/checkheight` and requires an exact match.

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

`./test.sh` runs the whole thing as a regression suite (105 checks, ~3 min): the
cross-validation above, planner reordering *and* cost-based ordering, six error
paths, same-type distinctness, nether/end queries including the end-city
exclusion zone, biome-precision plumbing, a real search whose every seed is
re-verified under the JDK reference, the loose-query warning, estimator
calibration, `describe` round-tripping a found seed, the UI server's endpoints,
and the hand-rolled PNG encoder (signature, chunk CRCs, zlib length). Each
assertion has been
confirmed to fail when the behaviour it checks is broken — a green run means
something.

### The last resort: a real world (tier 3)

Everything above checks our code against *another implementation*. Some claims
have no second implementation to check against — anything Minecraft places
*after* terrain, because features need a `WorldGenLevel` the headless backend
cannot build. For those, `tier3-java/` runs an actual 1.21.1 server and reads
blocks out of the world it generates.

```sh
python tier3-java/check_terrain.py 12345 40        # our heights vs the real world
python tier3-java/check_cactus.py 12345 -2468 832 6
python tier3-java/confirm_cactus.py <seed> <x> <z> <height>
```

`server.jar` is fetched from Mojang's own manifest and checksummed; it is never
committed. Running it writes `eula=true`, which accepts Mojang's licence on that
machine — a deliberate act, not a side effect.

It is built around one trap: `/execute if block` on an unloaded chunk fails
*exactly* like a block that is not there. Merging those is how earlier Bedrock
work reached three separate false conclusions, so every probe is preceded by a
positive control (y=−64 is bedrock in every overworld column) and answers
"unknown" rather than "no".

This is also what keeps the honest numbers honest. Our block-level heights are
exact in about four columns out of five; the cactus simulation, which replays
decoration RNG where one wrong column desynchronises a whole patch, lands at 79%
of predicted columns exact and 98% of chunks exact on block count. So the search
generates candidates and tier 3 confirms a record before it is claimed — the same
split this project uses everywhere else.

## Chest loot

Search for a structure whose chests hold a rare item:

```json
{ "id": "chest",
  "loot": { "structure": "desert_pyramid", "item": "diamond", "count": 1 },
  "within": 3000 }
```

Supported structures: `desert_pyramid`, `jungle_temple`, `igloo`, `outpost`,
`shipwreck`, `ruined_portal`, `fortress`, and `bastion`. The item list per structure is exposed at
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
unaffected in practice.

### The nether pair

Fortress and bastion were previously excluded, one for a crash and one because
its counts did not reproduce. Both are now in, for different reasons.

**Fortress — exact.** `getFortressPieces` documents its `n` argument as "the
maximum size of the output list", stores it in `env->nmax`, and then never reads
it: every piece is written at `env->list + *env->n` with no bound. That is not a
rare overflow — over a 3.6-million-fortress sample the median fortress has ~130
pieces and **84% have more than 64**, so the old array was overrun by most
fortresses, not by an unlucky one (largest seen: 257 pieces, 96 chests).
[`patches/fortress-piece-bound.patch`](patches/fortress-piece-bound.patch) makes
the engine honour the bound — returning `NULL` exactly as the existing collision
path does, so generation unwinds instead of corrupting memory — and `setup.sh`
applies it after pinning the upstream commit. With room to work in, the patch
changes nothing: the same 180 000-fortress sample produces byte-identical piece
counts before and after. The engine simulates the entire corridor/bridge graph,
so **every** fortress chest is found and the counts are exact.

**Bastion — a floor, stated as one.** cubiomes simulates only the starting piece
of each of the four bastion types, i.e. the chests that *always* generate (2 for
units, 1 for hoglin stable, 2 for treasure, 1 for bridge); the randomly
assembled remainder is not modelled (`// TODO: simulate all pieces` upstream). So
a bastion loot count is a **lower bound**: every hit is real and reproduces
exactly, but a bastion whose only copy of the item sits in a later chest is
missed. `lootCountIsLowerBound()` names that asymmetry rather than leaving it
implicit, and the API description tells the planner to say so in its notes.

The earlier "bastion loot did not reproduce" finding was the loot-context
singleton race described above, not the piece list — the search ran threaded and
the verifier did not. With the lock in place, 24 fortress and bastion instances
pulled from a 32-thread search all re-derive exactly, single-threaded.

Every supported structure has its loot re-derived from scratch in the test suite
(`tools/checkloot`, `tools/checkrpchest`) — including a 236-piece fortress, to
keep the buffer honest.

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

### Veins, and ore you can actually see

Two extra measurements turn ore density into a record hunt:

```json
{ "id": "dia", "ore": "diamond", "count": 1, "vein": 14, "within": 64 }
{ "id": "dia", "ore": "diamond", "count": 150, "exposed": true, "within": 48 }
```

`"vein": N` requires at least **N blocks in one face-connected blob** — the vein
you mine in a single sitting, rather than a scattered total across the disc.
Placements that happen to overlap merge into one larger vein, which is where the
big numbers come from. A 64-block disc typically holds ~1,500 diamond blocks in
total but a largest vein of only ~10; 24+ is rare.

`"exposed": true` counts only ore with a **non-solid neighbour in Minecraft's
real block terrain** — ore sitting in a cave or ravine wall, not sealed in stone.
It generates actual terrain columns for every chunk that holds the ore, so it is
the slowest filter in the tool: 1.18+ overworld only, radius capped at 48, and
worth pairing with something cheap. Two honest caveats: the terrain model is
solid/not-solid, so a water- or lava-filled pocket counts as "open" the same as
air; and a neighbour in the *next chunk* is treated as solid, which can miss an
exposed block at a chunk border but never invents one.

`tools/checkore <seed> <material> <version> <x> <z> <radius> [exposed]` reports
both figures independently, and the suite checks that finder and checker agree
and that the exposed count is always a subset of the total.

## Structure overlap

Two structures generating into the same ground — a ruined portal inside a
village, a shipwreck through a monument:

```json
{ "id": "ov", "overlap": ["village", "ruined_portal"], "within": 2000, "pad": 0 }
```

`within` is how far from the reference the **pair** may be; how close the two
structures are to *each other* is the footprint test, not a radius. `pad` adds
slack in blocks for "practically touching". Both halves are biome-viability
checked, so a pair where one structure would not generate is rejected.

> ⚠️ **Footprints are nominal.** cubiomes models where a structure is *placed*
> exactly, but not the full extent it assembles into (`getVariant` sizes cover
> only the starting piece of a jigsaw structure, and nothing at all for several
> others). The boxes used here are per-structure approximations — village 64×64,
> ruined portal 16×16, ancient city 128×128, and so on — so **a hit is a strong
> candidate for a real collision, not a proof**. Sprawling structures (village,
> mineshaft, fortress) reach well past their nominal box. Load the seed and look.

The suite checks that every reported pair really sits inside the summed
footprints and that both halves independently confirm as viable
(`tools/checkcluster` re-counts them from scratch).

## Amethyst geodes

```json
{ "id": "g", "structure": "geode", "size": 4, "cracked": false, "within": 300 }
```

`size` is the geode's distribution-point count — **3 or 4**, with 4 the big one.
`cracked` is the 95% draw that breaks a geode open, so `"cracked": true` barely
filters anything and **`"cracked": false` is the interesting ask**: the 1-in-20
sealed geode that is still intact when you find it. Both come from `getVariant`,
which reads the same draws the game makes, so they are exact.

Geodes are placed from the **chunk population seed**, not the region-based
structure grid. On 1.18+ that seed is derived from all 64 bits, so a geode
cannot be located in pass 1 (which only knows the lower 48) — the finder places
it in pass 2 with the full world seed instead. Two consequences: nothing can be
measured *from* a geode (`"of": "g"` is rejected rather than silently wrong), and
`count` clusters are not supported for it. `tools/checkvariant <seed> geode
<version> --at <x> <z>` re-reads size and crack independently.

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

## Structure clusters

A structure condition takes an optional `"count": N` to demand **several of that
structure packed within the radius** — a triple village near spawn, a knot of
witch huts, three outposts in a stretch:

```json
{ "id": "vils", "structure": "village", "count": 3, "within": 800, "of": "spawn" }
```

Pass 1 counts candidate positions geometrically (an over-admit, since biomes
aren't generated yet); pass 2 re-counts, biome-viability checking each instance,
and keeps the seed only if at least `count` survive. The reported position is the
cluster's centroid; open the map to see the members. This form is **anchored to
the reference** (origin/spawn/parent) — it finds clusters near a known point. A
cluster can't be combined with a variant filter. `tools/checkcluster` re-counts
from the same reference, and the test suite confirms every count reproduces
exactly and clears the threshold.

### Tight clusters (the "quad huts" search)

Add `"spread": T` to demand that the instances be packed **within T blocks of
each other**, located *anywhere* within `within` of the reference — the classic
quad-witch-hut hunt, where four huts must share one despawn sphere:

```json
{ "id": "quad", "structure": "swamp_hut", "count": 4, "spread": 160, "within": 10000, "of": "origin" }
```

Here `within` is the **search reach**, not the cluster size. The finder sweeps a
bounded region window (capped at 64 regions per side) and, anchoring on each
structure instance, counts how many others fall within `spread`; a seed passes
when some member has `count` neighbours (itself included) that close. The
reported position is that **anchor member** — a real structure, not a centroid —
which makes the result exactly re-checkable: `tools/checkcluster` counts viable
instances within `spread` of it and must reproduce the number. Tight clusters are
genuinely rare (four huts within 160 blocks is a many-tens-of-millions-of-seeds
search), so expect a long scan; the cost scales with the search reach, so keep
`within` only as large as you need.

This is *not* an infinite-world scan — it examines a large area around the
reference, which is what you actually want (a quad hut you can reach), not a
cluster half a million blocks away.

#### Solving for the cluster instead of scanning for it

`tools/quad.c` solves the **first** structure's two constraints with
`src/invert.c` and tests the other six — ~140× better than brute force, and
still 4.9e11 seeds *per corner offset*, so a complete enumeration costs more the
looser the spread. `tools/mitm.c` solves all eight at once
(`docs/meet-in-the-middle.md`):

```sh
./build/mitm.exe 1.21 swamp_hut quad 15          # every quad base in 2^48
./build/mitm.exe 1.21 swamp_hut --verify         # sound + complete, two-sided
```

**The planner uses it automatically.** A `spread` cluster condition on a
solvable structure no longer walks seeds 0,1,2,… — `src/solve.c` hands `find` a
list of seeds that already place the cluster, and the search spends its budget
on those. Only the seed *source* changes: pass 1 still evaluates every
candidate, so the solver decides what is **tried**, never what is a **hit**.
`find` prints which path it took and why, and falls back to scanning when a
cluster is loose enough that scanning is genuinely the better tool (measured,
not guessed — it builds under a time budget and judges by the yield).

The failure mode that matters is the list *missing* clusters, so that is checked
by brute force rather than argued: `mitm --covers 240 2000000` finds real
clusters the hard way and demands the solver would have proposed each one — 185
found, 0 missed.

One result the wiring makes cheap: when the solver **completes** and finds
nothing, that is a proof of absence over all 2⁴⁸ seeds, in seconds. The example
two paragraphs above is one — four swamp huts within **160 blocks of a common
member cannot exist**, because the two diagonal members are never closer than
9√2 chunks ≈ 204 blocks. A plain scan reports that as 0.0000% survival, which
reads as "rare" rather than "impossible".

Split the 48-bit seed into halves. The low half alone decides whether **any**
high half can work — `(s1 >> 17) mod 24` is `8·s1H + (s1L >> 17)` (128 ≡ 8 mod
24), and since `gcd(128, 24) = 8` divides 24 only 3 ways, a low half landing on
the wrong residue is dead for every high half at once, having touched none of
them. Eight constraints later
(x *and* z, all four structures) that is `2²⁴/8⁸ ≈ 1` surviving low half when
the offsets are pinned: **the low 24 bits of the seed fall out of the geometry.**
The survivors then meet the high halves, either by sweeping all 2²⁴ (obvious) or
by a bucket lookup keyed on the high half's residue class (70× fewer tests for
swamp huts, 28 000× for ruined portals).

The two halves are strong in *opposite* regimes, and `gcd(128, range)` decides
which. Swamp huts (gcd 8) get a sieve that pins the low half to ~1 value in 2²⁴
and a weak 3-residue bucket key; ruined portals (gcd 1) get **no sieve at all** —
every low half stays live — and a 25-residue key that carries the whole search.
Building only one of the two would have left half the structures unimproved.

Complete enumeration of **every** swamp-hut quad in the world, at spread 15 —
the tightest spread that has any:

| | seeds/pairs examined | wall clock | quad bases |
|---|---|---|---|
| `quad.c` | 2.39e13 seeds | ~49 h (projected from its measured rate) | 1 045 607 |
| `mitm --nested` | 8.34e11 pairs | 318 s | 1 045 607 |
| `mitm` (join) | 1.54e10 pairs | **44 s** | 1 045 607 |

Same 1 045 607 bases from the sweep and the join, measured on the same machine
that ran `quad.exe` for the projection (49 corner offsets × 2⁴⁸/24² seeds at its
observed 1.35e8 seeds/s).

A solver that silently drops seeds reports "nothing found" for seeds that exist,
so `--verify` is two-sided and the completeness half is the one that matters: it
checks acceptance against cubiomes' placement in **both** directions over
millions of seeds, brute-forces windows and demands the *same set* back (not
similar counts), takes low halves the sieve **discarded** and sweeps all 2²⁴ high
halves against each, and requires the nested sweep and the hash join to return
identical sets over the whole 2⁴⁸ space. Every base `quad.c` finds must be
reachable, and the suite checks that against the real binary.

**Which structures.** Every region-based structure in the game except the four
that use a different algorithm entirely:

| solvable | how |
|---|---|
| swamp hut, desert pyramid, jungle pyramid, igloo, village, ocean ruin, shipwreck, ruined portal, trail ruins, trial chambers | `ox = (s1>>17) % r` — the high half is pinned to one residue mod `r/gcd(128,r)` |
| **ancient city** | power-of-two range: Java scales instead of taking a remainder, which for r = 2ᵏ is exactly `ox = s1H >> (24−k)` — a prefix of the high half |
| **fortress, bastion (1.18+), pillager outpost** | same placement, plus a rejection roll that is a concrete function of the seed and the chunk |
| **monument, mansion, end city** | two draws averaged per axis (four per structure); the sieve uses each draw's marginal |
| **not solvable** | mineshaft and buried treasure (per-chunk rolls), stronghold (ring-based), geodes and wells (Xoroshiro population seeds) |

`--verify` is green for **all twelve**: swamp hut, village, ruined portal,
desert pyramid, shipwreck, ancient city, outpost, fortress, bastion, monument,
mansion and end city. The suite runs six of them — between them covering every
distinct code path, including `gcd(128, range)` of 8, 2 and 1, the power-of-two
placement, a rejection roll, and the averaged pair.

Mansion and end city took a while to get there, and the reason is worth knowing
if you touch the join: on a **windowed** run the bucket lookup used to compute
its class sets before checking whether a plain sweep would be cheaper, and for a
query whose sieve keeps every low half that setup cost eleven minutes to avoid
sweeping two values. Sweeping wins whenever the window is smaller than the walk's
setup, and that is now decided first — mansion's verify went from over forty
minutes to 2m29s.

One result worth stating: under `quad.c`'s pairwise rule, **spread 13 and 14 are
empty** — the two diagonal huts are ≥9 chunks apart on both axes, so nothing is
tighter than 9√2 ≈ 12.73, and the offsets that reach it have no seed at all. The
tightest swamp-hut quad that exists is spread 15. Since an empty answer from a
sieve looks exactly like a broken sieve, `mitm quad` corroborates one by solving
for three structures and testing the fourth with cubiomes — a route that never
consults the sieve about it (650 236 seeds place three huts right; none places
the fourth).

## Biome adjacency

A biome condition can be measured from **another biome**, not just from a
structure — which is how you ask for two biomes *next to each other*:

```json
{ "conditions": [
  { "id": "shroom", "biome": "mushroom_fields", "within": 800, "of": "spawn" },
  { "id": "mesa",   "biome": "badlands",        "within": 300, "of": "shroom" }
] }
```

That reads "a mushroom-island near spawn, with a mesa within 300 blocks of it."
To make this work, a biome condition now **records where it matched** and reports
that position; a biome that something else is measured from records the match
nearest its own centre (so a large biome's far corner doesn't anchor the child).
The planner schedules the parent biome before the child in pass 2 — the one
place pass 2 has an ordering constraint, since the parent's position is produced
there rather than in pass 1. A biome's parent must be a structure or a biome
(orbiting an ore/slime/area count has no single meaningful point).

Because the anchor is one representative point, adjacency errs toward **false
negatives** (it can miss a pairing when the recorded point is far from the
neighbour), never false positives — a reported pair genuinely has both biomes
within the requested distance. `tools/checkbiome` reports the biome at a point;
the test suite confirms both reported positions really are their biomes and the
child really is within its radius of the parent.

## Terrain height (approximate)

Find seeds with tall terrain near a point — a mountain over spawn, or a
structure sitting up high (a "tall pillager outpost"):

```json
{ "id": "peak", "height": 150, "within": 300, "of": "origin" }
```

The disc must contain a surface point at least `height` blocks high. For "tall
structure," measure from one with a small radius: `{ "height": 130, "within": 48,
"of": "outpost" }`.

> ⚠️ **This is the one approximate condition.** Everything else in the tool is
> exact and independently verified; terrain height is cubiomes' *estimate*
> (`mapApproxHeight`, the same routine it uses for spawn finding), not exact game
> height. It can be off by a few blocks either way, so it can occasionally report
> a peak that the real world doesn't quite reach — leave headroom on the
> threshold. Overworld only; most accurate on **1.18+**. `find` prints it with a
> leading `~` and the plan/UI label it "approx". `tools/checkheight` re-samples
> the same estimate (confirming the finder reproduces it, not that the estimate
> matches a real world), and the suite checks that.

Adding `"exact": true` switches to cubiomes' **real block-level terrain**
(`generateColumn`), which is a measurement rather than an estimate — at the cost
of a heavy noise column per 4×4 cell, so the radius is capped at 48 and the
planner runs it dead last. Pair it with `"relief": D` (peak minus valley) to find
a structure on a genuine cliff edge rather than merely on high ground. Exact
heights are reported in **world Y**, the same scale as the estimate; the suite
compares the two so the column-index conversion cannot silently drift back.

## Biome area

Find seeds where a biome is genuinely *large* near a point — a huge mushroom
island, a sprawling mesa, a jungle big enough to build in:

```json
{ "id": "sh", "biome_area": "mushroom_fields", "pct": 50, "within": 800, "of": "spawn" }
```

This keeps the seed if the biome fills at least `pct` percent of the disc. It
samples the disc on the same lattice a plain biome check uses and takes the
fraction of sample points that are the target biome, so a big radius plus a high
percentage means a big biome. `precision` (`fast`/`fine`/`exact`, default `fine`)
is the same recall/cost knob as a biome condition — a coarser scan only ever
*misses* qualifying seeds, it never invents one, so a reported percentage is a
floor. Radius caps at 4000 and defaults to 800; `of` may be `origin`, `spawn`,
or another condition. `tools/checkbiomearea` re-samples identically, and the test
suite confirms every reported percentage reproduces exactly (`find` ==
`checkbiomearea`) and clears the threshold.

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

## Questions about the finished world (tier 3)

Some questions are not about *where* something generates but about what is
actually there once every generation stage has run. cubiomes computes placement;
it does not build blocks, so it cannot answer them at all. `tier3-java/` runs a
real 1.21.1 server and reads the blocks out of the world it makes.

```sh
python tier3-java/village_smiths.py <seed> 1500      # blacksmiths per village
python tier3-java/hunt_smiths.py --min 5 --seeds 200 # hunt for a 5-smith village
python tier3-java/treasure_probe.py <seed>           # what surrounds a treasure
```

**"A village with 5 blacksmiths."** The 1.13 blacksmith became three buildings
in 1.14 — armorer, toolsmith, weaponsmith — each identified by the workstation
its template places. A 1.14+ village is assembled by the jigsaw generator, and
no seed-finding library implements that assembler. Rather than reproduce it,
this counts `blast_furnace` / `smithing_table` / `grindstone` in the finished
world: whatever the assembler decided, the blocks are there to count. Every
count carries a positive control — a village has one bell, and a box with no
bell reports nothing rather than zero.

**"A buried treasure somewhere it shouldn't be."** Probes all 26 neighbours of
the chest in one server round trip and identifies each against a candidate list,
reporting anything it cannot name as *unidentified* rather than as ordinary.
Measured over 108 chests: they sit at y 32–79 in sand, sandstone, water and
gravel, with **zero** unidentified neighbours — so a bedrock-encased treasure
does not occur, bedrock being a hundred blocks below anything measured. The
detector fires when something unusual is present (two chests had ore touching
them), so that negative is a measurement rather than a blind spot.

This tier is **slow and one-seed-at-a-time**: ~14 s per world, ~8 s per village.
It is a confirmer and a hunter, not a filter inside the main search — the flow is
cubiomes finds candidates in microseconds, the server checks the survivors.

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

## World types

```json
{ "version": "1.21", "large_biomes": true, "conditions": [ ... ] }
```

`large_biomes` selects the Large Biomes world preset: the same generator with
the biome scale multiplied, so structures, ores and terrain all still work —
they just land in a differently shaped world. It is a property of the world, not
of a condition, so it sits next to `conditions` and every generator the query
builds is told about it. The UI carries it through search, **map and describe**
alike; a Large Biomes result drawn with the default generator is a picture of a
different world. `tools/checkbiome` and `tools/checkore` take a trailing `large`
argument so results stay re-checkable in the world that produced them.

Amplified, superflat and single-biome presets are **not** supported.

## 1.18+ structure placement

From 1.18, three structures refuse to generate on ground that is too low, and
cubiomes' `isViableStructurePos` models the biome rules but not that one. The
finder therefore used to report structures that are not in the world. The rules
are now applied, read out of the shipped 1.21 classes rather than guessed:

| structure | rule |
|---|---|
| desert pyramid | lowest of four corner heights across 21×21 from the chunk min ≥ 63 |
| jungle temple | same across 12×15 ≥ 63 |
| woodland mansion | a 5×5 box at chunk min + 7, signs flipped by the rotation the chunk's structure RNG draws first, ≥ 60 |

Measured effect on what the finder reports: **45% of biome-viable desert
pyramids, 26% of jungle temples and 7% of mansions were phantom** and are now
rejected. Validated against the real MC 1.21.1 generator over 110 positions —
**0 false positives**, 5 false negatives.

Those false negatives are the deliberate direction. Minecraft's
`WORLD_SURFACE_WG` counts water, we measure solid ground, and cubiomes' terrain
differs from the game's by a block or two — so a structure sitting exactly on the
threshold, or with a corner under water, can be rejected when the game would
allow it. `tools/checkplacement <seed> <structure> <version> <x> <z>` re-derives
the corners and prints the verdict.

## A structure over a cave

```json
{ "id": "v", "structure": "village", "cave_below": 25, "within": 800 }
```

1.18 caves are part of the terrain density function, not a carving pass, so a
structure can end up on a thin crust above a cavern. This measures the tallest
run of open blocks under the structure — sampling the anchor and four corners of
its footprint, because a village is 60+ blocks wide and the interesting void is
rarely under the exact anchor.

Real block terrain, so it is one of the most expensive conditions and the
planner sorts it last. `tools/surface … column` re-measures any single column
independently, and the suite checks that every reported cave reproduces.

Worth being precise about the wording: a village is never *inside* a cave —
villages generate on the surface. What this finds is a village **over** one.

## Impossible queries are refused, not searched

Some requests no seed can satisfy, and searching for them does not fail — it
runs forever. The engine proves the ones that come from Minecraft's placement
rules and refuses up front, with the reason:

```
$ find outpost-in-village.json
plan error: impossible: a pillager outpost never generates within 10 chunks
(176 blocks) of a village -- Minecraft's placement data excludes it. Ask for
176 blocks or more
```

That rule is real and comes from the game's own data
(`data/minecraft/worldgen/structure_set/pillager_outposts.json`):

```json
"exclusion_zone": { "chunk_count": 10, "other_set": "minecraft:villages" }
```

`ExclusionZone.isPlacementForbidden` scans the ±10-chunk square for a village
**placement** chunk — placement, not viability, so a village that would fail its
own biome check still blocks the outpost. cubiomes does not model this, so the
finder used to report outposts the game will not build; it is now applied in
pass 1, where it is pure 48-bit math.

Currently proven impossible:

- a pillager outpost within 176 blocks of a village (so "an outpost inside a
  village", or on a building in one, cannot happen at all)
- two of the same structure closer than the placement grid allows — villages
  and swamp huts are never within ~144 blocks of each other, so a tighter
  cluster is impossible

**Rarity is deliberately not judged.** A 12-eye portal is astronomically
unlikely but possible, and `--explain` estimates that honestly. Only genuinely
impossible things are refused, or the tool would be lying about what it cannot
do.

## Known limitations

- **Structure overlap uses nominal footprints.** The per-structure boxes are
  approximations of assembled extent, not generated geometry, so an overlap hit
  is a strong candidate rather than a proof — and a sprawling village can collide
  with something outside its box without being reported. See
  [Structure overlap](#structure-overlap).
- **A leaderboard is only as good as its range.** "Best in 2 million seeds" is
  the honest claim; nothing here searches the whole 2⁶⁴ space, and the ranked
  header prints the range for exactly that reason.
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
src/invert.{h,c}    structure placement run backwards: which seeds put it HERE
src/mitm.{h,c}      meet-in-the-middle: all of a cluster's constraints at once
tools/invert.c      CLI + two-sided soundness/completeness proof for the inverter
tools/quad.c        quad-hut hunt by solving the first structure, testing the rest
tools/mitm.c        quad-hut hunt by solving all eight constraints | --verify
patches/            the one upstream fix this needs (applied by setup.sh)
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
