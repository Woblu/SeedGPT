// Sanity-check what "viable" actually meant: print the biome cubiomes' own
// viability test looks at, plus a naive surface probe for contrast.
//
// Gotcha this exists to document: biomes are 3D since 1.18. Probing
// getBiomeAt(g, 1, x, 63, z) can land in a CAVE biome (lush_caves,
// dripstone_caves) and tell you nothing about the surface. The real check
// samples scale 4 (quart coords) at sampleY=0 -- see isViableStructurePos.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

// Mirror of what isViableStructurePos samples for overworld structures.
static int viabilityBiome(Generator *g, int blockX, int blockZ)
{   // matches the 1.18+ surface sampling isViableStructurePos uses for the
    // grouped overworld structures: scale 0, quart-y 319>>2 (near build height)
    int chunkX = blockX >> 4, chunkZ = blockZ >> 4;
    return getBiomeAt(g, 0, chunkX * 4 + 2, 319 >> 2, chunkZ * 4 + 2);
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: biomecheck seed mx mz vx vz\n"); return 2; }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mx = atoi(argv[2]), mz = atoi(argv[3]);
    int vx = atoi(argv[4]), vz = atoi(argv[5]);

    Generator g;
    setupGenerator(&g, MC_1_21, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    int mb = viabilityBiome(&g, mx, mz);
    int vb = viabilityBiome(&g, vx, vz);
    int mnaive = getBiomeAt(&g, 1, mx, 63, mz);
    Pos spawn = getSpawn(&g);

    printf("seed %lld\n", (long long)seed);
    printf("  mansion (%5d,%5d)  biome=%-18s viable=%d   [naive y=63 probe: %s]\n",
           mx, mz, biome2str(MC_1_21, mb),
           isViableStructurePos(Mansion, &g, mx, mz, 0),
           biome2str(MC_1_21, mnaive));
    printf("  village (%5d,%5d)  biome=%-18s viable=%d\n", vx, vz,
           biome2str(MC_1_21, vb),
           isViableStructurePos(Village, &g, vx, vz, 0));
    printf("  world spawn        x=%d z=%d\n", spawn.x, spawn.z);
    return 0;
}
