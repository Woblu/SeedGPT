// Cross-validation harness: dump cubiomes' structure configs and positions
// in a diffable format, to compare against an independent JDK implementation.
#include "finders.h"
#include <stdio.h>

static const int64_t SEEDS[] = {
    0, 1, 42, -1, 123456789, 8675309, -4172144997902289642LL,
    3257840388504953787LL, 2151901553968352745LL, -4643277814275016011LL,
};
#define NSEEDS (sizeof(SEEDS)/sizeof(SEEDS[0]))

typedef struct { const char *name; int type; int mc; } Target;

int main(void)
{
    Target targets[] = {
        {"desert_pyramid", Desert_Pyramid, MC_1_21},
        {"village",        Village,        MC_1_21},
        {"village_262",    Village,        MC_26_2},
        {"mansion",        Mansion,        MC_1_21},   // triangular spread
        {"monument",       Monument,       MC_1_21},   // triangular spread
        {"ancient_city",   Ancient_City,   MC_1_21},
    };

    // Section 1: configs, so we can check them against the wiki independently.
    for (size_t t = 0; t < sizeof(targets)/sizeof(targets[0]); t++)
    {
        StructureConfig sc;
        if (!getStructureConfig(targets[t].type, targets[t].mc, &sc)) {
            printf("CONFIG %s UNSUPPORTED\n", targets[t].name);
            continue;
        }
        printf("CONFIG %s salt=%d regionSize=%d chunkRange=%d\n",
               targets[t].name, sc.salt, sc.regionSize, sc.chunkRange);
    }

    // Section 2: positions.
    for (size_t t = 0; t < sizeof(targets)/sizeof(targets[0]); t++)
    {
        StructureConfig sc;
        if (!getStructureConfig(targets[t].type, targets[t].mc, &sc))
            continue;
        for (size_t i = 0; i < NSEEDS; i++)
            for (int rx = -3; rx <= 3; rx++)
                for (int rz = -3; rz <= 3; rz++)
                {
                    Pos p;
                    if (!getStructurePos(targets[t].type, targets[t].mc,
                                         (uint64_t)SEEDS[i], rx, rz, &p))
                        continue;
                    printf("POS %s %lld %d %d %d %d\n",
                           targets[t].name, (long long)SEEDS[i], rx, rz, p.x, p.z);
                }
    }
    return 0;
}
