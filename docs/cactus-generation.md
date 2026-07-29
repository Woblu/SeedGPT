# Tall cacti: the exact algorithm

Everything needed to simulate cactus generation, read out of the game's own
data and decompiled source (MC 1.21.1) with `tools/mcsrc.py`. Written down so
building the search is a mechanical exercise rather than a research one.

**Why this is hard at all.** A single placement is only **1–3 blocks**. A
10-block cactus — let alone Minecraft@Home's 22-block record — exists only where
placements *stack on the same column*, across the ten tries of one feature,
across neighbouring chunks whose spread reaches the same spot, and across
however many chunks get to contribute. There is no "tall cactus" code path to
look up; the height is an emergent property of the decoration RNG.

## The placed feature (`patch_cactus_desert`)

```json
"placement": [
  { "type": "rarity_filter", "chance": 6 },   // 1 chunk in 6 even tries
  { "type": "in_square" },                    // nextInt(16) in x and z
  { "type": "heightmap", "heightmap": "MOTION_BLOCKING" },
  { "type": "biome" }
]
```

## The feature (`patch_cactus`, a `random_patch`)

```json
"tries": 10, "xz_spread": 7, "y_spread": 3
```

`RandomPatchFeature.place`, with `j = xz_spread + 1 = 8`, `k = y_spread + 1 = 4`:

```java
for (int l = 0; l < tries; l++) {
    mutableBlockPos.setWithOffset(origin,
        random.nextInt(j) - random.nextInt(j),     // x
        random.nextInt(k) - random.nextInt(k),     // y
        random.nextInt(j) - random.nextInt(j));    // z
    inner.place(level, chunkGenerator, random, mutableBlockPos);
}
```

Confirmed against the real `RandomPatchFeature.java`, not paraphrased: the loop
body is exactly the three `nextInt(j) - nextInt(j)` style offsets above, and the
success counter only increments when the inner feature actually places.

The inner feature is a `block_column`: one layer of cactus whose height is
`biased_to_bottom(min 1, max 3)`, placed upward, gated by a
`block_predicate_filter` requiring the target block to be **air** and a cactus to
**survive** there (sand or cactus below, no solid block horizontally adjacent).

`BiasedToBottomInt.sample` is **two** draws, not one:

```java
return this.minInclusive
     + randomSource.nextInt(randomSource.nextInt(this.maxInclusive - this.minInclusive + 1) + 1);
```

For 1..3 that is `1 + nextInt(nextInt(3) + 1)`. Reading it as a single draw
would consume one fewer number and desynchronise every later try in the patch.

**That is where stacking comes from.** `y_spread` is 3, so a later try can land
up to three blocks above the origin — on top of cactus a previous try just
placed, because cactus counts as valid ground for cactus. Beyond that, one
column can be reached by placements from neighbouring chunks: `in_square` puts
the origin anywhere in a 16×16 chunk and `xz_spread` reaches 7 further, so a
chunk's cacti can land ~23 blocks out.

## Seeding

```
populationSeed = getPopulationSeed(mc, worldSeed, chunkMinX, chunkMinZ)   // cubiomes has this
featureSeed    = populationSeed + index + 10000 * step
```

Counting through `desert.json` puts `patch_cactus_desert` 47th of 49, in step 9.
**That is the wrong number**, and it is the trap that would have sunk a first
attempt. `ChunkGenerator.applyBiomeDecoration` does not count per biome:

```java
holderSet.stream().map(Holder::value)
         .forEach(pf -> intSet.add(stepFeatureData.indexMapping().applyAsInt(pf)));
...
worldgenRandom.setFeatureSeed(l, p, k);   // p from that index set, not a counter
```

`p` is a **global** index into the topologically sorted list of every placed
feature that any biome in the world contributes to that step, built by
`FeatureSorter` over `biomeSource.possibleBiomes()`. An index that is merely
close still yields cacti — just cacti in the wrong places, which is exactly the
failure this project exists to avoid.

Rather than reimplement that sort and hope, `tier2-outpost/FeatureIndex.java`
asks the game's own sorter, with the arguments `ChunkGenerator` passes:

| | step | index |
|---|---|---|
| `minecraft:patch_cactus_desert` | 9 | **74** |
| `minecraft:patch_cactus_decorated` | 9 | **75** |

so `featureSeed = populationSeed + 74 + 90000`.

**Why that is trustworthy.** cubiomes carries a hand-maintained `{index, step}`
table for the ore features, and our ore search is already tested against real
worlds. `tier2-outpost/check_feature_index.sh` runs FeatureIndex against it:
all eight cross-checkable entries match exactly (dirt 0, diamond 18, buried
diamond 21, buried lapis 23, copper 25, clay 27, extra gold 28, emerald 33). A
tool that reproduces eight known-good indices is trustworthy for the ninth.

Note there are **two** cactus features. Badlands-family biomes pull
`patch_cactus_decorated`, so a column near a desert/badlands boundary can be
fed by both, at different feature seeds.

## What is still missing

The seeding is solved and verified. Three things are not.

**The exact heightmap.** The placement rides `MOTION_BLOCKING`, which counts
water and leaves; `terrain.c` computes `OCEAN_FLOOR_WG`, which does not. On open
desert sand the two coincide, but "usually coincide" is not a basis for a record
claim, and cacti near water or at a biome edge are exactly where the interesting
columns are.

**Block-level survival.** `would_survive` asks whether the block below is sand
or cactus and whether any horizontal neighbour is solid. That needs the surface
*material* and its neighbours, not just a surface height — a layer above what
`terrain.c` currently models.

**End-to-end confirmation.** Every other correctness claim here was checked
against something independent: a JDK reference, a headless generator, Bedrock
Dedicated Server, or — for the feature index above — cubiomes' verified table.
There is no such source for a finished cactus column. The headless backend stops
before decoration, because features need a `WorldGenLevel`.

The fix is the trick that worked for Bedrock: run a real Java server, generate
the world, probe blocks with `/execute if block`. That would confirm not only
cacti but `cave_below` and the 1.18+ placement rules, which currently rest on
our own reimplementation. It needs the Mojang EULA accepted on this machine,
which is not a call to make silently.
