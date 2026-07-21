// checkportal -- re-derive a ruined portal's buried/surface variant from
// scratch for one seed, independent of the search path.
//
// The finder's "surface" filter rejects the buried (underground) variant using
// getVariant().underground. This tool re-locates the nearest ruined portal and
// re-reads that flag WITHOUT trusting anything find.exe cached -- and, as a
// second check, recomputes the underground bit straight from the chunk RNG so
// getVariant itself is cross-checked, not merely re-called.
//
// The buried decision is exact: for a plains/mountain-category portal the game
// draws one nextFloat from chunkGenerateRnd(seed, cx, cz) and buries it if that
// is < 0.5. Desert/jungle/swamp/ocean/nether portals are never buried.
#include "finders.h"
#include "generator.h"
#include "biomes.h"
#include "rng.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Reimplement the underground draw from first principles (see finders.c
// getVariant, Ruined_Portal case): category -> plains/mountains -> nextFloat.
static int rederive_underground(int mc, uint64_t seed, int x, int z, int biomeID)
{
    int cat = getCategory(mc, biomeID);
    int biome = -1;
    switch (cat) {
    case desert: case jungle: case swamp: case ocean: case nether_wastes:
        biome = cat; break;
    }
    if (biome == -1) {
        switch (biomeID) {
        case mangrove_swamp:
            biome = swamp; break;
        case mountains: case mountain_edge: case wooded_mountains:
        case gravelly_mountains: case modified_gravelly_mountains:
        case savanna_plateau: case shattered_savanna:
        case shattered_savanna_plateau: case badlands: case eroded_badlands:
        case wooded_badlands_plateau: case modified_badlands_plateau:
        case modified_wooded_badlands_plateau: case snowy_taiga_mountains:
        case taiga_mountains: case stony_shore: case meadow:
        case frozen_peaks: case jagged_peaks: case stony_peaks: case snowy_slopes:
            biome = mountains; break;
        }
    }
    if (biome == -1) biome = plains;
    if (biome != plains && biome != mountains)
        return 0;                       // only these two draw the buried coin
    uint64_t rng = chunkGenerateRnd(seed, x >> 4, z >> 4);
    return nextFloat(&rng) < 0.5f;      // the exact decision Minecraft makes
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: checkportal <seed> [version] [radius]\n"
                        "       checkportal <seed> <version> --at <x> <z>\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    // --at <x> <z>: verify one specific portal (the exact position the finder
    // matched), rather than searching for the nearest.
    if (argc >= 6 && !strcmp(argv[3], "--at")) {
        Pos best = { atoi(argv[4]), atoi(argv[5]) };
        int biomeID = getBiomeAt(&g, 0, (best.x>>4)*4+2, 319>>2, (best.z>>4)*4+2);
        StructureVariant sv;
        getVariant(&sv, Ruined_Portal, mc, seed, best.x, best.z, biomeID);
        int reref = rederive_underground(mc, seed, best.x, best.z, biomeID);
        printf("%lld ruined_portal x=%d z=%d biome=%s underground=%d airpocket=%d giant=%d "
               "(reref=%d %s)\n",
               (long long)seed, best.x, best.z, biome2str(mc, biomeID),
               sv.underground, sv.airpocket, sv.giant, reref,
               sv.underground == reref ? "AGREE" : "MISMATCH");
        return sv.underground == reref ? 0 : 3;
    }

    int radius = (argc > 3) ? atoi(argv[3]) : 2000;
    StructureConfig sc;
    if (!getStructureConfig(Ruined_Portal, mc, &sc)) {
        fprintf(stderr, "ruined_portal not in this version\n"); return 2;
    }
    double span = sc.regionSize * 16.0;
    int r0 = (int)floor((-radius) / span), r1 = (int)floor((radius) / span);

    Pos best; int64_t bestd = -1; int found = 0;
    for (int rx = r0; rx <= r1; rx++)
    for (int rz = r0; rz <= r1; rz++) {
        Pos p;
        if (!getStructurePos(Ruined_Portal, mc, seed, rx, rz, &p)) continue;
        if ((int64_t)p.x*p.x + (int64_t)p.z*p.z > (int64_t)radius*radius) continue;
        if (!isViableStructurePos(Ruined_Portal, &g, p.x, p.z, 0)) continue;
        int64_t d = (int64_t)p.x*p.x + (int64_t)p.z*p.z;
        if (bestd < 0 || d < bestd) { bestd = d; best = p; found = 1; }
    }
    if (!found) { printf("%lld no ruined_portal within %d\n", (long long)seed, radius); return 1; }

    int biomeID = getBiomeAt(&g, 0, (best.x>>4)*4+2, 319>>2, (best.z>>4)*4+2);
    StructureVariant sv;
    getVariant(&sv, Ruined_Portal, mc, seed, best.x, best.z, biomeID);
    int reref = rederive_underground(mc, seed, best.x, best.z, biomeID);

    printf("%lld ruined_portal x=%d z=%d biome=%s underground=%d airpocket=%d giant=%d "
           "(reref=%d %s)\n",
           (long long)seed, best.x, best.z, biome2str(mc, biomeID),
           sv.underground, sv.airpocket, sv.giant, reref,
           sv.underground == reref ? "AGREE" : "MISMATCH");
    return sv.underground == reref ? 0 : 3;   // 3 = getVariant disagrees with re-derivation
}
