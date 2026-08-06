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

### Cost, honestly

~14 s to boot a world, ~8 s per village, ~2 s per treasure shell. The radius is
the only lever that amortises the boot. Each world is ~60 MB and is deleted as
soon as it has been read — fifty seeds was 2.9 GB before that was added.

Servers get their own port per instance. The default 25565 lingers in TIME_WAIT
between back-to-back worlds, which failed 31 seeds out of 40 in one run while
still printing a tidy "0 found" — so the hunts now report failed seeds loudly
and separately from negative results.
