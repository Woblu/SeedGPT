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
    fprintf(stderr, "%" PRIu64 " %s within %d of (%d,%d)\n", n, argv[3], radius, cx, cz);
    return n ? 0 : 1;
}
