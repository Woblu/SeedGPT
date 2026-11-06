#include "biomeat.h"
// describe -- given a seed, print what's actually in that world near spawn.
//
//   usage: describe <seed> [version] [radius_blocks] [large] [json]
//
// Turns a bare seed number into a readable picture: where spawn is and what
// biome it's in, then every searchable structure within `radius`, with the
// biome each one sits in. Pure forward generation (cubiomes) -- no search.
// "json" prints the same as one JSON object, for the web UI.
#include "query.h"        // structure vocabulary accessors
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "seed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

// Surface biome the way isViableStructurePos samples it on 1.18+ (scale 0,
// quart-y near build height) -- a y=63 probe lands in cave biomes.
static int surfaceBiome(Generator *g, int bx, int bz)
{
    return chunkBiome(g, bx, bz);
}

typedef struct { const char *name; Pos pos; int dist; int biome; char note[40]; } Found;

static int by_dist(const void *a, const void *b)
{
    const Found *x = a, *y = b;
    if (x->dist != y->dist) return x->dist < y->dist ? -1 : 1;
    // Equal whole-block distances are common, and qsort leaves their order to
    // the C library, so the same seed listed differently on Windows and Linux.
    int n = strcmp(x->name, y->name);
    if (n) return n;
    if (x->pos.x != y->pos.x) return x->pos.x < y->pos.x ? -1 : 1;
    return (x->pos.z > y->pos.z) - (x->pos.z < y->pos.z);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: describe <seed> [version] [radius] [large] [json]\n"); return 2; }
    uint64_t seed;
    if (!parseWorldSeed(argv[1], &seed)) { fprintf(stderr, "no seed given\n"); return 2; }
    int mc = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc <= MC_UNDEF) { fprintf(stderr, "unknown version %s\n", argv[2]); return 2; }
    int radius = (argc > 3) ? atoi(argv[3]) : 2000;
    int large = 0, json = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "large")) large = 1;
        if (!strcmp(argv[i], "json"))  json = 1;
    }

    Generator g;
    setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    Pos spawn = getSpawn(&g);
    int spawnBiome = surfaceBiome(&g, spawn.x, spawn.z);

    // Every structure in range, however many. This used to stop at a fixed 512
    // across ALL types, filled in vocabulary order -- so at a large radius the
    // dense structures (shipwrecks, ruined portals, ocean ruins) used the whole
    // buffer and any type listed after them was silently missing, which is
    // exactly the rare one (an ancient city) people describe a seed to find.
    int cap = 1024, nf = 0;
    Found *found = malloc(sizeof(Found) * cap);
    if (!found) { fprintf(stderr, "out of memory\n"); return 1; }
    int64_t r2 = (int64_t)radius * radius;

    unsigned char seen[256] = {0};
    for (int i = 0; i < queryStructureCount(); i++) {
        int type = queryStructureType(i);
        // The vocabulary has aliases ("geode"/"amethyst_geode",
        // "treasure"/"buried_treasure"): list each structure once.
        if (type < 0 || type >= 256 || seen[type]) continue;
        seen[type] = 1;
        // Geodes are a per-chunk decoration feature -- thousands in any radius
        // worth describing -- not a landmark; they would drown the list.
        if (type == Geode) continue;
        StructureConfig sc;
        if (!getStructureConfig(type, mc, &sc)) continue;   // not in this version
        if (sc.dim != DIM_OVERWORLD) continue;              // skip nether/end here

        double span = sc.regionSize * 16.0;
        int rx0 = (int)floor((spawn.x - radius) / span), rx1 = (int)floor((spawn.x + radius) / span);
        int rz0 = (int)floor((spawn.z - radius) / span), rz1 = (int)floor((spawn.z + radius) / span);
        for (int rx = rx0; rx <= rx1; rx++)
        for (int rz = rz0; rz <= rz1; rz++) {
            Pos p;
            if (!getStructurePos(type, mc, seed, rx, rz, &p)) continue;
            int64_t dx = (int64_t)p.x - spawn.x, dz = (int64_t)p.z - spawn.z;
            int64_t d2 = dx*dx + dz*dz;
            if (d2 > r2) continue;
            // The finder's own gate (biome, exclusion zones, 1.18 terrain floor),
            // so describe never lists a structure a search would rule out.
            if (!structureGenerates(mc, large ? LARGE_BIOMES : 0, seed, &g, type, p)) continue;
            if (nf == cap) {
                Found *more = realloc(found, sizeof(Found) * cap * 2);
                if (!more) break;
                found = more; cap *= 2;
            }
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

    if (json) {
        // Names are cubiomes identifiers ([a-z_]), so no escaping is needed.
        printf("{\"seed\":\"%" PRId64 "\",\"version\":\"%s\",\"structure_seed\":\"%" PRIu64 "\",",
               (int64_t)seed, mc2str(mc), (uint64_t)(seed & ((1ULL << 48) - 1)));
        printf("\"spawn\":{\"x\":%d,\"z\":%d,\"biome\":\"%s\"},\"radius\":%d,\"structures\":[",
               spawn.x, spawn.z, biome2str(mc, spawnBiome), radius);
        for (int j = 0; j < nf; j++) {
            const char *bn = biome2str(mc, found[j].biome);
            printf("%s{\"type\":\"%s\",\"x\":%d,\"z\":%d,\"dist\":%d,\"biome\":\"%s\"",
                   j ? "," : "", found[j].name, found[j].pos.x, found[j].pos.z,
                   found[j].dist, bn ? bn : "?");
            if (found[j].note[0]) printf(",\"note\":\"%s\"", found[j].note);
            printf("}");
        }
        printf("]}\n");
        free(found);
        return 0;
    }

    printf("seed %" PRId64 "   (MC %s)\n", (int64_t)seed, mc2str(mc));
    printf("  structure seed (low 48) : %" PRIu64 "\n", (uint64_t)(seed & ((1ULL << 48) - 1)));
    printf("  world spawn : x=%d z=%d   biome=%s\n\n",
           spawn.x, spawn.z, biome2str(mc, spawnBiome));

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
    free(found);
    return 0;
}
