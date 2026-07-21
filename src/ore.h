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
