# Reversal: working backwards from the RNG

Status: milestone 1 **done** (`src/lcg.h`, checked against `java.util.Random`);
the rest is plan. `src/invert.c` and `src/mitm.c` already reverse structure
placement.

## Why this is the gap

This finder works *forwards*: enumerate seeds, generate, test. That is the
right shape for anything decided by biomes (pass 2 dominates, and no amount of
cleverness in pass 1 helps), and it is how every consumer tool works.

Professional seedfinders work *backwards* on the targets that make records.
Kris Kludwisz's public repositories are the clearest examples:

- **DungeonChain** — a GPU kernel brute-forces *chunk* seeds that produce a
  chain of dungeons, pairs them, converts chunk-seed pairs to 48-bit world
  seeds, then filters the sister seeds by biome.
- **TwoChunkCarverReversal** — solves the carver-seed equation for two chunks
  by looping one unknown and Hensel-lifting the other bit by bit, then maps the
  result back to a seed with Matthew Bolan's `NextLongReverser`.
- **ChestStack**, **EndIslandPillars** — CUDA plus reversal, split into task
  ids so many machines can share the range.

The libraries behind this are public: LattiCG (lattice reduction for "find the
Java Random states satisfying these inequalities"), Bolan's
`ChunkRandomReversal` and `BoundNextIntSkips`, KaptainWutax's SEED /
FeatureUtils / MCUtils. SeedcrackerX documents the same structure: structure
seeds from 32 + 40 liftable bits, then the upper bits by brute force.

The point is not speed for its own sake. A target defined by *decoration RNG*
(a dungeon floor pattern, a chest stack, a pillar arrangement) has no biome
step to dominate the cost, so the whole filter is integer RNG math — which is
exactly what reverses algebraically and what GPUs run at 10^9–10^10 per second.
That is also the one place this project's "GPU is not worth it" measurement
stops applying.

## The version boundary

Pre-1.18, population and decorator seeds are built from the 48-bit Java LCG
state, so a chunk-level RNG target reverses to a 48-bit structure seed and the
upper 16 bits are free. **From 1.18 the population seed comes from Xoroshiro
seeded with all 64 bits** (see the loot fix: `getPopulationSeed`), so that
shortcut is gone; SeedcrackerX's dungeon/decorator cracking stops at 1.17 for
the same reason. For 1.18+ decoration targets, the realistic tool is a fast
forward brute force (GPU) over full 64-bit seeds, not algebra.

## Milestones

Each one ends with a check against an independent reference, because every
negative result in this project's history that was not checked turned out to
be a bug.

1. **LCG primitives** (`src/lcg.h`, small) — **done.** Forward/backward steps, skip-n in
   O(log n), `nextLong` reversal (the 2^16-candidate inversion), `nextInt(bound)`
   with the rejection loop. *Check:* `tools/lcgcheck self` (round trips, 0
   failures) and `tools/lcgcheck table` vs `tools/LcgCheck.java` on the real
   `java.util.Random` -- identical over 207 seeds; both run in `test.sh`.

2. **Population-seed inversion, pre-1.18** (`src/popinvert.c`, medium). Given a
   chunk and a target population/decorator RNG state, list every 48-bit
   structure seed that produces it (`pop = (x*a + z*b) ^ ws` with `a`, `b` drawn
   from `ws` — the two-unknown problem TwoChunkCarverReversal solves by
   Hensel lifting). *Check:* forward `getPopulationSeed` on every candidate, and
   a completeness proof on small bit widths the way `tools/invert.c` does for
   structure placement.

3. **One decoration target, end to end** (medium–large). Dungeons are the
   community's most-wanted and the classic reversal target: per-chunk attempts,
   positions from `nextInt`, a floor of mossy/plain cobblestone drawn from the
   RNG. The RNG part is exact; the TERRAIN part (is there an air pocket with a
   floor) is not, and needs the tier-2/3 oracles to confirm — the same split as
   cactus height. *Check:* every candidate confirmed on a real 1.16.5/1.17.1
   server before it is reported, like the casing hunt.

4. **GPU kernel for the brute-force stage** (medium). The GTX 1660 SUPER on
   this machine runs compute 7.5. Start from the chunk-seed stage of milestone
   3, mirror the CPU code exactly, and require the two to agree on a sampled
   range before trusting either. Needs the CUDA toolkit (elevated install).

5. **Task splitting for many machines** (small, builds on what exists). `find`
   already reports a gap-free `FIND_OFFSET` resume point and builds off Windows
   (`src/platform.h`); add a task-id → range mapping and a results format a
   coordinator can merge.

## Not in scope

- 1.18+ decoration reversal by algebra (see the version boundary).
- Anything that would need Minecraft's code in this repository; the tier-2
  backends read it at build time and it stays out of git.
