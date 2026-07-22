// checkslime -- count slime chunks in a disc around a point, from scratch, for
// one seed. Independent of the search path: it re-derives every chunk's slime
// status directly from the world seed via cubiomes' isSlimeChunk (a per-chunk
// Java-RNG check: seed + chunk-coord mixing -> setSeed -> nextInt(10)==0), so a
// finder result can be confirmed rather than trusted. Slime-chunk placement is
// version-independent (unchanged since the mechanic shipped), so no MC version
// argument is needed.
#include "finders.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: checkslime <seed> [x] [z] [radius]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int x = (argc > 2) ? atoi(argv[2]) : 0;
    int z = (argc > 3) ? atoi(argv[3]) : 0;
    int radius = (argc > 4) ? atoi(argv[4]) : 128;

    long long lim = (long long)radius * radius;
    int c0x = (x - radius) >> 4, c1x = (x + radius) >> 4;
    int c0z = (z - radius) >> 4, c1z = (z + radius) >> 4;
    int n = 0;
    for (int chx = c0x; chx <= c1x; chx++)
    for (int chz = c0z; chz <= c1z; chz++) {
        int bx = (chx << 4) + 8 - x, bz = (chz << 4) + 8 - z;
        if ((long long)bx*bx + (long long)bz*bz > lim) continue;
        if (isSlimeChunk(seed, chx, chz)) n++;
    }
    printf("%lld slime x=%d z=%d r=%d count=%d\n",
           (long long)seed, x, z, radius, n);
    return 0;
}
