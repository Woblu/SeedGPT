// describe -- given a seed, print what's actually in that world near spawn.
//
//   usage: describe <seed> [version] [radius_blocks]
//
// Turns a bare seed number into a readable picture: where spawn is and what
// biome it's in, then every searchable structure within `radius`, with the
// biome each one sits in. Pure forward generation (cubiomes) -- no search.
#include "query.h"        // structure vocabulary accessors
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

// Surface biome the way isViableStructurePos samples it on 1.18+ (scale 0,
// quart-y near build height) -- a y=63 probe lands in cave biomes.
static int surfaceBiome(Generator *g, int bx, int bz)
{
    return getBiomeAt(g, 0, (bx >> 4) * 4 + 2, 319 >> 2, (bz >> 4) * 4 + 2);
}

typedef struct { const char *name; Pos pos; int dist; int biome; char note[40]; } Found;

static int by_dist(const void *a, const void *b)
{
    return ((const Found*)a)->dist - ((const Found*)b)->dist;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: describe <seed> [version] [radius]\n"); return 2; }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version %s\n", argv[2]); return 2; }
    int radius = (argc > 3) ? atoi(argv[3]) : 2000;

    Generator g;
    setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    Pos spawn = getSpawn(&g);
    printf("seed %" PRId64 "   (MC %s)\n", (int64_t)seed, mc2str(mc));
    printf("  structure seed (low 48) : %" PRIu64 "\n", seed & ((1ULL << 48) - 1));
    printf("  world spawn : x=%d z=%d   biome=%s\n\n",
           spawn.x, spawn.z, biome2str(mc, surfaceBiome(&g, spawn.x, spawn.z)));

    Found found[512];
    int nf = 0;
    int64_t r2 = (int64_t)radius * radius;

    for (int i = 0; i < queryStructureCount() && nf < 512; i++) {
        int type = queryStructureType(i);
        StructureConfig sc;
        if (!getStructureConfig(type, mc, &sc)) continue;   // not in this version
        if (sc.dim != DIM_OVERWORLD) continue;              // skip nether/end here

        double span = sc.regionSize * 16.0;
        int rr = (int)(radius / span) + 1;
        for (int rx = -rr; rx <= rr && nf < 512; rx++)
        for (int rz = -rr; rz <= rr && nf < 512; rz++) {
            Pos p;
            if (!getStructurePos(type, mc, seed, rx, rz, &p)) continue;
            int64_t dx = (int64_t)p.x - spawn.x, dz = (int64_t)p.z - spawn.z;
            int64_t d2 = dx*dx + dz*dz;
            if (d2 > r2) continue;
            if (!isViableStructurePos(type, &g, p.x, p.z, 0)) continue;   // filter to real ones
            found[nf].name = queryStructureName(i);
            found[nf].pos = p;
            found[nf].dist = (int)sqrt((double)d2);
            found[nf].biome = surfaceBiome(&g, p.x, p.z);
            found[nf].note[0] = 0;
            // Ruined portals are the one structure worth annotating: about half
            // of those in plains/mountain biomes generate UNDERGROUND, which is
            // why a reported portal can look absent at the surface.
            if (type == Ruined_Portal) {
                StructureVariant sv;
                if (getVariant(&sv, Ruined_Portal, mc, seed, p.x, p.z,
                               found[nf].biome) > 0) {
                    snprintf(found[nf].note, sizeof found[nf].note, "%s%s%s",
                             sv.underground ? "BURIED" : "surface",
                             sv.giant ? ", giant" : "",
                             sv.airpocket ? ", air pocket" : "");
                }
            }
            nf++;
        }
    }

    qsort(found, nf, sizeof(Found), by_dist);

    // Show at most PER_TYPE nearest of each structure, so a dense structure
    // (ruined portals, trial chambers) doesn't bury a rare one (mansion).
    #define PER_TYPE 3
    printf("  structures within %d blocks of spawn (%d found; nearest %d of each):\n",
           radius, nf, PER_TYPE);
    if (nf == 0) printf("    (none -- try a larger radius)\n");

    for (int i = 0; i < queryStructureCount(); i++) {
        const char *name = queryStructureName(i);
        int shown = 0;
        for (int j = 0; j < nf && shown < PER_TYPE; j++) {
            if (found[j].name != name) continue;   // same interned pointer
            printf("    %-15s x=%6d z=%6d  %5d away   %-22s %s\n",
                   found[j].name, found[j].pos.x, found[j].pos.z, found[j].dist,
                   biome2str(mc, found[j].biome), found[j].note);
            shown++;
        }
        // count how many more of this type exist beyond what we showed
        int total = 0;
        for (int j = 0; j < nf; j++) if (found[j].name == name) total++;
        if (total > PER_TYPE)
            printf("    %-15s   ... and %d more\n", "", total - PER_TYPE);
    }
    return 0;
}
