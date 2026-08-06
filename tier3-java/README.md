# tier3-java — a real Java world as a block oracle

Every correctness claim elsewhere in this project is checked against something
independent: a JDK reference for the RNG, cubiomes for biomes and structures,
the headless 1.21.1 backend for jigsaw assembly, Bedrock Dedicated Server for
Bedrock. Java **features** — anything decoration places after terrain — had no
such source, because running them needs a `WorldGenLevel` that the headless
backend cannot build.

So this runs the actual server and reads blocks out of the world it generates.

## Setup

`server.jar` is fetched from Mojang's official metadata (never committed —
it is 51 MB and not ours to redistribute), and its SHA-1 is checked against the
manifest:

```
python -c "
import hashlib,json,urllib.request,os
m=json.load(urllib.request.urlopen('https://launchermeta.mojang.com/mc/game/version_manifest_v2.json'))
v=next(x for x in m['versions'] if x['id']=='1.21.1')
s=json.load(urllib.request.urlopen(v['url']))['downloads']['server']
urllib.request.urlretrieve(s['url'],'tier3-java/server.jar')
h=hashlib.sha1(open('tier3-java/server.jar','rb').read()).hexdigest()
print('sha1',h,'MATCH' if h==s['sha1'] else 'MISMATCH')"
```

Running it writes `eula=true`, which accepts Mojang's EULA on this machine.
That was done deliberately and with permission, not silently.

## Use

```python
from java_oracle import JavaServer
with JavaServer(seed=12345) as srv:
    srv.forceload(x, z)
    print(srv.cactus_height(x, z))
```

One server, ~8 s to boot, ~40 ms per probe batch. Console commands all run in
the same tick, so `run([...])` with a thousand commands costs about what one
costs — batch aggressively.

`run-<seed>/` worlds are gitignored and can be deleted freely.

## The trap this is built around

`/execute if block` on a chunk that is not loaded fails **exactly** like a block
that is not there. Merging those two is how the Bedrock work reported "no
structure here" three separate times for chunks it had simply never loaded.

So every probe is preceded by a positive control: y=−64 is bedrock in every
overworld column, so if that fails, the chunk is not loaded and the answer is
`None` — unknown — never `False`.

## check_terrain.py — what it found

Diffs `src/terrain.c` against the real world. Getting the *comparison* right
took three attempts, and each wrong version accused correct code:

| Framing | Verdict | Reality |
|---|---|---|
| "highest non-air block must equal our surface" | terrain broken at (1190,1290) | an **iceberg**: packed ice on y=66 over a seabed at 46, and icebergs are a feature |
| ice counted as terrain | broken at (1676,916) and (947,-523) | a frozen **ice sheet** at 62 over gravel at 61 |
| snow_block *not* counted as terrain | broken at (461,-149) | our y=165 **is** a snow_block; the `snow` layer at 166 is the decoration |

The check that survives is narrower and tests our claim directly rather than
inferring it: the block at our reported height must be a terrain material, and
the block one above must not be. What decoration stacked on top is then
irrelevant, which is the whole point.

**The real limitation it surfaced.** Our block-level heights are exact in about
four columns out of five, not five out of five. Measured over 30 scattered
columns in seed 12345: 24 exact, errors of +/-1 in most of the rest and
occasionally more.

An earlier version of this note blamed 1.18+ aquifers and said submerged columns
were badly wrong, on the strength of a single sample at (1595,1645) where we
report solid ground at y=57 and the true seabed is y=33. Measuring properly
killed that theory: under water 8 of 10 columns are exact, on dry land 16 of 20.
The error is not about water, and that column is an outlier rather than a class.

Two things it is NOT. It is not the corner-cache in terrain.c, though that did
contain a real aliasing bug -- four cells could hash to one slot, and the second
lookup would overwrite data the first pointer still referenced, handing
generateColumn a duplicated corner. That is fixed (the slot is now the cell's low
bits, so the four corners cannot collide), and it changed none of the measured
columns. And it is not the corner ordering, which matches cubiomes' own caller.

What is left is cubiomes' density reimplementation differing from the game near
the zero crossing, plus genuine overhangs where "topmost solid block" is a
different question from "the surface". Closing that means rewriting the density
function, not patching a caller. Until then anything block-exact -- height and
relief records, `cave_below`, cactus columns -- should be confirmed against this
oracle before it is claimed, which is what confirm_cactus.py is for.

## Two things cubiomes cannot answer, and now something can

Both of these are the same shape: a question about the *finished world* rather
than about a placement formula. cubiomes computes where things go; it does not
build the blocks, so it cannot be asked what is actually there.

### village_smiths.py — how many blacksmiths a village really has

The 1.13 "blacksmith" split into three buildings in 1.14, each identified by the
workstation its template places: `blast_furnace` (armorer), `smithing_table`
(toolsmith), `grindstone` (weaponsmith). A 1.14+ village is assembled by the
jigsaw generator at generation time, and no seed-finding library implements that
assembler — FeatureUtils and mc_feature both ship it unfinished, and `tier2/`
runs the real one only for 1.16.5 and was never wired to anything.

