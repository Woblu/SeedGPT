// climatecheck -- prove the climate pre-filter never throws away a good seed.
//
//   climatecheck <version> <seeds> [large]
//
// The filter's whole claim is that it is exact: it rejects only where the biome
// provably cannot generate. That claim rests on two things, and this checks
// both against the real generator rather than against my reading of it.
//
//   1. BIT-EXACTNESS. The temperature the filter computes must be the identical
//      int64 the full biome sampler hands to climateToBiome -- same shifted
//      coordinate, same float truncation. Off by one unit and the comparison
//      against a biome's limits is no longer sound at the boundary.
//
//   2. CONTAINMENT. Wherever the generator actually places biome B, that
//      temperature must lie inside B's own parameter limits. This is the step
//      that would catch getBiomeParaLimits meaning something subtler than
//      "the range in which this biome can occur".
//
// A single failure of either means the filter can silently lose seeds, which is
// worse than not having it.
#include "generator.h"
#include "biomenoise.h"
#include "finders.h"
#include "climate.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: climatecheck <version> <seeds> [large]\n");
        return 2;
    }
    int mc = str2mc(argv[1]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int nseeds = atoi(argv[2]);
    int large = (argc > 3 && !strcmp(argv[3], "large")) ? 1 : 0;

    Generator g;
    setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);

    long long checked = 0, bitfail = 0, containfail = 0, nolimits = 0;
    int64_t worst = 0;   // largest excursion outside a biome's own box
    // Points spread far enough apart to land in unrelated climate, and offset
    // off the origin so the sample is not all one continent.
    const int PTS = 64;

    for (int s = 0; s < nseeds; s++) {
        uint64_t seed = (uint64_t)(s * 0x9E3779B97F4A7C15ULL + 12345);
        applySeed(&g, DIM_OVERWORLD, seed);

        for (int p = 0; p < PTS; p++) {
            // Quart coordinates, taken the way query.c takes them: the centre
            // of a chunk, at the surface probe height.
            int bx = ((p * 7919) % 20000) - 10000;
            int bz = ((p * 104729) % 20000) - 10000;
            int qx = (bx >> 4) * 4 + 2, qz = (bz >> 4) * 4 + 2;

            int64_t np[NP_MAX];
            int id = sampleBiomeNoise(&g.bn, np, qx, 319 >> 2, qz, NULL, 0);
            int64_t mine = climateTempAt(mc, seed, large, qx, qz);
            checked++;

            if (mine != np[0]) {
                if (bitfail < 5)
                    printf("BIT   seed=%llu (%d,%d) filter=%lld generator=%lld\n",
                           (unsigned long long)seed, bx, bz,
                           (long long)mine, (long long)np[0]);
                bitfail++;
                continue;   // containment is meaningless if the value is wrong
            }

            const int *lim = getBiomeParaLimits(mc, id);
            if (!lim) { nolimits++; continue; }
            // How far OUTSIDE its own box did this biome generate? Not always
            // zero: climateToBiome picks the nearest biome in six dimensions,
            // not the one whose box contains the point, so a biome can win just
            // past its edge when the alternatives are worse on other axes.
            int64_t over = 0;
            if (mine < lim[0]) over = lim[0] - mine;
            if (mine > lim[1]) over = mine - lim[1];
            if (over > 0) {
                if (over > worst) {
                    worst = over;
                    printf("RANGE seed=%llu (%d,%d) biome=%s t=%lld outside "
                           "[%d,%d] by %lld\n",
                           (unsigned long long)seed, bx, bz, biome2str(mc, id),
                           (long long)mine, lim[0], lim[1], (long long)over);
                }
                containfail++;
            }
        }
    }

    printf("checked %lld samples over %d seeds (%s)\n", checked, nseeds,
           large ? "large biomes" : "default");
    printf("  bit-exact mismatches: %lld\n", bitfail);
    printf("  generated outside its own box: %lld (worst by %lld units)\n",
           containfail, (long long)worst);
    printf("  biomes with no published limits (not filtered): %lld\n", nolimits);
    if (bitfail) {
        printf("UNSOUND: the filter does not reproduce the generator\n");
        return 1;
    }
    printf("  margin needed %lld, CLIMATE_MARGIN is %d\n",
           (long long)worst, CLIMATE_MARGIN);
    if (worst >= CLIMATE_MARGIN) {
        printf("UNSOUND: margin too small\n");
        return 1;
    }
    printf("SOUND\n");
    return 0;
}
