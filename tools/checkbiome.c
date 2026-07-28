// checkbiome -- report the surface biome at a point, from scratch, for one
// seed. Independent of the search path: it re-inits the generator and probes
// the same way viability checks do (y=319>>2 quart column). Used to confirm a
// biome / biome-adjacency result: check that the finder's reported forest
// position really is forest, its desert position really is desert, and (by
// arithmetic on the two) that they are within the requested distance.
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: checkbiome <seed> <version> <x> <z> [expect_biome]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[3]), z = atoi(argv[4]);
    const char *expect = (argc > 5 && strcmp(argv[5], "large")) ? argv[5] : NULL;
    // "large" anywhere in the tail selects the Large Biomes world preset, so a
    // result found in that world can be re-checked in the same world.
    int large = 0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "large")) large = 1;

    // We don't know the biome's dimension a priori; if an expected name is
    // given, generate in that biome's dimension, else default overworld.
    int dim = DIM_OVERWORLD;
    if (expect) {
        for (int id = 0; id < 256; id++) {
            const char *nm = biome2str(mc, id);
            if (nm && !strcmp(nm, expect)) { dim = getDimension(id); break; }
        }
    }

    Generator g; setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
    applySeed(&g, dim, seed);
    int id = getBiomeAt(&g, 0, (x>>4)*4 + 2, 319 >> 2, (z>>4)*4 + 2);
    const char *name = biome2str(mc, id);

    printf("%lld x=%d z=%d biome=%s%s", (long long)seed, x, z,
           name ? name : "?", large ? " [large biomes]" : "");
    if (expect) printf(" expect=%s %s", expect,
                       (name && !strcmp(name, expect)) ? "MATCH" : "MISMATCH");
    printf("\n");
    return (expect && (!name || strcmp(name, expect))) ? 1 : 0;
}
