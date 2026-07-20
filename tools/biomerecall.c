// biomerecall -- measure what a biome scan actually finds, and what it costs.
//
//   usage: biomerecall [radius] [seeds] [biome]
//
// Biome conditions sample points across a disc; the spacing trades recall
// against cost. This measures both against an exhaustive quart-resolution scan
// (genBiomes at scale 4), which is ground truth on 1.18+.
//
// It exists so the SCAN_FAST/FINE/EXACT numbers documented in src/query.h are
// measured rather than assumed -- re-run it if the engine or MC version moves.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MC MC_1_21

// Current implementation: sparse point samples every `step` blocks.
static int latticeHit(Generator *g, int cx, int cz, int within, int biome, int step)
{
    int64_t lim = (int64_t)within * within;
    for (int dx = -within; dx <= within; dx += step)
    for (int dz = -within; dz <= within; dz += step) {
        if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
        int bx = cx + dx, bz = cz + dz;
        if (getBiomeAt(g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2) == biome) return 1;
    }
    return 0;
}

// Complete area generation at `scale` (1 cell = `scale` blocks), one call.
static int areaHit(Generator *g, int cx, int cz, int within, int biome,
                   int scale, int *cache)
{
    int c0x = (cx - within) / scale, c1x = (cx + within) / scale;
    int c0z = (cz - within) / scale, c1z = (cz + within) / scale;
    int sx = c1x - c0x + 1, sz = c1z - c0z + 1;
    Range r = {scale, c0x, c0z, sx, sz, 319 >> 2, 1};
    if (genBiomes(g, cache, r)) return -1;
    int64_t lim = (int64_t)within * within;
    for (int iz = 0; iz < sz; iz++)
    for (int ix = 0; ix < sx; ix++) {
        int64_t dx = (int64_t)(c0x + ix) * scale - cx;
        int64_t dz = (int64_t)(c0z + iz) * scale - cz;
        if (dx*dx + dz*dz > lim) continue;
        if (cache[iz*sx + ix] == biome) return 1;
    }
    return 0;
}

typedef struct { const char *name; int kind; int param; } Strategy;
// kind 0 = point lattice with step=param, kind 1 = genBiomes at scale=param

int main(int argc, char **argv)
{
    int within = (argc > 1) ? atoi(argv[1]) : 400;
    int nseeds = (argc > 2) ? atoi(argv[2]) : 300;
    const char *bname = (argc > 3) ? argv[3] : "jungle";

    int biome = -1;
    for (int i = 0; i < 256; i++) {
        const char *n = biome2str(MC, i);
        if (n && !strcmp(n, bname)) { biome = i; break; }
    }
    if (biome < 0) { fprintf(stderr, "unknown biome %s\n", bname); return 2; }

    Strategy strat[] = {
        {"lattice step=64 (current)", 0, 64},
        {"lattice step=16",           0, 16},
        {"genBiomes scale=64",        1, 64},
        {"genBiomes scale=16",        1, 16},
        {"genBiomes scale=4 (truth)", 1,  4},
    };
    int ns = (int)(sizeof(strat)/sizeof(strat[0]));

    Generator g;
    setupGenerator(&g, MC, 0);
    int qspan = (2*within)/4 + 8;
    Range rmax = {4, 0, 0, qspan, qspan, 319 >> 2, 1};
    int *cache = allocCache(&g, rmax);

    int *hits = calloc(nseeds * ns, sizeof(int));
    double *tsum = calloc(ns, sizeof(double));
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);

    for (int s = 0; s < nseeds; s++) {
        applySeed(&g, DIM_OVERWORLD, (uint64_t)s);
        for (int k = 0; k < ns; k++) {
            QueryPerformanceCounter(&a);
            int h = strat[k].kind == 0
                  ? latticeHit(&g, 0, 0, within, biome, strat[k].param)
                  : areaHit(&g, 0, 0, within, biome, strat[k].param, cache);
            QueryPerformanceCounter(&b);
            tsum[k] += (double)(b.QuadPart - a.QuadPart) / f.QuadPart;
            if (h < 0) { fprintf(stderr, "genBiomes failed at scale %d\n", strat[k].param); return 1; }
            hits[s*ns + k] = h;
        }
    }

    int truthIdx = ns - 1;
    int trueMatches = 0;
    for (int s = 0; s < nseeds; s++) if (hits[s*ns + truthIdx]) trueMatches++;

    printf("biome=%s radius=%d seeds=%d  (true matches: %d)\n\n",
           bname, within, nseeds, trueMatches);
    printf("  %-28s %8s %8s %10s\n", "strategy", "missed", "recall", "us/seed");
    for (int k = 0; k < ns; k++) {
        int miss = 0, fp = 0;
        for (int s = 0; s < nseeds; s++) {
            if (hits[s*ns+truthIdx] && !hits[s*ns+k]) miss++;
            if (!hits[s*ns+truthIdx] && hits[s*ns+k]) fp++;
        }
        printf("  %-28s %8d %7.1f%% %10.0f%s\n", strat[k].name, miss,
               trueMatches ? 100.0*(trueMatches-miss)/trueMatches : 100.0,
               tsum[k]*1e6/nseeds, fp ? "  <-- FALSE POSITIVES" : "");
    }
    free(cache); free(hits); free(tsum);
    return 0;
}
