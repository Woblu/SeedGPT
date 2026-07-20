// confidence -- how much does the engine actually verify each structure?
//
// getStructurePos gives a generation ATTEMPT. isViableStructurePos is supposed
// to say whether a structure really generates there. For some structures that
// check is substantive (a biome test); for others it returns true
// unconditionally, so every attempt is reported as a hit and the engine cannot
// filter false positives at all.
//
// This measures the pass rate per structure. ~100% means "unverified": the
// engine is not modelling the game's placement rules for it, so a reported
// position may not exist in game.
#include "query.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int mc = (argc > 1) ? str2mc(argv[1]) : MC_1_21;
    int nseeds = (argc > 2) ? atoi(argv[2]) : 60;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    printf("MC %s -- isViableStructurePos pass rate over %d seeds\n\n",
           mc2str(mc), nseeds);
    printf("  %-22s %8s %8s   %s\n", "structure", "attempts", "pass", "verification");

    Generator g;
    setupGenerator(&g, mc, 0);

    for (int i = 0; i < queryStructureCount(); i++) {
        int type = queryStructureType(i);
        StructureConfig sc;
        if (!getStructureConfig(type, mc, &sc)) continue;
        if (sc.dim != DIM_OVERWORLD) continue;   // keep the comparison fair

        long attempts = 0, pass = 0;
        for (int s = 0; s < nseeds; s++) {
            applySeed(&g, DIM_OVERWORLD, (uint64_t)s * 7919 + 13);
            for (int rx = -2; rx <= 2; rx++)
            for (int rz = -2; rz <= 2; rz++) {
                Pos p;
                if (!getStructurePos(type, mc, (uint64_t)s * 7919 + 13, rx, rz, &p)) continue;
                attempts++;
                if (isViableStructurePos(type, &g, p.x, p.z, 0)) pass++;
            }
        }
        double rate = attempts ? 100.0 * pass / attempts : 0.0;
        const char *verdict = rate > 99.5 ? "NONE - every attempt reported"
                            : rate > 90.0 ? "weak"
                                          : "biome-checked";
        printf("  %-22s %8ld %7.1f%%   %s\n",
               queryStructureName(i), attempts, rate, verdict);
    }
    return 0;
}
