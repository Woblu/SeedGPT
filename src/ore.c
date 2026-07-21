#include "ore.h"
#include <stdlib.h>
#include <string.h>

// Materials the finder exposes, each keyed to the block its configs place.
// Overworld ores dominate demand; a few nether ores are included too.
static const OreMaterial MATS[] = {
    {"diamond",       DIAMOND_ORE,       DIM_OVERWORLD},
    {"iron",          IRON_ORE,          DIM_OVERWORLD},
    {"gold",          GOLD_ORE,          DIM_OVERWORLD},
    {"emerald",       EMERALD_ORE,       DIM_OVERWORLD},
    {"redstone",      REDSTONE_ORE,      DIM_OVERWORLD},
    {"lapis",         LAPIS_ORE,         DIM_OVERWORLD},
    {"copper",        COPPER_ORE,        DIM_OVERWORLD},
    {"coal",          COAL_ORE,          DIM_OVERWORLD},
    {"quartz",        NETHER_QUARTZ_ORE, DIM_NETHER},
    {"ancient_debris",ANCIENT_DEBRIS,    DIM_NETHER},
    {"nether_gold",   NETHER_GOLD_ORE,   DIM_NETHER},
};
#define NMAT ((int)(sizeof(MATS)/sizeof(MATS[0])))

const OreMaterial *oreMaterials(int *n) { *n = NMAT; return MATS; }

const OreMaterial *oreMaterialByName(const char *name)
{
    for (int i = 0; i < NMAT; i++)
        if (!strcmp(MATS[i].name, name)) return &MATS[i];
    return NULL;
}

static int cmpPos3(const void *a, const void *b)
{
    const Pos3 *p = a, *q = b;
    if (p->x != q->x) return p->x - q->x;
    if (p->z != q->z) return p->z - q->z;
    return p->y - q->y;
}

int oreCountMaterial(const Generator *g, const SurfaceNoise *sn, int mc,
                     const OreMaterial *mat, int cx, int cz, int radius)
{
    int dim = mat->dim;
    int64_t lim = (int64_t)radius * radius;
    int c0x = (cx - radius) >> 4, c1x = (cx + radius) >> 4;
    int c0z = (cz - radius) >> 4, c1z = (cz + radius) >> 4;

    // Collect every matching ore block, then de-duplicate: two configs (e.g.
    // regular and buried diamond) can place the same block, and Minecraft would
    // show one, so counting one is the honest measure.
    Pos3 *hits = NULL; int nhits = 0, cap = 0;

    for (int chx = c0x; chx <= c1x; chx++)
    for (int chz = c0z; chz <= c1z; chz++) {
        // Ore biome is sampled at the chunk's decoration anchor.
        int biome = getBiomeAt(g, 1, (chx << 4) + 8, 64, (chz << 4) + 8);
        for (int ot = 0; ot < ORE_NUM; ot++) {
            OreConfig oc;
            if (!getOreConfig(ot, mc, biome, &oc)) continue;
            if (oc.oreBlock != mat->block || oc.dim != dim) continue;
            Pos3List list = generateOres(g, sn, oc, chx, chz);
            for (int i = 0; i < list.size; i++) {
                Pos3 p = list.pos3s[i];
                int64_t dx = p.x - cx, dz = p.z - cz;
                if (dx*dx + dz*dz > lim) continue;
                if (nhits >= cap) {
                    cap = cap ? cap * 2 : 64;
                    hits = realloc(hits, cap * sizeof(Pos3));
                }
                hits[nhits++] = p;
            }
            freePos3List(&list);
        }
    }

    if (nhits <= 1) { free(hits); return nhits; }
    qsort(hits, nhits, sizeof(Pos3), cmpPos3);
    int uniq = 1;
    for (int i = 1; i < nhits; i++)
        if (cmpPos3(&hits[i], &hits[i-1]) != 0) uniq++;
    free(hits);
    return uniq;
}
