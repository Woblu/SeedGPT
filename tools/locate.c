// locate -- list every instance of a structure in a radius, one per line.
//
//   locate <seed> <version> <structure> <radius> [centreX] [centreZ] [--all]
//
// describe.exe prints the nearest few of each type, which is what you want when
// you are reading it. This prints ALL of them in a machine-readable form,
// because the tier-3 backends need to walk a list: "probe every buried treasure
// in this seed and tell me what is around it" starts here.
//
// Positions are viability-checked by default (the structure really generates),
// since an unviable position has no blocks to probe and would waste a server
// round-trip. --all skips that and prints raw placements.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: locate <seed> <version> <structure> <radius> "
                        "[centreX] [centreZ] [--all]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    int st = -1;
    for (int i = 0; i < 64 && st < 0; i++) {
        const char *n = struct2str(i);
        if (n && !strcmp(n, argv[3])) st = i;
    }
    // Accept the names the QUERY engine accepts, not only cubiomes' own. The
    // query language says "outpost" and cubiomes says "pillager_outpost", so
    // `locate <seed> 1.21 outpost` printed nothing at all -- and a caller that
    // redirected stderr read that silence as "this seed has no outposts". It
    // does not: this exact mismatch produced a confident report that a real
    // outpost did not exist.
    if (st < 0) {
        static const struct { const char *alias, *real; } kAlias[] = {
            {"outpost",  "pillager_outpost"},
            {"treasure", "buried_treasure"},
            {"pyramid",  "desert_pyramid"},
            {"temple",   "jungle_temple"},
            {"hut",      "swamp_hut"},
            {"geode",    "amethyst_geode"},
            {"portal",   "ruined_portal"},
            // cubiomes calls it bastion_remnant; every other tool here, the
            // query engine and the vocabulary all say "bastion". Without this
            // alias locate was the one component that answered "unknown
            // structure" for a structure the rest of the project supports.
            {"bastion",  "bastion_remnant"},
            // Same story: the project says jungle_temple everywhere, cubiomes
            // says jungle_pyramid, and locate was reporting "unknown" for a
            // structure every other component handles.
            {"jungle_temple", "jungle_pyramid"},
        };
        for (size_t a = 0; a < sizeof kAlias / sizeof *kAlias && st < 0; a++) {
            if (strcmp(kAlias[a].alias, argv[3])) continue;
            for (int i = 0; i < 64 && st < 0; i++) {
                const char *n = struct2str(i);
                if (n && !strcmp(n, kAlias[a].real)) st = i;
            }
        }
    }
    if (st < 0) {
        // Printed on STDOUT too, so suppressing stderr cannot turn "I do not
        // know that name" into "there are none of those here".
        printf("ERROR unknown structure \"%s\"\n", argv[3]);
        fprintf(stderr, "unknown structure \"%s\"\n", argv[3]);
        return 2;
    }

    int radius = atoi(argv[4]);

    int cx = (argc > 5 && argv[5][0] != '-') ? atoi(argv[5]) : 0;
    int cz = (argc > 6 && argv[6][0] != '-') ? atoi(argv[6]) : 0;
    int skipViable = 0;
    for (int i = 5; i < argc; i++) if (!strcmp(argv[i], "--all")) skipViable = 1;

    // STRONGHOLDS are not placed like other structures and cubiomes does not
    // expose them through getStructurePos, so locate answered "does not exist in
    // 1.21" for the structure players ask about most -- the one with the End
    // portal in it. The engine has had them all along: initFirstStronghold and
    // nextStronghold drive the `eyes` condition, and the cost model already
    // rates a stronghold at ~10ms against ~1ns for a village, because the game
    // samples many biomes to place one.
    //
    // They are generated in RINGS from the origin, in order, so this walks the
    // iterator rather than scanning a region grid.
    if (st == Stronghold) {
        Generator gs;
        setupGenerator(&gs, mc, 0);
        applySeed(&gs, DIM_OVERWORLD, seed);
        StrongholdIter sh;
        initFirstStronghold(&sh, mc, seed);
        uint64_t nsh = 0;
        int64_t lim2 = (int64_t)radius * radius;
        while (nextStronghold(&sh, &gs) > 0) {
            int64_t dx = sh.pos.x - cx, dz = sh.pos.z - cz;
            if (dx * dx + dz * dz <= lim2) {
                printf("%d %d\n", sh.pos.x, sh.pos.z);
                nsh++;
            }
            // The rings march outward, so once the ring itself is past the
            // radius nothing later can come back inside it.
            // sh.dist is the next ring distance in CHUNKS. Once the ring
            // itself is well past the radius, nothing later in it can come
            // back inside, so the walk can stop instead of running forever.
            // Rings are centred on the ORIGIN, so the cutoff has to clear the
            // search centre's own distance too, or a search far from 0,0
            // stops at the inner rings before reaching any near it.
            if (sh.dist * 16.0 > sqrt((double)cx * cx + (double)cz * cz)
                                 + (double)radius * 2.0 + 2048.0)
                break;
        }
        fprintf(stderr, "%" PRIu64 " stronghold within %d of (%d,%d) -- "
                        "overworld coordinates\n", nsh, radius, cx, cz);
        return nsh ? 0 : 1;
    }

    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) {
        fprintf(stderr, "%s does not exist in %s\n", argv[3], argv[2]);
        return 2;
    }

    Generator g;
    setupGenerator(&g, mc, 0);
    applySeed(&g, sc.dim, seed);

    double span = sc.regionSize * 16.0;
    int r0x = (int)floor((cx - radius) / span), r1x = (int)floor((cx + radius) / span);
    int r0z = (int)floor((cz - radius) / span), r1z = (int)floor((cz + radius) / span);
    int64_t lim = (int64_t)radius * radius;

    uint64_t n = 0;
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos p;
        if (!getStructurePos(st, mc, seed & ((1ULL << 48) - 1), rx, rz, &p)) continue;
        int64_t dx = p.x - cx, dz = p.z - cz;
        if (dx*dx + dz*dz > lim) continue;
        if (!skipViable && !isViableStructurePos(st, &g, p.x, p.z, 0)) continue;
        printf("%d %d\n", p.x, p.z);
        n++;
    }
    // WHICH DIMENSION THESE COORDINATES ARE IN.
    //
    // ruined_portal and ruined_portal_nether print identical numbers from 1.18
    // -- both are {salt 34222645, spread 40, sep 25} and differ only in
    // dimension -- so an unlabelled list of nether coordinates is
    // indistinguishable from an overworld one. Standing at those overworld
    // coordinates and finding nothing is exactly what "the finder hallucinates
    // ruined portals" looks like.
    //
    // Nether coordinates are also 1/8 scale: a nether structure at (x,z) sits
    // near overworld (8x, 8z), so the number is not just in the wrong place, it
    // is in the wrong units for anyone who assumes overworld.
    const char *dimname = sc.dim == DIM_NETHER ? "NETHER"
                        : sc.dim == DIM_END    ? "END" : "overworld";
    fprintf(stderr, "%" PRIu64 " %s within %d of (%d,%d) -- %s coordinates\n",
            n, argv[3], radius, cx, cz, dimname);
    if (sc.dim != DIM_OVERWORLD) {
        // On STDOUT as well: a caller that reads only stdout must not be able to
        // mistake these for overworld positions.
        printf("# %s COORDINATES", dimname);
        if (sc.dim == DIM_NETHER)
            printf(" -- divide by 8 for the overworld point above them");
        printf("\n");
        fprintf(stderr, "  these are %s coordinates, not overworld ones\n", dimname);
    }
    return n ? 0 : 1;
}
