// checkvariant -- read a structure's variant flags (zombie village, igloo
// basement, giant/buried ruined portal) from scratch for one seed, independent
// of the search path. Confirms the finder's variant filters rather than
// trusting them. getVariant reads the exact RNG decision the game makes, so
// these flags are exact, not approximations.
#include "finders.h"
#include "generator.h"
#include "biomes.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int name2struct(const char *s)
{
    if (!strcmp(s, "village"))       return Village;
    if (!strcmp(s, "igloo"))         return Igloo;
    if (!strcmp(s, "ruined_portal")) return Ruined_Portal;
    if (!strcmp(s, "geode"))         return Geode;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: checkvariant <seed> <structure> [version] --at <x> <z>\n"
            "       structures with variants: village igloo ruined_portal geode\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int st = name2struct(argv[2]);
    if (st < 0) { fprintf(stderr, "no variants for \"%s\"\n", argv[2]); return 2; }
    int mc = (argc > 3) ? str2mc(argv[3]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    if (argc < 7 || strcmp(argv[4], "--at")) {
        fprintf(stderr, "expected: --at <x> <z>\n"); return 2;
    }
    int x = atoi(argv[5]), z = atoi(argv[6]);

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    int biomeID = getBiomeAt(&g, 0, (x>>4)*4+2, 319>>2, (z>>4)*4+2);
    StructureVariant sv;
    getVariant(&sv, st, mc, seed, x, z, biomeID);
    if (st == Geode) {
        // A geode's size is its distribution-point count (3 or 4) and "cracked"
        // is the 95% draw that opens it up; a sealed geode is the rare one.
        printf("%lld geode x=%d z=%d biome=%s size=%d cracked=%d\n",
               (long long)seed, x, z, biome2str(mc, biomeID), sv.size, sv.cracked);
        return 0;
    }
    printf("%lld %s x=%d z=%d biome=%s abandoned=%d basement=%d giant=%d underground=%d\n",
           (long long)seed, struct2str(st), x, z, biome2str(mc, biomeID),
           sv.abandoned, sv.basement, sv.giant, sv.underground);
    return 0;
}
