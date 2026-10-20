// checkregion -- does generateRegion give the same surface heights for a
// non-square region as for 1x1-chunk regions?
//
// generateRegion memoises noise columns in a W x H grid of cells and indexed it
// as relCellX * W + relCellZ, which is right only when W == H. A region wider
// than tall overflowed the buffer; one taller than wide aliased cells and came
// back with wrong heights. exactFootprint asks for rectangles whenever its
// centre is off-chunk, so exact terrain, exposed ore, cave_below and stronghold
// depth all went through it. patches/terrainnoise-region-stride.patch fixes the
// stride; this is the regression check (a 1x1 region is square, so it is the
// independent reference).
//
//   usage: checkregion [seed]      exit 0 = every column agrees
#include "terrainnoise.h"
#include <stdio.h>
#include <stdlib.h>

static TerrainNoise tn;     // large, and set up ONCE: a second setup crashes

static int run(int W, int H, int cx0, int cz0)
{
    int bw = W << 4, bh = H << 4;
    int *big = malloc(sizeof(int) * bw * bh);
    if (!big) return 1;
    generateRegion(&tn, cx0, cz0, W, H, NULL, big, 1);
    int bad = 0, total = 0;
    for (int i = 0; i < W; i++)
    for (int j = 0; j < H; j++) {
        int one[256];
        generateRegion(&tn, cx0 + i, cz0 + j, 1, 1, NULL, one, 1);
        for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            total++;
            if (big[((i << 4) + x) * bh + ((j << 4) + z)] != one[x * 16 + z]) bad++;
        }
    }
    printf("%dx%d chunks at (%d,%d): %d of %d columns differ from 1x1 generation\n",
           W, H, cx0, cz0, bad, total);
    free(big);
    return bad != 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = argc > 1 ? (uint64_t)strtoll(argv[1], NULL, 10) : 12345;
    if (!setupTerrainNoise(&tn, MC_1_21, 0)) { puts("terrain setup failed"); return 2; }
    initTerrainNoise(&tn, seed, DIM_OVERWORLD);
    int r = 0;
    r |= run(4, 3, -2, -1);   // wider than tall: overflowed the memo
    r |= run(3, 4, -2, -1);   // taller than wide: aliased cells
    r |= run(5, 2, 7, -9);
    r |= run(3, 3, -2, -1);   // square: control
    return r;
}
