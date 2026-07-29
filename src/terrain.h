// Real block-level terrain, shared by every condition that needs actual blocks
// rather than the smoothed height estimate.
//
// cubiomes' TerrainNoise is expensive to set up (splines + ~45 octaves of
// Perlin) and must be re-initialised per world seed, so it is kept THREAD-LOCAL
// and reused across the many candidate seeds one worker thread evaluates.
//
// UNITS. generateColumn/generateRegion report heights in COLUMN INDEX space:
// index 0 is world Y -64, and the returned "first air above solid" is
// index+1. Callers want world Y, so everything here converts:
//     world Y of the top solid block = ys[i] - TERRAIN_Y_BIAS
// Getting this wrong is silent -- the numbers still look like plausible
// heights, just 65 blocks too high.
#pragma once

#include "terrainnoise.h"
#include <stdint.h>

#define TERRAIN_Y_BIAS   65    // index space -> world Y (see above)
#define TERRAIN_COLUMN  384    // blocks per column: world Y -64 .. 319
#define TERRAIN_Y_MIN   (-64)

// Thread-local terrain context for this version, seed and world type, or NULL
// when the version has no block-level terrain model (pre-1.18). Cheap on repeat
// calls with the same seed. `flags` are the setupGenerator flags of the query
// (LARGE_BIOMES), so terrain matches the world the search is describing.
TerrainNoise *terrainFor(int mc, uint64_t worldSeed, uint32_t flags);

// Tallest run of non-solid blocks strictly BELOW the surface at (x,z), within
// `depth` blocks of it -- the cavern under a structure. Returns 0 when the
// version has no block terrain. Noise caves are part of the density function,
// so this sees the real 1.18+ cave systems, not just carver tunnels.
int terrainVoidBelow(int mc, uint64_t worldSeed, uint32_t flags, int x, int z,
                     int depth, int *topOut);

// Top solid block's world Y at (x,z), or 0 on failure (with *ok cleared).
//
// This is ONE column: four noise columns and an interpolation, rather than the
// 256 columns a whole chunk costs. Sampling a handful of scattered points (the
// corners of a structure footprint) that way is ~60x cheaper than generating
// the chunks they fall in. Noise columns are cached per thread, so the shared
// corners of a footprint are computed once.
//
// It reports SOLID terrain, matching Minecraft's OCEAN_FLOOR_WG heightmap.
// Minecraft's WORLD_SURFACE_WG additionally counts water, so a submerged column
// reads higher there than here -- see the placement checks in query.c for why
// that difference is deliberately left in the safe direction.
int terrainSurfaceY(int mc, uint64_t worldSeed, uint32_t flags, int x, int z, int *ok);
