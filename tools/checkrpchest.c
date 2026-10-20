// checkrpchest -- compute a ruined portal's chest position and loot from
// scratch, for one seed. PROTOTYPE / verifier for ruined-portal chest loot.
//
// cubiomes has no getStructurePieces case for ruined portals, but everything
// needed is present: getVariant gives the template (start), rotation, mirror and
// biome; getStructureSaltConfig ships the biome/version-aware decorator salts;
// getPopulationSeed + the salt give the loot seed exactly as for shipwrecks.
// The only data not in cubiomes is each template's chest offset -- taken from
// KaptainWutax/FeatureUtils (RuinedPortalGenerator), and cross-checked: 12/13
// template SIZES match cubiomes' own table exactly (portal_5 differs only in Y,
// which does not affect the chest's x/z), confirming start<->portal_N alignment.
//
// The chest world position uses Minecraft's canonical template transform
// (StructureTemplate.transform: mirror on X for FRONT_BACK, then rotate about
// pivot = size/2). Loot is rolled with cubiomes' verified loot tables.
#include "finders.h"
#include "generator.h"
#include "biomes.h"
#include "rng.h"
#include "util.h"
#include "loot.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include "biomeat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Chest offset (x,y,z) within each template, indexed [giant][start-1].
static const int RP_CHEST[2][10][3] = {
    {{2,2,0},{8,2,6},{3,3,6},{3,3,2},{4,3,2},{1,1,4},{0,1,2},{4,4,2},{4,1,0},{2,1,7}},
    {{4,3,3},{9,1,9},{9,2,3}},
};
// Template size (x,y,z); only x,z feed the rotation pivot.
static const int RP_SIZE[2][10][3] = {
    {{6,10,6},{9,12,9},{8,9,9},{8,9,9},{10,7,7},{5,7,7},{9,7,9},{14,9,9},{10,8,9},{12,8,10}},
    {{11,17,16},{11,16,16},{16,16,16}},
};

// Minecraft StructureTemplate.transform, x/z only. mirror = FRONT_BACK (x->-x).
static void rpChestXZ(const StructureVariant *sv, int minBlockX, int minBlockZ,
                      int *cx, int *cz)
{
    int gi = sv->giant ? 1 : 0, idx = sv->start - 1;
    const int *off = RP_CHEST[gi][idx];
    const int *sz  = RP_SIZE[gi][idx];
    int ox = off[0], oz = off[2];
    int px = sz[0] / 2, pz = sz[2] / 2;
    int i = sv->mirror ? -ox : ox, k = oz;
    int rx, rz;
    switch (sv->rotation) {                 // 0:none 1:cw90 2:cw180 3:ccw90
    case 1: rx = px + pz - k; rz = pz - px + i; break;
    case 2: rx = px + px - i; rz = pz + pz - k; break;
    case 3: rx = px - pz + k; rz = px + pz - i; break;
    default: rx = i; rz = k; break;
    }
    *cx = minBlockX + rx;
    *cz = minBlockZ + rz;
}

// Loot seed for a ruined portal chest, pinned to FeatureUtils' RuinedPortal
// (ILoot.getLoot) and cross-checked against cubiomes:
//   - decorator salt = 40005, and cubiomes' decoratorIndex + 10000*generationStep
//     equals 40005 for 1.17 -- two implementations agree on the salt.
//   - the population seed is taken at the CHEST's chunk (setDecoratorSeed with
//     chunkChestPos.getX()*16), not the portal origin.
//   - ruined portal's getSpecificCalls() is null and shouldAdvanceInChunks() is
//     false, and there is one chest (index 0) -- so, unlike igloo (which
//     discards a nextLong) or desert pyramid (nextInt(3)), there is NO
//     pre-consumption: set the decorator seed, then read one nextLong.
static uint64_t rpLootSeed(int mc, uint64_t seed, const StructureVariant *sv,
                           int chestX, int chestZ)
{
    StructureSaltConfig ss;
    if (!getStructureSaltConfig(Ruined_Portal, mc, sv->biome, &ss)) return 0;
    uint64_t pop = getPopulationSeed(mc, seed, chestX & ~15, chestZ & ~15);  // all 64 bits on 1.18+
    CREATE_RANDOM_SOURCE(rnd, mc <= MC_1_17);
    rnd.setSeed(rnd.state, pop + ss.decoratorIndex + 10000ULL * ss.generationStep);
    return rnd.nextLong(rnd.state);
}

