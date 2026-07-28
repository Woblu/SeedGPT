// checkplacement -- re-run the 1.18+ surface placement rule for one structure
// at one position, from scratch, for one seed. Independent of the search path:
// it re-derives the corner heights from real block terrain and prints them, so
// a finder result can be confirmed rather than trusted.
//
// The rules are the ones in the shipped Minecraft classes:
//   desert pyramid (21x21) / jungle temple (12x15): the lowest of four corner
//     heights, measured from the structure chunk's min block, must be >= 63.
//   woodland mansion: a 5x5 box anchored at chunk min + 7, its signs flipped by
//     the rotation the chunk's structure RNG draws first, must be >= 60.
//
// Heights here are SOLID ground (cubiomes' terrain). Minecraft's WORLD_SURFACE_WG
// also counts water, so a corner under water reads higher in game than here --
// which can only make this stricter, never more permissive.
#include "finders.h"
#include "generator.h"
#include "terrain.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
            "usage: checkplacement <seed> <structure> <version> <x> <z>\n"
            "       structures with a surface rule: desert_pyramid jungle_temple mansion\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    const char *what = argv[2];
    int mc = str2mc(argv[3]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[4]), z = atoi(argv[5]);
    uint32_t gflags = (argc > 6 && !strcmp(argv[6], "large")) ? LARGE_BIOMES : 0;

    int w, d, thresh, mansion = 0;
    if (!strcmp(what, "desert_pyramid"))      { w = 21; d = 21; thresh = 63; }
    else if (!strcmp(what, "jungle_temple"))  { w = 12; d = 15; thresh = 63; }
    else if (!strcmp(what, "mansion"))        { w = 5;  d = 5;  thresh = 60; mansion = 1; }
    else { fprintf(stderr, "no surface rule for \"%s\"\n", what); return 2; }

    int x0 = x & ~15, z0 = z & ~15;
    if (mansion) {
        uint64_t rnd = chunkGenerateRnd(seed, x >> 4, z >> 4);
        int rot = nextInt(&rnd, 4);
        w = (rot == 1 || rot == 2) ? -5 : 5;
        d = (rot == 2 || rot == 3) ? -5 : 5;
        x0 += 7; z0 += 7;
    }
    int lo = 1 << 30, ys[4];
    for (int i = 0; i < 4; i++) {
        int ok = 0;
        ys[i] = terrainSurfaceY(mc, seed, gflags, x0 + ((i & 1) ? w : 0),
                                z0 + ((i & 2) ? d : 0), &ok);
        if (!ok) { fprintf(stderr, "no block-level terrain for this version\n"); return 2; }
        if (ys[i] < lo) lo = ys[i];
    }
    printf("%lld %s x=%d z=%d corners=%d,%d,%d,%d lowest=%d need=%d %s\n",
           (long long)seed, what, x, z, ys[0], ys[1], ys[2], ys[3], lo, thresh,
           lo >= thresh ? "generates" : "REJECTED");
    return lo >= thresh ? 0 : 1;
}