So this does not reproduce the assembler. It counts the workstations in the
finished world, which is both simpler and version-current: whatever the assembler
decided, the blocks are there.

```sh
python village_smiths.py <seed> [radius] [--min N]
python hunt_smiths.py --min 5 --seeds 200 --radius 1500
```

**Every count carries a positive control.** A village has exactly one bell, so a
box reporting zero bells reports nothing at all rather than zero smiths — the
same discipline as the bedrock control above, for the same reason. `bells > 1`
means the box caught more than one village meeting point, so that line is a
CLUSTER, not one village; it is printed rather than hidden, because "five smiths"
means different things for the two.

Counting is `/fill ... replace`, which is **destructive** — probing a
village-sized volume one position at a time is hundreds of thousands of commands
against a few dozen fills. Each seed therefore gets a fresh throwaway world and
is counted exactly once.

### treasure_probe.py — what is actually around a buried treasure

```sh
python treasure_probe.py <seed> [radius]
python hunt_treasure.py --seeds 40 --radius 3000 --json out.jsonl
```

Identification is by **candidate list**: `/execute if block` can only ask "is it
X?", and `/data get block` only speaks for block entities, so a neighbour
matching nothing on the list is reported as *unidentified* — never as air, and
never folded into "ordinary". That distinction is the point, since an unknown
neighbour might be the interesting one.

**What the data says.** Over 108 chests across 6 seeds (1.21.1):

| | |
|---|---|
| chest Y | 32–79, concentrated in 48–63 |
| neighbours | sand 1314, sandstone 706, water 241, gravel 208, stone 167, … |
| unidentified | **0** — the candidate list covers everything actually present |
| unusual | 2 chests with an ore touching them (copper, coal) |

So on this evidence a buried treasure **cannot** be encased in bedrock: it
generates at the ocean floor or beach surface, and bedrock is at y −64…−59,
a hundred blocks below anything measured. No spawner was adjacent to any chest
either. The detector does fire when something unusual is there — the two ore
cases prove it is not simply blind — so the negative is a measurement, not a
silence. It is 108 chests, not a proof over all seeds; a wider hunt would
strengthen or overturn it, and `--json` exists so one can accumulate.

### treasure_casing.py — the chest writes its own walls

The probe above answers "what is next to the chest", which turned out to be the
wrong question. Buried treasure does not simply sit in terrain: it takes the
block at its landing spot and writes that block into whichever of its six **face**
neighbours were air or water. The chest arrives already walled in.

```sh
python treasure_casing.py <seed> [radius]
```

The test that separates "encased by the structure" from "happens to be next to
some": the overwrite hits the six faces and not the twenty diagonals, so faces
should be markedly more uniform. Over 16 chests they are — **75% against 59%** —
and individual chests show the rule outright:

```
chest (1737,44,153)
    faces  [andesite, andesite, andesite, andesite, gravel, gravel]
```

Four faces written with the block it landed on; two left as gravel because those
were already solid, so there was nothing to overwrite. **4/6 is a complete
casing, not a partial one**, which is why a hunt must not score it as half a
result. Judging by the fraction of *originally air-or-water* faces that came out
ore would be truer still, but that state is gone by the time anything can look.

### hunt_treasure_ore.py — can the casing be ore?

```sh
python hunt_treasure_ore.py --target iron_ore --min 2 --seeds 120 --radius 6000
python hunt_treasure_ore.py --target any --min 1 ...     # rank by total ore faces
```

Scores the six faces, not all 26 neighbours — the diagonals are terrain the
structure never touched, and counting them buries the signal. Screening is one
pass against the 20 ores plus the 16 blocks a casing is actually made of; a face
matching none of those falls through to the full 85-block list rather than being
recorded as unknown, since an unidentified face is exactly the one that might
matter. That is 186 commands per chest against 630, and roughly 4× the chests
per hour.

**What the casing can be.** Over 87 chests: sand 64, gravel 13, sandstone 6,
stone 2, andesite 1, diorite 1. Two groups, and the split is the generation rule
showing through — the sediment the chest sat *in*, and the rock it sat *on*. The
scan descends from the ocean floor until the block *beneath* is stone-like, so
ore, not being a stopping block, is fallen *through* and landed *on*. The
reachable configuration is therefore narrow but real: an ore blob whose top block
sits directly under the beach sediment, with stone below it.

**Deepslate iron ore cannot case a buried treasure.** Deepslate replaces stone
only below y≈8, and every chest measured sits at y 32–79 — the scan stops at the
first stone-like block under the ocean floor and never reaches deepslate depth.
Plain `iron_ore` is the reachable target; the deepslate variants are on the
candidate list because leaving them off would have mislabelled a find, not
because a treasure can reach them.

### hunt_iron_casing.py — spend the server only where it can pay

