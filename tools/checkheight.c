// checkheight -- report the highest APPROXIMATE surface height in a disc around
// a point, from scratch, for one seed. Independent of the search path: it
// re-inits the generator and re-samples mapApproxHeight on the same lattice the
// finder uses. Note this is cubiomes' *approximation* of terrain height (as used
// for spawn finding), NOT exact game height -- so it confirms the finder
// reproduces the same estimate, not that the estimate matches a real world.
#include "generator.h"
#include "biomenoise.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: checkheight <seed> <version> <x> <z> [radius] [precision]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[3]), z = atoi(argv[4]);
    int radius = (argc > 5) ? atoi(argv[5]) : 64;
    int step = 16;
    if (argc > 6) {
        if      (!strcmp(argv[6], "fast"))  step = 64;
        else if (!strcmp(argv[6], "fine"))  step = 16;
        else if (!strcmp(argv[6], "exact")) step = 4;
    }

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    SurfaceNoise sn; SurfaceNoise *snp = NULL;
    if (mc < MC_1_18) { initSurfaceNoise(&sn, DIM_OVERWORLD, seed); snp = &sn; }

    long long lim = (long long)radius * radius;
    int peak = -64, px = x, pz = z;
    for (int dx = -radius; dx <= radius; dx += step)
    for (int dz = -radius; dz <= radius; dz += step) {
        if ((long long)dx*dx + (long long)dz*dz > lim) continue;
        int bx = x + dx, bz = z + dz;
        float y = 0;
        mapApproxHeight(&y, NULL, &g, snp, bx >> 2, bz >> 2, 1, 1);
        if ((int)y > peak) { peak = (int)y; px = bx; pz = bz; }
    }
    printf("%lld x=%d z=%d r=%d step=%d peak=%d at=(%d,%d)\n",
           (long long)seed, x, z, radius, step, peak, px, pz);
    return 0;
}
