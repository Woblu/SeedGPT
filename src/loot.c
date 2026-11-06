#include "loot.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include <stdlib.h>
#include <string.h>

// A modest fixed cache: there are only ~10 distinct loot-table names across all
// searchable structures, so linear scan is fine and avoids any hashing.
#define MAX_TABLES 24

// cubiomes' loot contexts are SINGLETONS: init_*() returns &staticContext, so
// every thread (and every LootCache) shares one context per table, and
// generate_loot mutates that context. Concurrent rolls raced and produced
// nondeterministic counts, which a global lock around every roll fixed -- at
// the price of serialising every roll in the program, which capped a loot
// search at about twice single-thread speed however many threads ran.
//
// What a roll writes is small and all INSIDE the struct: prng_state,
// generated_item_count and generated_items[27]. The pools, names and
// functions it points to are read-only once built (generate_loot_pool reads
// pool->... and lf->params, and writes only through the context's own fields).
// So each thread takes a PRIVATE COPY of the struct (getCtx) and rolls it with
// no lock at all. The lock remains only around init_*, whose first call builds
// the shared tables and is not thread-safe.
static volatile int g_loot_lock = 0;
static inline void loot_lock(void)   { while (__atomic_exchange_n(&g_loot_lock, 1, __ATOMIC_ACQUIRE)) {} }
static inline void loot_unlock(void) { __atomic_store_n(&g_loot_lock, 0, __ATOMIC_RELEASE); }

// Nether fortresses are far bigger than any other piece structure here: over a
// 3.6-million-fortress sample the largest had 257 pieces and 84% had more than
// 64, so the old stack array was overrun by most fortresses rather than a rare
// one. 1024 is ~4x the observed maximum, and the engine now REFUSES to exceed
// whatever bound it is given (patches/fortress-piece-bound.patch), so going
// over is a detectable refusal instead of a write past the end of the array.
#define PIECE_CAP 1024

struct LootCache {
    char             *names[MAX_TABLES];
    LootTableContext *ctx[MAX_TABLES];
    int               n;
    int               mc;
    Piece            *pieces;    // per-thread scratch, so no allocation per roll
};

LootCache *lootCacheNew(void)
{
    LootCache *lc = calloc(1, sizeof *lc);
    if (!lc) return NULL;
    lc->mc = -1;
    // Callers treat the cache as infallible, so a failed piece buffer must not
    // turn into a NULL cache they don't check. lootCountItem refuses instead.
    lc->pieces = malloc((size_t)PIECE_CAP * sizeof(Piece));
    return lc;
}

void lootCacheFree(LootCache *lc)
{
    if (!lc) return;
    free(lc->pieces);
    // Our private copies are plain structs we allocated; free those. Never
    // free_loot_table: the arrays they point to belong to the shared context
    // init_* built, and freeing those double-frees and crashes.
    for (int i = 0; i < lc->n; i++) { free(lc->names[i]); free(lc->ctx[i]); }
    free(lc);
}

static LootTableContext *getCtx(LootCache *lc, int mc, const char *table)
{
    if (lc->mc != mc) {          // version changed: cached tables no longer valid
        for (int i = 0; i < lc->n; i++) { free(lc->names[i]); free(lc->ctx[i]); }
        lc->n = 0;
        lc->mc = mc;
    }
    for (int i = 0; i < lc->n; i++)
        if (!strcmp(lc->names[i], table)) return lc->ctx[i];
    if (lc->n >= MAX_TABLES) return NULL;

    LootTableContext *shared = NULL;
    loot_lock();                 // init_* builds shared static tables: not thread-safe
    int ok = init_loot_table_name(&shared, table, mc) && shared;
    loot_unlock();
    if (!ok) return NULL;
    LootTableContext *ctx = malloc(sizeof *ctx);
    if (!ctx) return NULL;
    *ctx = *shared;              // private mutable state; read-only tables shared
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
    // Ruined portals have no getStructurePieces chest case, so their chest is
    // computed directly (see rpChest below) from the template data -- every
    // piece of that computation is cross-checked against FeatureUtils/MCUtils
    // and cubiomes, so it meets the same bar as the five above.
    case Ruined_Portal:
        return 1;
    // Fortress: the engine simulates the whole corridor/bridge graph, so every
    // chest is found. What blocked it was the buffer overrun, now fixed on both
    // sides -- the engine honours its bound, and PIECE_CAP is 4x the largest
    // fortress in a 3.6M sample. Counts are exact.
    case Fortress:
        return 1;
    // Bastion: the engine simulates only the starting piece of each of the four
    // bastion types -- the chests that ALWAYS generate (2 for units, 1 for
    // hoglin stable, 2 for treasure, 1 for bridge). Those chests are exact; the
    // ones in the randomly-assembled remainder are not modelled at all. So a
    // bastion loot count is a LOWER BOUND: every hit is real, but a bastion
    // whose only copy of the item sits in a generated-later chest is missed.
    // That asymmetry is stated in the README and in /api/lootitems rather than
    // papered over.
    case Bastion:
        return 1;
    default:
        return 0;
    }
}

// Only the guaranteed starting-piece chests are modelled for these types, so
// the count is a floor rather than a total. Callers that must not overstate
// coverage ask here instead of hardcoding the list.
int lootCountIsLowerBound(int structType)
{
    return structType == Bastion;
}