// Where the portal actually IS, and whether you will see it standing there.
//
// This tool reported x and z and nothing else, and that is the whole reason
// ruined portals read as hallucinated: at (32,112) in seed 20094 the portal is
// real and sits at y=37 on the seabed under 26 blocks of ocean. Fly to the
// coordinates, look around, see water, conclude the finder invented it.
//
// The height is cubiomes' surface estimate, which matched the measured portal
// blocks at both spots checked (36 vs 37, 64 vs 64). It is an ESTIMATE and is
// printed as one.
static void portal_visibility(Generator *g, int mc, int x, int z,
                              int underground, char *out, size_t n)
{
    (void)mc;
    // mapApproxHeight is what checkheight.exe uses and what cubiomes offers for
    // a surface estimate; there is no exact block-height call here. Pre-1.18
    // also needs a SurfaceNoise, which 1.18+ does not.
    float fy = 0;
    SurfaceNoise sn; SurfaceNoise *snp = NULL;
    if (mc < MC_1_18) { initSurfaceNoise(&sn, DIM_OVERWORLD, g->seed); snp = &sn; }
    mapApproxHeight(&fy, NULL, g, snp, x >> 2, z >> 2, 1, 1);
    int y = (int)fy;
    const char *how;
    if (underground)   how = "BURIED -- underground variant, dig down";
    else if (y < 60)   how = "SUBMERGED -- on the sea floor, look underwater";
    else               how = "surface";
    snprintf(out, n, "y~%d %s", y, how);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: checkrpchest <seed> [version] [radius] [item]\n"
                        "       checkrpchest <seed> <version> --at <x> <z> [item]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    uint64_t s48 = seed & ((1ULL << 48) - 1);

    Generator g; setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    // --at <x> <z>: verify one specific portal (the position the finder matched).
    if (argc >= 6 && !strcmp(argv[3], "--at")) {
        Pos best = { atoi(argv[4]), atoi(argv[5]) };
        const char *want = (argc > 6) ? argv[6] : NULL;
        int biomeID = chunkBiome(&g, best.x, best.z);
        StructureVariant sv;
        getVariant(&sv, Ruined_Portal, mc, s48, best.x, best.z, biomeID);
        int cx, cz;
        rpChestXZ(&sv, best.x & ~15, best.z & ~15, &cx, &cz);
        uint64_t lootSeed = rpLootSeed(mc, seed, &sv, cx, cz);
        LootTableContext *ctx = NULL;
        if (!init_loot_table_name(&ctx, "ruined_portal", mc) || !ctx) return 1;
        set_loot_seed(ctx, lootSeed); generate_loot(ctx);
        int wc = 0;
        char vis[96];
        portal_visibility(&g, mc, best.x, best.z, sv.underground, vis, sizeof vis);
        printf("  %s\n", vis);
        printf("%lld ruined_portal x=%d z=%d chest x=%d z=%d loot:",
               (long long)seed, best.x, best.z, cx, cz);
        for (int k = 0; k < ctx->generated_item_count; k++) {
            const char *nm = get_item_name(ctx, ctx->generated_items[k].item);
            printf(" %dx%s", ctx->generated_items[k].count, nm);
            // Enchantments inline, in the same {name level} form checkloot uses.
            // A ruined portal's golden sword is the classic enchanted-loot case
            // -- it is half of what CoolStuffFinder looks for -- and this tool
            // printed it as a bare "golden_sword", which is a different and much
            // weaker claim than "Looting III golden sword".
            const ItemStack *is = &ctx->generated_items[k];
            for (int e = 0; e < is->enchantment_count && e < 16; e++) {
                int rawid = is->enchantments[e].enchantment;
                const char *en = get_enchantment_name(rawid);
                if (en) printf("{%s %d}", en, is->enchantments[e].level);
                else    printf("{enchant#%d %d}", rawid, is->enchantments[e].level);
            }
            if (want && strstr(nm, want)) wc += ctx->generated_items[k].count;
        }
        printf("\n");
        if (want) printf("  %s=%d\n", want, wc);
        return 0;
    }

    int radius = (argc > 3) ? atoi(argv[3]) : 3000;
    const char *want = (argc > 4) ? argv[4] : NULL;

    StructureConfig sc;
    if (!getStructureConfig(Ruined_Portal, mc, &sc)) { fprintf(stderr, "no config\n"); return 2; }
    double span = sc.regionSize * 16.0;
    int r0 = (int)floor((-radius) / span), r1 = (int)floor((radius) / span);
    Pos best; int64_t bestd = -1; int found = 0;
    for (int rx = r0; rx <= r1; rx++)
    for (int rz = r0; rz <= r1; rz++) {
        Pos p;
        if (!getStructurePos(Ruined_Portal, mc, s48, rx, rz, &p)) continue;
        if ((int64_t)p.x*p.x + (int64_t)p.z*p.z > (int64_t)radius*radius) continue;
        if (!isViableStructurePos(Ruined_Portal, &g, p.x, p.z, 0)) continue;
        int64_t d = (int64_t)p.x*p.x + (int64_t)p.z*p.z;
        if (bestd < 0 || d < bestd) { bestd = d; best = p; found = 1; }
    }
    if (!found) { printf("%lld no ruined_portal within %d\n", (long long)seed, radius); return 1; }

    int biomeID = chunkBiome(&g, best.x, best.z);
    StructureVariant sv;
    getVariant(&sv, Ruined_Portal, mc, s48, best.x, best.z, biomeID);

    int cx, cz;
    rpChestXZ(&sv, best.x & ~15, best.z & ~15, &cx, &cz);
    uint64_t lootSeed = rpLootSeed(mc, seed, &sv, cx, cz);

    char vis0[96];
    portal_visibility(&g, mc, best.x, best.z, sv.underground, vis0, sizeof vis0);
    printf("  %s\n", vis0);
    printf("%lld ruined_portal x=%d z=%d biome=%s start=%d giant=%d rot=%d mir=%d underground=%d\n",
           (long long)seed, best.x, best.z, biome2str(mc, biomeID),
           sv.start, sv.giant, sv.rotation, sv.mirror, sv.underground);
    printf("  chest x=%d z=%d  lootSeed=%lld\n", cx, cz, (long long)lootSeed);

    LootTableContext *ctx = NULL;
    if (!init_loot_table_name(&ctx, "ruined_portal", mc) || !ctx) { fprintf(stderr, "no loot table\n"); return 1; }
    set_loot_seed(ctx, lootSeed);
    generate_loot(ctx);
    printf("  loot:");
    int wantCount = 0;
    for (int k = 0; k < ctx->generated_item_count; k++) {
        const char *nm = get_item_name(ctx, ctx->generated_items[k].item);
        int cnt = ctx->generated_items[k].count;
        printf(" %dx%s", cnt, nm);
        const ItemStack *is = &ctx->generated_items[k];
        for (int e = 0; e < is->enchantment_count && e < 16; e++) {
            int rawid = is->enchantments[e].enchantment;
            const char *en = get_enchantment_name(rawid);
            if (en) printf("{%s %d}", en, is->enchantments[e].level);
            else    printf("{enchant#%d %d}", rawid, is->enchantments[e].level);
        }
        if (want && (strstr(nm, want))) wantCount += cnt;
    }
    printf("\n");
    if (want) printf("  %s=%d\n", want, wantCount);
    return 0;
}
