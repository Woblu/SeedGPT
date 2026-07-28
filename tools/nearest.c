// nearest -- the closest viable structure, or the closest point of a biome, to
// a coordinate. The Java-side counterpart of Bedrock's `/locate`, so the two
// engines can be asked the same question in the same units and diffed.
//
//   nearest <seed> <version> structure <name> [x] [z] [radius] [large]
//   nearest <seed> <version> biome     <name> [x] [z] [radius] [large]
//
// Structures are biome-viability checked (and surface-rule checked on 1.18+),
// so this reports what the game would actually place, not every attempt.
// Biomes are sampled on a 16-block lattice, which is the finder's "fine"
// precision: it can miss a biome pocket smaller than that, never invent one.
#include "query.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int str2biome_local(int mc, const char *s)
{
    for (int id = 0; id < 256; id++) {
        const char *n = biome2str(mc, id);
        if (n && !strcmp(n, s)) return id;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "usage: nearest <seed> <version> structure|biome <name> [x] [z] [radius] [large]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    const char *kind = argv[3], *name = argv[4];
    int x = (argc > 5) ? atoi(argv[5]) : 0;
    int z = (argc > 6) ? atoi(argv[6]) : 0;
    int radius = (argc > 7) ? atoi(argv[7]) : 4000;
    int large = 0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "large")) large = 1;

    Generator g;
    setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);

    if (!strcmp(kind, "biome")) {
        int id = str2biome_local(mc, name);
        if (id < 0) { fprintf(stderr, "unknown biome \"%s\"\n", name); return 2; }
        applySeed(&g, getDimension(id), seed);
        int64_t best = -1; int bx = 0, bz = 0;
        // Rings outward from the point, so the first hit at a given ring is
        // close to optimal without scanning the whole disc.
        for (int r = 0; r <= radius && best < 0; r += 16) {
            for (int dx = -r; dx <= r; dx += 16)
            for (int dz = -r; dz <= r; dz += 16) {
                if (r > 0 && abs(dx) != r && abs(dz) != r) continue;   // ring only
                int px = x + dx, pz = z + dz;
                if (getBiomeAt(&g, 0, (px>>4)*4 + 2, 319 >> 2, (pz>>4)*4 + 2) != id) continue;
                int64_t d = (int64_t)dx*dx + (int64_t)dz*dz;
                if (best < 0 || d < best) { best = d; bx = px; bz = pz; }
            }
        }
        if (best < 0) { printf("none within %d\n", radius); return 1; }
        printf("%lld %s x=%d z=%d dist=%d\n", (long long)seed, name, bx, bz,
               (int)sqrt((double)best));
        return 0;
    }

    int listing = !strcmp(kind, "list");
    if (strcmp(kind, "structure") && !listing) {
        fprintf(stderr, "kind must be structure|biome|list\n"); return 2;
    }

    int stype = -1;
    for (int i = 0; i < queryStructureCount(); i++)
        if (!strcmp(queryStructureName(i), name)) stype = queryStructureType(i);
    if (stype < 0) { fprintf(stderr, "unknown structure \"%s\"\n", name); return 2; }
    StructureConfig sc;
    if (!getStructureConfig(stype, mc, &sc)) {
        fprintf(stderr, "%s does not exist in %s\n", name, argv[2]); return 2;
    }
    applySeed(&g, sc.dim, seed);

    double span = sc.regionSize * 16.0;
    int r0x = (int)floor((x - radius) / span), r1x = (int)floor((x + radius) / span);
    int r0z = (int)floor((z - radius) / span), r1z = (int)floor((z + radius) / span);
    int64_t best = -1; Pos bp = {0,0};
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos p;
        if (!getStructurePos(stype, mc, seed, rx, rz, &p)) continue;
        int64_t dx = p.x - x, dz = p.z - z, d = dx*dx + dz*dz;
        if (d > (int64_t)radius * radius) continue;
        // "list" mode dumps every viable instance as "<seed> <chunkX> <chunkZ>",
        // which is the observation format the placement fitter reads. Being able
        // to produce the SAME format from Java is what lets the fitter be tested
        // against a known answer before it is trusted on Bedrock data.
        if (listing) {
            if (!isViableStructurePos(stype, &g, p.x, p.z, 0)) continue;
            // Chunk coordinates by FLOOR division. The editions report a
            // structure at different points inside its chunk -- Java at the
            // chunk corner, Bedrock at the centre (x,z == 8 mod 16) -- so
            // anything comparing them must divide, never subtract a guessed
            // offset, which silently shifts data into the next region.
            printf("%lld %d %d\n", (long long)seed, p.x >> 4, p.z >> 4);
            continue;
        }
        if (best >= 0 && d >= best) continue;
        if (!isViableStructurePos(stype, &g, p.x, p.z, 0)) continue;
        best = d; bp = p;
    }
    if (listing) return 0;
    if (best < 0) { printf("none within %d\n", radius); return 1; }
    printf("%lld %s x=%d z=%d dist=%d\n", (long long)seed, name, bp.x, bp.z,
           (int)sqrt((double)best));
    return 0;
}