Measured, rather than assumed (`bench_probe.py`): the server costs ~6.5 s per
chest and **commands are free** — the probe is 0.07 s of that, so earlier work
shrinking command counts was worth almost nothing. It is chunk generation, plus
boot. `spawn-chunk-radius=0` and `save-off` cut boot from 100 s to 44 s; forcing
a single chunk did *not* help, because the server generates the neighbourhood
anyway to finish a chunk (the 5.3 s stayed put and reappeared in the column scan).

The real lever is the tier-2 headless path, which replicates
`BuriedTreasurePiece`'s placement scan in-process:

| | server | headless |
|---|---|---|
| per chest | ~6.5 s | **0.092 s** (~70×) |
| boot | 44 s *per seed* | 4.5 s **once** |
| agreement | — | 18/19 on seed 4 |

It cannot answer the question directly: it runs terrain and surface rules but not
`applyBiomeDecoration`, and ore is placed in decoration. But it says how *deep* a
chest lands, and depth is the whole game for iron — the middle band spans
y −24…56 peaking at 16, so above y 56 only the small uniform band contributes.
The measured chests sit at y 32–79 concentrated in **48–63**, i.e. almost all of
them land where iron is scarcest. That is why 1300 chests produced coal (which
peaks near y 96) and never iron.

```sh
python hunt_iron_casing.py --max-y 46 --seeds 200 --radius 4000
```

Scan cheap, probe deep. Measured over 20 seeds: 603 treasures scanned in 0.9 min,
5.5% deep enough to probe, **6.7 min against 65 min** for probing all of them —
about 10× end to end. The deep chests also case differently: gravel 25, sand 5,
stone 2, granite 1, against sand-dominated at ordinary depths.

### OreGen — decoration without a server

The gap above was that the headless path stopped before `applyBiomeDecoration`,
which is where ore is placed. `tier2-outpost/OreGen.java` closes it. Decoration
needs a `WorldGenLevel`, and vanilla only ever builds one on a `ServerLevel`;
since it is an interface, OreGen supplies one by **dynamic proxy** — the handful
of methods a feature really calls are implemented against a single `ProtoChunk`,
and every default method is delegated back to the interface via `invokeDefault`,
which keeps the class to what matters rather than the ~80 methods the type has.

Three things that had to be right, each found by it being wrong first:

- `shouldGenerateStructures()` **must** be true. `applyBiomeDecoration` advances
  one shared counter across a step — first over every structure *registered* for
  it, then over its features — calling `setFeatureSeed(seed, k, step)` each time.
  Returning false skips the structure loop and shifts every ore's seed, placing
  ore where the game never would, silently.
- Neighbour chunks return an **empty** chunk, never this one. `ProtoChunk` masks
  x/z by 15, so handing back our own chunk answers with a block wrapped round
  from inside it — wrong, and quiet about it.
- `isStateAtPosition` decides *where* a feature may place. Left to the default
  `false` it would not crash; it would stop ore being placed at all, and the path
  would look fast and sane while reporting no ore anywhere.

Unsupplied methods are **recorded**, not defaulted silently (`UNIMPLEMENTED`),
which is what found `getLevelData`, `getFluidTicks`, `isStateAtPosition` and
`nextSubTickCount` in one run each.

**Verified against the server on 49 chests** (`verify_oregen.py`):

| | agreement |
|---|---|
| `chestY` | **47/49 (96%)** |
| `fill` (the casing block) | **38/49 (78%)** |
| speed | 0.125 s vs 6.5 s — **52×** |

The fill misses have a shape: headless says `sand` where the server says
`sandstone`, `dirt` where it says `sand` — the sediment column running one block
deeper, so the chest lands on sediment instead of on water over rock. None of the
disagreements involved ore, but with almost no ore in 49 chests that is not
evidence either way.

**So OreGen shortlists; the server decides.** A chest whose fill it calls ore is
sent to the server for confirmation. Depth is kept as a second route onto the
shortlist precisely because fill is only 78% — trusting it alone would drop real
candidates it misread as sand. Measured end to end: 399 treasures scanned in
0.8 min, 5 worth probing, **2.0 min against 43 min**.

Light is faked (full sky, no block light). Ore does not consult it, so ore
answers are unaffected — but light-gated vegetation may place differently here,
which matters for anything reading the surface.

**Neither cut is a sieve.** The fast path disagreed on 1 of 19
chests because decoration can add surface blocks that move the landing spot, so a
chest whose true depth is below the cut but whose predicted depth is above it is
dropped and never probed. Fine for a hunt, which wants one good find; *not* fine
for a completeness claim. The counts printed are of chests probed, never of
chests that exist.

### Cost, honestly

~14 s to boot a world, ~8 s per village, ~2 s per treasure shell. The radius is
the only lever that amortises the boot. Each world is ~60 MB and is deleted as
soon as it has been read — fifty seeds was 2.9 GB before that was added.

Servers get their own port per instance. The default 25565 lingers in TIME_WAIT
between back-to-back worlds, which failed 31 seeds out of 40 in one run while
still printing a tidy "0 found" — so the hunts now report failed seeds loudly
and separately from negative results.
