// surface -- the exact block-level surface height at a point, and the terrain
// drop around it. The Java-engine counterpart of probing blocks in a running
// game, so the two can be diffed at a coordinate.
//
//   surface <seed> <version> <x> <z> [halfsize] [large]
//
// Reports the top SOLID block (Minecraft's OCEAN_FLOOR_WG, not WORLD_SURFACE_WG
// -- water is not counted), plus the lowest and highest solid surface within
// +/- halfsize blocks. That drop is what a structure with a filled foundation
// would have to span, which is the whole point of measuring it.
#include "generator.h"
#include "terrain.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: surface <seed> <version> <x> <z> [halfsize] [large]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[3]), z = atoi(argv[4]);
    int half = (argc > 5) ? atoi(argv[5]) : 0;
    uint32_t gflags = 0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "large")) gflags = LARGE_BIOMES;
    if (half < 0)  half = 0;
    if (half > 64) half = 64;      // this is a footprint probe, not a survey

    int ok = 0;
    int centre = terrainSurfaceY(mc, seed, gflags, x, z, &ok);
    if (!ok) { fprintf(stderr, "no block-level terrain for this version (needs 1.18+)\n"); return 2; }

    int lo = centre, hi = centre;
    for (int dx = -half; dx <= half; dx++)
    for (int dz = -half; dz <= half; dz++) {
        int o = 0;
        int y = terrainSurfaceY(mc, seed, gflags, x + dx, z + dz, &o);
        if (!o) continue;
        if (y < lo) lo = y;
        if (y > hi) hi = y;
    }
    printf("%lld x=%d z=%d surface=%d low=%d high=%d drop=%d half=%d\n",
           (long long)seed, x, z, centre, lo, hi, hi - lo, half);
    return 0;
}