// ---- ruined portal chest, computed from template data ----------------------
// cubiomes gives the portal's position, variant (template start), rotation,
// mirror and biome via getVariant, but no chest. The chest offset per template
// comes from FeatureUtils (RuinedPortalGenerator.STRUCTURE_TO_LOOT); the offset
// is transformed into the world with Minecraft's canonical template transform
// (verified identical to MCUtils' BPos.transform for all 104 variant/rotation/
// mirror cases); and the loot seed is the decoration seed at the chest's chunk
// with salt 40005 (equal in cubiomes and FeatureUtils) and no pre-consumption
// (ruined portal's getSpecificCalls() is null). One chest per portal.
static const int RP_CHEST[2][10][3] = {
    {{2,2,0},{8,2,6},{3,3,6},{3,3,2},{4,3,2},{1,1,4},{0,1,2},{4,4,2},{4,1,0},{2,1,7}},
    {{4,3,3},{9,1,9},{9,2,3}},
};
static const int RP_SIZE[2][10][3] = {
    {{6,10,6},{9,12,9},{8,9,9},{8,9,9},{10,7,7},{5,7,7},{9,7,9},{14,9,9},{10,8,9},{12,8,10}},
    {{11,17,16},{11,16,16},{16,16,16}},
};

static void rpChestXZ(const StructureVariant *sv, int minBlockX, int minBlockZ,
                      int *cx, int *cz)
{
    int gi = sv->giant ? 1 : 0, idx = sv->start - 1;
    if (idx < 0 || idx >= (gi ? 3 : 10)) { *cx = minBlockX; *cz = minBlockZ; return; }
    const int *off = RP_CHEST[gi][idx];
    const int *sz  = RP_SIZE[gi][idx];
    int ox = off[0], oz = off[2];
    int px = sz[0] / 2, pz = sz[2] / 2;
    int i = sv->mirror ? -ox : ox, k = oz;   // FRONT_BACK mirrors X
    int rx, rz;
    switch (sv->rotation) {                  // 0:none 1:cw90 2:cw180 3:ccw90
    case 1: rx = px + pz - k; rz = pz - px + i; break;
    case 2: rx = px + px - i; rz = pz + pz - k; break;
    case 3: rx = px - pz + k; rz = px + pz - i; break;
    default: rx = i; rz = k; break;
    }
    *cx = minBlockX + rx;
    *cz = minBlockZ + rz;
}

// `seed` is the full world seed: on 1.18+ the population seed depends on all
// 64 bits (see loot.h).
static uint64_t rpLootSeed(int mc, uint64_t seed, const StructureVariant *sv,
                           int chestX, int chestZ)
{
    StructureSaltConfig ss;
    if (!getStructureSaltConfig(Ruined_Portal, mc, sv->biome, &ss)) return 0;
    uint64_t pop = getPopulationSeed(mc, seed, chestX & ~15, chestZ & ~15);
    CREATE_RANDOM_SOURCE(rnd, mc <= MC_1_17);
    rnd.setSeed(rnd.state, pop + ss.decoratorIndex + 10000ULL * ss.generationStep);
    return rnd.nextLong(rnd.state);          // no pre-consumption for ruined portal
}

static int rpCountItem(LootCache *lc, int mc, uint64_t seed, int blockX, int blockZ,
                       const StructureVariant *sv, const char *item)
{
    int cx, cz;
    rpChestXZ(sv, blockX & ~15, blockZ & ~15, &cx, &cz);
    uint64_t lootSeed = rpLootSeed(mc, seed, sv, cx, cz);
    // getCtx returns this thread's private copy, so the roll needs no lock.
    LootTableContext *ctx = getCtx(lc, mc, "ruined_portal");
    int total = -1;
    if (ctx) {
        int itemId = get_item_id(ctx, item);
        if (itemId < 0) total = 0;
        else {
            set_loot_seed(ctx, lootSeed);
            generate_loot(ctx);
            total = 0;
            for (int k = 0; k < ctx->generated_item_count; k++)
                if (ctx->generated_items[k].item == itemId)
                    total += ctx->generated_items[k].count;
        }
    }
    return total;
}

int lootCountItem(LootCache *lc, int mc, uint64_t seed, int structType,
                  int blockX, int blockZ, StructureVariant *sv, const char *item)
{
    if (!lc || !lootStructureSupported(structType)) return -1;

    // Ruined portals aren't in getStructurePieces; compute their single chest
    // from the template data instead.
    if (structType == Ruined_Portal)
        return rpCountItem(lc, mc, seed, blockX, blockZ, sv, item);

    StructureSaltConfig ss;
    // The decoration salt can depend on the biome: from 1.18 an ocean
    // shipwreck and a beached one are separate structures with separate
    // decoration indices, and biome -1 reads as "not oceanic", so every wreck
    // got the beached salt and every ocean wreck's chests the wrong loot seeds
    // (a real 1.21.1 server: 3/3 chests right on a beached wreck, 0/2 on an
    // ocean one). getVariant keeps the biome for exactly this.
    if (!getStructureSaltConfig(structType, mc, sv ? sv->biome : -1, &ss)) return -1;

    Piece *pieces = lc->pieces;
    if (!pieces) return -1;
    int n = getStructurePieces(pieces, PIECE_CAP, structType, ss, sv, mc, seed, blockX, blockZ);
    if (n < 0) return -1;
    // A fortress that fills the buffer was cut short, and the pieces after the
    // cut would have consumed RNG -- so the chests we did roll are right but the
    // total is not. Refuse rather than report a number we know is short.
    if (n >= PIECE_CAP) return -1;

    int total = 0;
    // No lock: getCtx hands back this thread's private copy of each context.
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
