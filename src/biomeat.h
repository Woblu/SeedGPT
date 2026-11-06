// The surface biome at a 1:4 ("quart") position, correct on every version.
//
// The whole codebase used to sample biomes as getBiomeAt(g, 0, qx, 319>>2, qz):
// scale 0, which cubiomes reads as "whatever g->entry is". From 1.18, and in
// the nether and end, that is an ordinary 1:4 lookup. But for the 1.8-1.17
// OVERWORLD g->entry is NULL -- isViableStructurePos points it at a layer for
// the duration of its own check and restores NULL on the way out -- so every
// one of those calls returned `none`. Measured: plains within 600 blocks of
// origin matched 13 of 15 seeds on 1.21 and 0 of 200 on 1.16 and 1.17. Every
// biome, biome-area and island condition on 1.17 and earlier matched nothing,
// and every variant lookup that takes a biome (ruined portal type and burial,
// zombie villages before 1.14) ran with biome -1.
//
// Old versions are asked through the 1:4 layer (scale 4), which is the layer
// cubiomes' own viability check uses for them; y does not exist there.
#pragma once

#include "generator.h"

static inline int surfaceBiomeQ(const Generator *g, int qx, int qz)
{
    if (g->dim == DIM_OVERWORLD && g->mc <= MC_1_17)
        return getBiomeAt(g, 4, qx, 0, qz);
    return getBiomeAt(g, 0, qx, 319 >> 2, qz);
}

// The quart at a chunk's centre -- where structure viability samples, and so
// the right point for "what biome is this structure in".
static inline int chunkBiome(const Generator *g, int blockX, int blockZ)
{
    return surfaceBiomeQ(g, (blockX >> 4) * 4 + 2, (blockZ >> 4) * 4 + 2);
}
