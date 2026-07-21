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
            "usage: checkore <seed> <material> [version] [x] [z] [radius]\n"
            "       materials: diamond iron gold emerald redstone lapis copper coal quartz ancient_debris nether_gold\n");
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

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, mat->dim, seed);
    SurfaceNoise sn; initSurfaceNoise(&sn, mat->dim, seed);

    int n = oreCountMaterial(&g, &sn, mc, mat, x, z, radius);
    printf("%lld %s x=%d z=%d r=%d dim=%d count=%d\n",
           (long long)seed, mat->name, x, z, radius, mat->dim, n);
    return 0;
}
