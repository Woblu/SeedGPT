// Ore-density counting. cubiomes models Minecraft's ore placement exactly
// (getOreConfig -> generateOres), but by *feature*: "diamond" is produced by
// several configs (regular, buried, large, medium) whose set changes by
// version. Rather than hardcode those families, we match on the BLOCK a config
// places (config.oreBlock == DIAMOND_ORE), which captures every diamond source
// in any version automatically.
//
// This is a pass-2, biome-dependent, per-chunk cost -- the most expensive
// condition type -- so radii are capped small (see query.c).
#pragma once

#include "generator.h"
#include "finders.h"

// A user-facing material maps to one placed block id (enum Blocks).
typedef struct { const char *name; int block; int dim; } OreMaterial;

// Returns the material table (NULL-terminated) and its length via *n.
const OreMaterial *oreMaterials(int *n);

// Look up a material by user name ("diamond"); NULL if unknown.
const OreMaterial *oreMaterialByName(const char *name);

// Count distinct ore blocks of `mat` within `radius` blocks of (cx,cz) in the
// x/z plane (any y). `sn` must be initialised for the material's dimension and
// this seed. Positions are de-duplicated so overlapping configs don't double
// count. Returns the count.
int oreCountMaterial(const Generator *g, const SurfaceNoise *sn, int mc,
                     const OreMaterial *mat, int cx, int cz, int radius);

// The same scan, with the two record-hunting measurements layered on top.
//
//   veinMax   (optional) largest CONNECTED blob of this ore -- the thing a
//             player actually mines in one go. Blocks touching face-to-face
//             count as one vein, so two placements that happen to overlap merge
//             into a bigger one. That merging is exactly where records come
//             from, which is why this is not just "the placement size".
//   exposed   if non-zero, the returned count is restricted to blocks with at
//             least one AIR neighbour in real 1.18+ terrain (generateColumn) --
//             ore visible in a cave wall rather than sealed in stone.
//
// `exposed` needs mc >= 1.18 and the overworld, and costs a terrain column per
// candidate chunk (milliseconds). It under-reports at chunk borders: a
// neighbour outside the generated chunk is treated as solid, which can only
// miss an exposed block, never invent one.
int oreScan(const Generator *g, const SurfaceNoise *sn, int mc, uint32_t gflags,
            const OreMaterial *mat, int cx, int cz, int radius,
            int exposed, int *veinMax);
