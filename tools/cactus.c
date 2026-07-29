// cactus -- list the cacti our simulation says a region contains.
//
//   cactus <seed> <version> <x> <z> [radius] [large]
//
// Prints one line per cactus column, tallest first:
//
//   CACTUS <x> <z> <baseY> <height>
//
// Machine-readable on purpose: tier3-java/check_cactus.py diffs this against a
// real server block for block, which is the only reason to believe any of it.
#include "cactus.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int cactusTrace;

static int bycol(const void *a, const void *b)
{
    const Cactus *p = a, *q = b;
    if (p->height != q->height) return q->height - p->height;
    if (p->x != q->x) return p->x - q->x;
    return p->z - q->z;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: cactus <seed> <version> <x> <z> [radius] [large]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[3]), z = atoi(argv[4]);
    int radius = (argc > 5) ? atoi(argv[5]) : 64;
    uint32_t gflags = 0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "large")) gflags = LARGE_BIOMES;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "trace")) cactusTrace = 1;

    Generator g;
    setupGenerator(&g, mc, gflags);
    applySeed(&g, DIM_OVERWORLD, seed);

    int cx0 = (x - radius) >> 4, cx1 = (x + radius) >> 4;
    int cz0 = (z - radius) >> 4, cz1 = (z + radius) >> 4;

    int cap = 8192;
    Cactus *buf = malloc(cap * sizeof *buf);
    int n = cactusRegion(&g, mc, gflags, seed, cx0, cz0, cx1, cz1, buf, cap);
    if (n < 0) {
        fprintf(stderr, "no block-level terrain for this version (needs 1.18+)\n");
        return 2;
    }
    if (n > cap) n = cap;
    qsort(buf, n, sizeof *buf, bycol);
    for (int i = 0; i < n; i++)
        printf("CACTUS %d %d %d %d\n", buf[i].x, buf[i].z, buf[i].baseY, buf[i].height);
    printf("# %d cactus columns in chunks [%d..%d] x [%d..%d]\n", n, cx0, cx1, cz0, cz1);
    free(buf);
    return 0;
}
