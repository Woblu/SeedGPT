// checkloot -- recompute the loot at a structure for one seed, independently of
// the search path. Given seed + structure type + block coords, it re-locates
// the piece, re-rolls every chest, and prints the item totals.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include "biomeat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: checkloot <seed> <structure> <blockX> <blockZ> [version] [item]\n"
                        "       checkloot <seed> <structure> <x> <z> <version> --ench <name> [minLevel]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    const char *sname = argv[2];
    int bx = atoi(argv[3]), bz = atoi(argv[4]);
    int mc = (argc > 5) ? str2mc(argv[5]) : MC_1_21;
    const char *want = (argc > 6) ? argv[6] : NULL;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    const char *wantEnch = NULL; int wantEnchLvl = 1;
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--ench") || a + 1 >= argc) continue;
        wantEnch = argv[a + 1];
        if (a + 2 < argc) { int v = atoi(argv[a + 2]); if (v > 0) wantEnchLvl = v; }
    }

    int type = -1;
    struct { const char *n; int t; } tbl[] = {
        {"desert_pyramid",Desert_Pyramid},{"jungle_temple",Jungle_Pyramid},
        {"igloo",Igloo},{"outpost",Outpost},{"shipwreck",Shipwreck},
        {"fortress",Fortress},{"bastion",Bastion}};
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
    int biome = chunkBiome(&g, bx, bz);
    getVariant(&sv, type, mc, seed, bx, bz, biome);
    StructureSaltConfig ss;
    if (!getStructureSaltConfig(type, mc, -1, &ss)) { fprintf(stderr, "no salt\n"); return 1; }

    // Fortresses run to a few hundred pieces (largest seen: 257 over a 3.6M
    // sample), so this buffer is sized for them, not for the handful the small
    // structures need. The engine refuses to exceed it rather than overrunning.
    enum { PIECE_CAP = 1024 };
    Piece *pieces = malloc((size_t)PIECE_CAP * sizeof(Piece));
    if (!pieces) { fprintf(stderr, "out of memory\n"); return 3; }
    // The FULL seed: on 1.18+ chest loot seeds depend on all 64 bits (loot.h).
    int n = getStructurePieces(pieces, PIECE_CAP, type, ss, &sv, mc, seed, bx, bz);
    if (n < 0) { fprintf(stderr, "engine declined to build pieces\n"); return 1; }
    if (n >= PIECE_CAP) { fprintf(stderr, "piece buffer full -- count would be short\n"); return 1; }

    // Aggregate every item across all chests.
    struct { char name[64]; int count; } agg[128]; int na = 0;
    // And every ENCHANTMENT, as (item, enchantment, level) triples. cubiomes has
    // carried these on ItemStack all along -- enchantment_count and
    // enchantments[] -- and nothing here ever read them, so this engine could
    // say "a golden sword" but never "a Looting III golden sword". That gap is
    // why two of CoolStuffFinder's four targets were out of reach.
    struct { char item[64]; char ench[48]; int level; int count; } ea[256]; int ne = 0;
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

            const ItemStack *is = &ctx->generated_items[k];
            for (int e = 0; e < is->enchantment_count && e < 16; e++) {
                int rawid = is->enchantments[e].enchantment;
                const char *en = get_enchantment_name(rawid);
                int lv = is->enchantments[e].level;
                char unk[32];
                if (!en) {
                    // NOT skipped. An unnamed enchantment is an UNKNOWN, and
                    // dropping it reads as "not enchanted" -- which is how the
                    // enchant_randomly path looked completely dead for 52
                    // golden items before anyone printed the id.
                    snprintf(unk, sizeof unk, "enchant#%d", rawid);
                    en = unk;
                }
                int q;
                for (q = 0; q < ne; q++)
                    if (!strcmp(ea[q].item, nm) && !strcmp(ea[q].ench, en)
                        && ea[q].level == lv) break;
                if (q == ne && ne < 256) {
                    snprintf(ea[ne].item, 64, "%s", nm);
                    snprintf(ea[ne].ench, 48, "%s", en);
                    ea[ne].level = lv; ea[ne].count = 0; ne++;
                }
                if (q < 256) ea[q].count += is->count;
            }
        }
    }

    // --ench <name> [minLevel] -> how many items carry it at that level or above.
    // A separate mode rather than a filter on the item line, because "how many
    // Looting III swords" and "how many swords" are different questions and
    // answering the second while being asked the first is the whole problem.
    if (wantEnch) {
        int total = 0;
        for (int q = 0; q < ne; q++)
            if (!strcmp(ea[q].ench, wantEnch) && ea[q].level >= wantEnchLvl)
                total += ea[q].count;
        printf("%lld %s (%d,%d): %s>=%d=%d\n", (long long)seed, sname, bx, bz,
               wantEnch, wantEnchLvl, total);
        free(pieces);
        return total > 0 ? 0 : 1;
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
        if (ne) {
            printf("  enchanted:");
            for (int q = 0; q < ne; q++)
                printf(" %dx%s{%s %d}", ea[q].count, ea[q].item, ea[q].ench, ea[q].level);
            printf("\n");
        }
    }
    return 0;
}
