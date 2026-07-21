#include "loot.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include <stdlib.h>
#include <string.h>

// A modest fixed cache: there are only ~10 distinct loot-table names across all
// searchable structures, so linear scan is fine and avoids any hashing.
#define MAX_TABLES 24

struct LootCache {
    char             *names[MAX_TABLES];
    LootTableContext *ctx[MAX_TABLES];
    int               n;
    int               mc;
};

LootCache *lootCacheNew(void)
{
    LootCache *lc = calloc(1, sizeof *lc);
    lc->mc = -1;
    return lc;
}

void lootCacheFree(LootCache *lc)
{
    if (!lc) return;
    for (int i = 0; i < lc->n; i++) free(lc->names[i]);
    // cubiomes loot contexts are heap-allocated by init_*; free_loot_table
    // exists but freeing a context that init_* built from static tables can
    // double-free shared arrays and crash. A worker holds one cache for the
    // whole run (<=24 contexts), so leaking them is bounded and safe.
    free(lc);
}

static LootTableContext *getCtx(LootCache *lc, int mc, const char *table)
{
    if (lc->mc != mc) {          // version changed: cached tables no longer valid
        for (int i = 0; i < lc->n; i++) free(lc->names[i]);
        lc->n = 0;   // leak the old contexts (see lootCacheFree); bounded + safe
        lc->mc = mc;
    }
    for (int i = 0; i < lc->n; i++)
        if (!strcmp(lc->names[i], table)) return lc->ctx[i];
    if (lc->n >= MAX_TABLES) return NULL;

    LootTableContext *ctx = NULL;
    if (!init_loot_table_name(&ctx, table, mc) || !ctx) return NULL;
    lc->names[lc->n] = strdup(table);
    lc->ctx[lc->n] = ctx;
    lc->n++;
    return ctx;
}

int lootStructureSupported(int structType)
{
    switch (structType) {
    case Desert_Pyramid:
    case Jungle_Pyramid:
    case Igloo:
    case Outpost:
    case Shipwreck:
        return 1;
    // The nether structures are deliberately excluded, each for a hard reason:
    //   - Fortress: getFortressPieces stores its buffer bound (env.nmax) but
    //     never enforces it, so a large fortress overflows the piece array and
    //     crashes the search.
    //   - Bastion: getStructurePieces only simulates some pieces, and the loot
    //     the search reports did NOT reproduce under independent verification
    //     (find over-counted vs checkloot). A result we cannot confirm violates
    //     the project's core rule, so it is not offered.
    // Ruined portals have no chest enumeration at all. See README "Chest loot".
    default:
        return 0;
    }
}

int lootCountItem(LootCache *lc, int mc, uint64_t seed, int structType,
                  int blockX, int blockZ, StructureVariant *sv, const char *item)
{
    if (!lootStructureSupported(structType)) return -1;

    StructureSaltConfig ss;
    // Loot placement uses a decoration salt config; the biome argument only
    // matters for the position salt (already resolved), so -1 is fine here.
    if (!getStructureSaltConfig(structType, mc, -1, &ss)) return -1;

    Piece pieces[64];
    int n = getStructurePieces(pieces, 64, structType, ss, sv, mc, seed, blockX, blockZ);

    int total = 0;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < pieces[i].chestCount; c++) {
            const char *table = pieces[i].lootTables[c];
            if (!table) continue;
            LootTableContext *ctx = getCtx(lc, mc, table);
            if (!ctx) continue;

            int itemId = get_item_id(ctx, item);
            if (itemId < 0) continue;   // this item can't come from this table

            set_loot_seed(ctx, pieces[i].lootSeeds[c]);
            generate_loot(ctx);
            for (int k = 0; k < ctx->generated_item_count; k++)
                if (ctx->generated_items[k].item == itemId)
                    total += ctx->generated_items[k].count;
        }
    }
    return total;
}
