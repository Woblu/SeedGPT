// dumpcol -- print the solid/air profile of ONE column from the EXACT terrain.
//
//   dumpcol <seed> <version> <x> <z>
//
// Written to settle where a floating-island claim came from. The condition said
// an island underside sat at y=103 in a column whose real surface is y=62, and
// the approximate height agreed with the game -- so the disagreement had to be
// in how the exact column is read, not in the terrain. This prints the column
// itself rather than any interpretation of it.
#include "terrain.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: dumpcol <seed> <version> <x> <z>\n"); return 2; }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[3]), z = atoi(argv[4]);

    int top = 0;
    int v = terrainVoidBelow(mc, seed, 0, x, z, 128, &top);
    int cap = 0;
    int f = terrainFloating(mc, seed, 0, x, z, 1, 24, 1, &cap);
    printf("seed %lld (%d,%d)\n", (long long)seed, x, z);
    printf("  terrainVoidBelow : %d blocks, top of void at y=%d\n", v, top);
    printf("  terrainFloating  : %d, cap (underside) y=%d\n", f, cap);

    int ok = 0;
    int h = terrainSurfaceY(mc, seed, 0, x, z, &ok);
    printf("  terrainSurface   : y=%d (ok=%d)\n", h, ok);
    return 0;
}
