#ifndef SC_CACTUS_H
#define SC_CACTUS_H

#include "generator.h"
#include <stdint.h>

// Simulating cactus decoration, block for block.
//
// There is no "tall cactus" code in Minecraft. One placement is 1-3 blocks
// (`biased_to_bottom` 1..3), so a 10-block cactus -- never mind the 22-block
// records -- is placements piling onto one column. That happens because the
// patch's y_spread is 3, so a later try can land on top of cactus an earlier try
// just placed, and because a chunk's cacti reach ~23 blocks out, so neighbouring
// chunks feed the same column. Real height therefore needs a real simulation:
// terrain, sand, what is already standing there, and the RNG in exact order.
//
// WHY THE RNG ORDER IS UNFORGIVING. A try whose target fails the block predicate
// consumes NO random numbers, while a try that succeeds consumes two (the height
// sample). So mispredicting a single placement desynchronises every later try in
// the patch. There is no "roughly right" version of this: it either tracks the
// game or it produces confident nonsense.
//
// Everything below was read out of 1.21.1's own source and data, and the whole
// thing is checked block-for-block against a real server by
// tier3-java/check_cactus.py.

#define CACTUS_STEP          9      // vegetal decoration
#define CACTUS_INDEX_DESERT  74     // from the game's own FeatureSorter -- see
#define CACTUS_INDEX_BADLANDS 75    // docs/cactus-generation.md
#define CACTUS_RARITY_DESERT  6     // patch_cactus_desert
#define CACTUS_RARITY_BADLANDS 13   // patch_cactus_decorated
#define CACTUS_TRIES          10
#define CACTUS_XZ_SPREAD      7
#define CACTUS_Y_SPREAD       3

typedef struct {
    int x, z;        // column
    int baseY;       // world Y of the lowest cactus block
    int height;      // blocks standing in that column
} Cactus;

// Simulate every cactus in the chunk region [cx0,cx1] x [cz0,cz1] and report
// them. Chunks up to 2 outside the region are simulated too, because their
// patches reach in; only cacti whose column lies inside are reported.
//
// Returns the number written, or -1 if this version has no block terrain
// (pre-1.18). `out` may be NULL to count only.
int cactusRegion(Generator *g, int mc, uint32_t gflags, uint64_t worldSeed,
                 int cx0, int cz0, int cx1, int cz1, Cactus *out, int maxOut);

// The tallest cactus in a disc of `radius` blocks around (x,z), or 0 if none.
// Fills *where when non-NULL. This is what a search condition calls.
int cactusTallest(Generator *g, int mc, uint32_t gflags, uint64_t worldSeed,
                  int x, int z, int radius, Cactus *where);

#endif
