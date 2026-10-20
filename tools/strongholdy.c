// strongholdy -- how DEEP is each stronghold, and does it break the surface.
//
//   strongholdy <seed> <version> [radius] [--pieces]
//
// Every other structure in this project is an x/z question. A stronghold is not:
// "surface exposed stronghold" and "stronghold inside a mansion" are both
// questions about the structure's Y against the terrain around it, and until
// now nothing here could answer them -- locate printed x and z and stopped.
//
// cubiomes CAN produce the real Y; upstream just leaves it off because loot and
// eye counts do not depend on it (see getStrongholdPiecesOpt). This turns it on.
//
// maxY is the top of the highest piece's bounding box, which is the stronghold's
// roof. Compare it against the surface to get depth. A stronghold whose roof is
// at or above the surface is exposed.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "biomenoise.h"
#include "features/stronghold.h"
#include "../src/terrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define MAXP 2048

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: strongholdy <seed> <version> [radius] [--pieces] [--sea]\n"
                        "       --sea  count pieces breaking out under water too\n"
                        "              (default is dry land only, as the search is)\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int radius = (argc > 3 && argv[3][0] != '-') ? atoi(argv[3]) : 3000;
    int showPieces = 0;
    // Dry land by default, matching the search: a stronghold breaking through a
    // river bed is true by the arithmetic and not the find anyone means.
    int landOnly = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--pieces")) showPieces = 1;
        if (!strcmp(argv[i], "--sea"))    landOnly = 0;
    }

    Generator g;
    setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    Piece *pieces = (Piece *)malloc(sizeof(Piece) * MAXP);
    Piece *pieces2 = (Piece *)malloc(sizeof(Piece) * MAXP);
    if (!pieces || !pieces2) return 3;

    StrongholdIter sh;
    initFirstStronghold(&sh, mc, seed);
    int64_t lim2 = (int64_t)radius * radius;
    int n = 0;

    while (nextStronghold(&sh, &g) > 0) {
        int64_t dx = sh.pos.x, dz = sh.pos.z;
        // sh.dist is the NEXT stronghold's distance in chunks, sh.pos the
        // CURRENT one's. Rings march outward, so once the next is comfortably
        // past the radius nothing later can fall back inside it -- but the
        // current one must be tested first. Breaking before testing it threw
        // away an in-range stronghold whenever the next ring cleared the cutoff
        // (the bug the engine fixed in its own ring walk).
        int pastLast = (sh.dist * 16.0 > (double)radius * 2.0 + 2048.0);
        if (dx * dx + dz * dz > lim2) {
            if (pastLast) break;
            continue;
        }
        int cnt = getStrongholdPiecesOpt(pieces, MAXP, mc, seed,
                                         sh.pos.x >> 4, sh.pos.z >> 4, 1);
        if (cnt <= 0) continue;
        // Accurate Y requires draining the whole piece queue, which consumes RNG
        // the loot path never spends. If that changes the piece LIST and not
        // just its height, the two paths are describing different structures and
        // the Y cannot be trusted -- so say when they diverge instead of leaving
        // it to be discovered against a server.
        int cnt0 = getStrongholdPieces(pieces2, MAXP, mc, seed,
                                       sh.pos.x >> 4, sh.pos.z >> 4);
        int minY = pieces[0].bb0.y, maxY = pieces[0].bb1.y;
        int px = 0, pz = 0, py = -9999, hasPortal = 0;
        for (int i = 0; i < cnt; i++) {
            if (pieces[i].bb0.y < minY) minY = pieces[i].bb0.y;
            if (pieces[i].bb1.y > maxY) maxY = pieces[i].bb1.y;
            if (pieces[i].type == SH_PORTAL_ROOM) {
                px = pieces[i].pos.x; pz = pieces[i].pos.z;
                py = pieces[i].bb0.y; hasPortal = 1;
            }
        }

        // Terrain height over the stronghold, so "exposed" is a comparison and
        // not a guess.
        //
        // PER PIECE, against the ground over THAT piece -- which is what
        // src/query.c has always done and what this tool did not. Measuring the
        // global maxY against the surface at the START chunk is apples to
        // oranges: a stronghold sprawls 112 blocks, so the tall piece and the
        // start can be a hundred blocks and forty metres of hillside apart. The
        // old framing disagreed with the engine on real seeds -- it called
        // 5919784748248469320 "depth=5" where the search correctly found a
        // piece breaking out -- which makes it useless as a check on the
        // search, the one job it has.
        //
        // Block-exact terrain only exists from 1.18 here -- and 1.18 is exactly
        // where strongholds stopped coming near the surface -- so the versions
        // that matter for this question are the ones without it. mapApproxHeight
        // covers them: it is the same estimator the height condition uses, and
        // it is an ESTIMATE, reported as one. It is also SMOOTHED at 1:4, so it
        // reads a few blocks high over exactly the sharp dip that exposes a
        // stronghold -- treat a near-zero depth as a candidate, not a verdict.
        int ok = 0, anyExact = 0;
        int bestDepth = 0x7fffffff, bestSurf = 0, bestRoof = 0;
        Pos bestAt = sh.pos;
        for (int i = 0; i < cnt; i++) {
            int cx = (pieces[i].bb0.x + pieces[i].bb1.x) >> 1;
            int cz = (pieces[i].bb0.z + pieces[i].bb1.z) >> 1;
            int pok = 0;
            int psurf = terrainSurfaceY(mc, seed, 0, cx, cz, &pok);
            if (!pok) {
                float fy = 0;
                mapApproxHeight(&fy, NULL, &g, &sn, cx >> 2, cz >> 2, 1, 1);
                psurf = (int)fy;
            } else anyExact = 1;
            if (landOnly && psurf < 62) continue;   // sea/river floor, not a find
            int d = psurf - pieces[i].bb1.y;
            if (d < bestDepth) {
                bestDepth = d; bestSurf = psurf;
                bestRoof = pieces[i].bb1.y; bestAt = (Pos){cx, cz};
            }
        }
        ok = (bestDepth != 0x7fffffff);

        printf("%d %d pieces=%d minY=%d maxY=%d", sh.pos.x, sh.pos.z, cnt, minY, maxY);
        if (cnt != cnt0) printf(" DIVERGES(loot-path pieces=%d)", cnt0);
        if (hasPortal) printf(" portal=%d,%d,%d", px, py, pz);
        if (ok) {
            printf(" shallowest=%d,%d roof=%d surface=%d%s depth=%d%s",
                   bestAt.x, bestAt.z, bestRoof, bestSurf, anyExact ? "" : "~",
                   bestDepth,
                   bestDepth <= 0 ? " EXPOSED" : (bestDepth <= 4 ? " SHALLOW" : ""));
        } else {
            printf(" surface=? (every piece over water)");
        }
        printf("\n");

        if (showPieces) {
            for (int i = 0; i < cnt; i++)
                printf("   %-4s y %d..%d  x %d..%d  z %d..%d\n",
                       pieces[i].name ? pieces[i].name : "?",
                       pieces[i].bb0.y, pieces[i].bb1.y,
                       pieces[i].bb0.x, pieces[i].bb1.x,
                       pieces[i].bb0.z, pieces[i].bb1.z);
        }
        n++;
    }
    free(pieces);
    fprintf(stderr, "%d stronghold(s) within %d\n", n, radius);
    return n ? 0 : 1;
}
