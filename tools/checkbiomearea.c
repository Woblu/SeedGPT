// checkbiomearea -- measure what fraction of a disc is a given biome, from
// scratch, for one seed. Independent of the search path: it re-inits the
// generator and re-samples the same scanStep lattice the finder uses, so a
// finder result can be confirmed rather than trusted. The sampled fraction is
// what CT_BIOME_AREA thresholds on; matching it exactly proves the plumbing.
//
// Precision must match the finder's (fast|fine|exact -> 64|16|4 block step);
// defaults to fine, the finder's default.
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: checkbiomearea <seed> <biome> [version] [x] [z] [radius] [precision]\n"
            "       precision: fast|fine|exact  (default fine)\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    const char *biome = argv[2];
    int mc = (argc > 3) ? str2mc(argv[3]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = (argc > 4) ? atoi(argv[4]) : 0;
    int z = (argc > 5) ? atoi(argv[5]) : 0;
    int radius = (argc > 6) ? atoi(argv[6]) : 800;
    int step = 16;
    if (argc > 7) {
        if      (!strcmp(argv[7], "fast"))  step = 64;
        else if (!strcmp(argv[7], "fine"))  step = 16;
        else if (!strcmp(argv[7], "exact")) step = 4;
        else { fprintf(stderr, "precision must be fast|fine|exact\n"); return 2; }
    }

    int biomeId = -1;
    for (int id = 0; id < 256; id++) {
        const char *nm = biome2str(mc, id);
        if (nm && !strcmp(nm, biome)) { biomeId = id; break; }
    }
    if (biomeId < 0) { fprintf(stderr, "unknown biome \"%s\"\n", biome); return 2; }
    int dim = getDimension(biomeId);

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, dim, seed);

    long long lim = (long long)radius * radius;
    int total = 0, match = 0;
    for (int dx = -radius; dx <= radius; dx += step)
    for (int dz = -radius; dz <= radius; dz += step) {
        if ((long long)dx*dx + (long long)dz*dz > lim) continue;
        int bx = x + dx, bz = z + dz;
        total++;
        int id = getBiomeAt(&g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2);
        if (id == biomeId) match++;
    }
    int pct = total > 0 ? (int)((long long)match * 100 / total) : 0;
    printf("%lld %s x=%d z=%d r=%d step=%d match=%d total=%d pct=%d\n",
           (long long)seed, biome, x, z, radius, step, match, total, pct);
    return 0;
}
