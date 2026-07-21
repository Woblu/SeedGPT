// checkloot -- recompute the loot at a structure for one seed, independently of
// the search path. Given seed + structure type + block coords, it re-locates
// the piece, re-rolls every chest, and prints the item totals.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: checkloot <seed> <structure> <blockX> <blockZ> [version] [item]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    const char *sname = argv[2];
    int bx = atoi(argv[3]), bz = atoi(argv[4]);
    int mc = (argc > 5) ? str2mc(argv[5]) : MC_1_21;
    const char *want = (argc > 6) ? argv[6] : NULL;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    int type = -1;
    struct { const char *n; int t; } tbl[] = {
        {"desert_pyramid",Desert_Pyramid},{"jungle_temple",Jungle_Pyramid},
        {"igloo",Igloo},{"outpost",Outpost},{"shipwreck",Shipwreck}};
    for (int i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++)
        if (!strcmp(tbl[i].n, sname)) type = tbl[i].t;
    if (type < 0) { fprintf(stderr, "unsupported structure %s\n", sname); return 2; }

    // Apply the seed for the structure's own dimension -- bastion and fortress
    // are in the nether, and sampling an overworld biome for them would pick the
    // wrong variant/salt.
    StructureConfig scfg;
    int dim = getStructureConfig(type, mc, &scfg) ? scfg.dim : DIM_OVERWORLD;
    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, dim, seed);

    StructureVariant sv;
    int biome = getBiomeAt(&g, 0, (bx>>4)*4+2, 319>>2, (bz>>4)*4+2);
    getVariant(&sv, type, mc, seed & ((1ULL<<48)-1), bx, bz, biome);
    StructureSaltConfig ss;
    if (!getStructureSaltConfig(type, mc, -1, &ss)) { fprintf(stderr, "no salt\n"); return 1; }

    Piece pieces[64];
    int n = getStructurePieces(pieces, 64, type, ss, &sv, mc, seed & ((1ULL<<48)-1), bx, bz);

    // Aggregate every item across all chests.
    struct { char name[64]; int count; } agg[128]; int na = 0;
    for (int i = 0; i < n; i++)
    for (int c = 0; c < pieces[i].chestCount; c++) {
        LootTableContext *ctx = NULL;
        const char *t = pieces[i].lootTables[c];
        if (!t || !init_loot_table_name(&ctx, t, mc) || !ctx) continue;
        set_loot_seed(ctx, pieces[i].lootSeeds[c]);
        generate_loot(ctx);
        for (int k = 0; k < ctx->generated_item_count; k++) {
            const char *nm = get_item_name(ctx, ctx->generated_items[k].item);
            int j; for (j = 0; j < na; j++) if (!strcmp(agg[j].name, nm)) break;
            if (j == na && na < 128) { snprintf(agg[na].name, 64, "%s", nm); agg[na].count = 0; na++; }
            if (j < 128) agg[j].count += ctx->generated_items[k].count;
        }
    }

    printf("%lld %s (%d,%d):", (long long)seed, sname, bx, bz);
    if (want) {
        int total = 0;
        for (int j = 0; j < na; j++)
            if (!strcmp(agg[j].name, want) ||
                (!strncmp(agg[j].name,"minecraft:",10) && !strcmp(agg[j].name+10, want)))
                total += agg[j].count;
        printf(" %s=%d\n", want, total);
    } else {
        for (int j = 0; j < na; j++) printf(" %dx%s", agg[j].count, agg[j].name);
        printf("\n");
    }
    return 0;
}
