// checkcluster -- count viable instances of a structure within a radius of a
// point, from scratch, for one seed. Independent of the search path: it re-inits
// the generator, re-derives every structure position in range (getStructurePos),
// and biome-viability checks each (isViableStructurePos), exactly as the game's
// placement does. Used to confirm a cluster result ("N villages within R of
// spawn") reproduces rather than being trusted.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Mirror of query.c's structure vocabulary (names -> cubiomes ids).
static int name2struct(const char *s)
{
    struct { const char *n; int t; } tbl[] = {
        {"mansion", Mansion}, {"village", Village}, {"monument", Monument},
        {"desert_pyramid", Desert_Pyramid}, {"jungle_temple", Jungle_Pyramid},
        {"swamp_hut", Swamp_Hut}, {"igloo", Igloo}, {"shipwreck", Shipwreck},
        {"outpost", Outpost}, {"ancient_city", Ancient_City},
        {"ruined_portal", Ruined_Portal}, {"trail_ruins", Trail_Ruins},
        {"trial_chambers", Trial_Chambers}, {"treasure", Treasure},
        {"ocean_ruin", Ocean_Ruin}, {"fortress", Fortress}, {"bastion", Bastion},
        {"ruined_portal_nether", Ruined_Portal_N}, {"end_city", End_City},
    };
    for (int i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++)
        if (!strcmp(tbl[i].n, s)) return tbl[i].t;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: checkcluster <seed> <structure> <version> <x> <z> [radius]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int st = name2struct(argv[2]);
    if (st < 0) { fprintf(stderr, "unknown structure \"%s\"\n", argv[2]); return 2; }
    int mc = str2mc(argv[3]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int x = atoi(argv[4]), z = atoi(argv[5]);
    int radius = (argc > 6) ? atoi(argv[6]) : 800;

    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) { fprintf(stderr, "structure not in version\n"); return 2; }

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, sc.dim, seed);

    double span = sc.regionSize * 16.0;
    int r0x = (int)floor((x - radius) / span), r1x = (int)floor((x + radius) / span);
    int r0z = (int)floor((z - radius) / span), r1z = (int)floor((z + radius) / span);
    int64_t lim = (int64_t)radius * radius;
    int cnt = 0;
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos p;
        if (!getStructurePos(st, mc, seed & ((1ULL<<48)-1), rx, rz, &p)) continue;
        int64_t dx = p.x - x, dz = p.z - z;
        if (dx*dx + dz*dz > lim) continue;
        if (!isViableStructurePos(st, &g, p.x, p.z, 0)) continue;
        cnt++;
    }
    printf("%lld %s x=%d z=%d r=%d count=%d\n",
           (long long)seed, argv[2], x, z, radius, cnt);
    return 0;
}
