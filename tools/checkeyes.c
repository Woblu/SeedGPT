// checkeyes -- recompute an end portal's eye count from scratch for one seed.
// Deliberately independent of the search path: it re-locates the stronghold
// and re-reads the portal room rather than trusting anything find.exe cached.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "features/stronghold.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: checkeyes <seed> [version]\n"); return 2; }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    StrongholdIter sh;
    initFirstStronghold(&sh, mc, seed);
    if (!nextStronghold(&sh, &g)) { printf("%lld no stronghold\n", (long long)seed); return 1; }

    StructureSaltConfig ss;
    if (!getStructureSaltConfig(Stronghold, mc, -1, &ss)) return 1;

    Piece pieces[512];
    int n = getStrongholdLoot(pieces, 512, ss, mc, seed, sh.pos.x >> 4, sh.pos.z >> 4);
    for (int i = 0; i < n; i++) {
        if (pieces[i].type != SH_PORTAL_ROOM) continue;
        int bits = pieces[i].additionalData, eyes = 0;
        for (int b = 0; b < 12; b++) if (bits & (1 << b)) eyes++;
        printf("%lld stronghold x=%d z=%d eyes=%d frames=0b",
               (long long)seed, sh.pos.x, sh.pos.z, eyes);
        for (int b = 11; b >= 0; b--) putchar((bits >> b) & 1 ? '1' : '0');
        putchar('\n');
        return 0;
    }
    printf("%lld no portal room\n", (long long)seed);
    return 1;
}
