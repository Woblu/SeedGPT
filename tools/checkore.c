// checkore -- count ore blocks of a material around a point, from scratch, for
// one seed. Independent of the search path: it re-inits the generator and
// surface noise and re-counts, so a finder result can be confirmed rather than
// trusted. cubiomes models Minecraft's ore placement exactly (getOreConfig ->
// generateOres); the count is that model, deduplicated across configs.
#include "ore.h"
#include "generator.h"
#include "biomenoise.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: checkore <seed> <material> [version] [x] [z] [radius] [exposed]\n"
            "       materials: diamond iron gold emerald redstone lapis copper coal quartz ancient_debris nether_gold\n"
            "       exposed: pass \"exposed\" to count only ore with a non-solid\n"
            "                neighbour in real 1.18+ terrain (cave/ravine/water)\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    const OreMaterial *mat = oreMaterialByName(argv[2]);
    if (!mat) { fprintf(stderr, "unknown material \"%s\"\n", argv[2]); return 2; }
    int mc = (argc > 3) ? str2mc(argv[3]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = (argc > 4) ? atoi(argv[4]) : 0;
    int z = (argc > 5) ? atoi(argv[5]) : 0;
    int radius = (argc > 6) ? atoi(argv[6]) : 128;
    int exposed = 0, large = 0;
    for (int i = 7; i < argc; i++) {
        if (!strcmp(argv[i], "exposed"))      exposed = 1;
        else if (!strcmp(argv[i], "large"))   large = 1;
    }

    Generator g; setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
    applySeed(&g, mat->dim, seed);
    SurfaceNoise sn; initSurfaceNoise(&sn, mat->dim, seed);

    // The vein figure is the largest face-connected blob among those blocks --
    // the same measure the finder's "vein" threshold and leaderboard use.
    int vein = 0;
    int n = oreScan(&g, &sn, mc, large ? LARGE_BIOMES : 0, mat, x, z, radius,
                    exposed, &vein);
    printf("%lld %s x=%d z=%d r=%d dim=%d count=%d vein=%d%s\n",
           (long long)seed, mat->name, x, z, radius, mat->dim, n, vein,
           exposed ? " (exposed only)" : "");
    return 0;
}
