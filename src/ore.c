#include "ore.h"
#include "terrain.h"
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

// Chunk-major ordering, so the terrain pass generates each chunk once.
static int cmpChunk(const void *a, const void *b)
{
    const Pos3 *p = a, *q = b;
    int pcx = p->x >> 4, qcx = q->x >> 4;
    if (pcx != qcx) return pcx - qcx;
    int pcz = p->z >> 4, qcz = q->z >> 4;
    if (pcz != qcz) return pcz - qcz;
    return 0;
}

// Binary search for a position in the sorted, de-duplicated array.
static int findPos3(const Pos3 *a, int n, Pos3 key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = cmpPos3(&a[mid], &key);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

// Largest face-connected blob among the sorted, unique positions. Iterative
// flood fill (a deep vein would blow a recursive stack, and coal veins in a
// 256-radius disc run to thousands of blocks).
static int largestVein(const Pos3 *a, int n)
{
    if (n <= 0) return 0;
    unsigned char *seen = calloc(n, 1);
    int *stack = malloc((size_t)n * sizeof(int));
    if (!seen || !stack) { free(seen); free(stack); return 0; }
    static const int D[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    int best = 0;
    for (int i = 0; i < n; i++) {
        if (seen[i]) continue;
        int top = 0, size = 0;
        stack[top++] = i; seen[i] = 1;
        while (top > 0) {
            int cur = stack[--top];
            size++;
            for (int d = 0; d < 6; d++) {
                Pos3 nb = { a[cur].x + D[d][0], a[cur].y + D[d][1], a[cur].z + D[d][2] };
                int j = findPos3(a, n, nb);
                if (j >= 0 && !seen[j]) { seen[j] = 1; stack[top++] = j; }
            }
        }
        if (size > best) best = size;
    }
    free(seen); free(stack);
    return best;
}

// Keep only the ore blocks with an air neighbour in real terrain. Works one
// chunk at a time: a chunk's 256 columns cost one generateRegion call, and the
// ore in a chunk is nearly always a handful of blocks, so per-chunk is far
// cheaper than per-block. Neighbours outside the chunk are treated as solid --
// a false negative at the border, never a false positive.
static int keepExposed(Pos3 *a, int n, int mc, uint32_t gflags, uint64_t worldSeed)
{
    if (n <= 0) return 0;
    TerrainNoise *tn = terrainFor(mc, worldSeed, gflags);
    if (!tn) return 0;   // no block-level terrain on this version: claim nothing

    int (*blocks)[TERRAIN_COLUMN] = malloc(16 * 16 * sizeof(*blocks));
    if (!blocks) return 0;

    // Group by chunk so each chunk's terrain is generated exactly once.
    qsort(a, n, sizeof(Pos3), cmpChunk);

    int out = 0, i = 0;
    while (i < n) {
        int chx = a[i].x >> 4, chz = a[i].z >> 4;
        int j = i;
        while (j < n && (a[j].x >> 4) == chx && (a[j].z >> 4) == chz) j++;
        generateRegion(tn, chx, chz, 1, 1, blocks, NULL, /*flag=*/0);
        for (int k = i; k < j; k++) {
            int rx = a[k].x - (chx << 4), rz = a[k].z - (chz << 4);
            int idx = a[k].y - TERRAIN_Y_MIN;
            if (idx < 0 || idx >= TERRAIN_COLUMN) continue;
            int open = 0;
            if (idx + 1 < TERRAIN_COLUMN && !blocks[rx * 16 + rz][idx + 1]) open = 1;
            if (!open && idx > 0 && !blocks[rx * 16 + rz][idx - 1]) open = 1;
            if (!open && rx > 0  && !blocks[(rx-1) * 16 + rz][idx]) open = 1;
            if (!open && rx < 15 && !blocks[(rx+1) * 16 + rz][idx]) open = 1;
            if (!open && rz > 0  && !blocks[rx * 16 + (rz-1)][idx]) open = 1;
            if (!open && rz < 15 && !blocks[rx * 16 + (rz+1)][idx]) open = 1;
            if (open) a[out++] = a[k];   // out <= k always, so this is in-place safe
        }
        i = j;
    }
    free(blocks);
    return out;
}

int oreCountMaterial(const Generator *g, const SurfaceNoise *sn, int mc,
                     const OreMaterial *mat, int cx, int cz, int radius)
{
    return oreScan(g, sn, mc, 0, mat, cx, cz, radius, 0, NULL);
}

int oreScan(const Generator *g, const SurfaceNoise *sn, int mc, uint32_t gflags,
            const OreMaterial *mat, int cx, int cz, int radius,
            int exposed, int *veinMax)
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

    if (veinMax) *veinMax = 0;
    if (nhits <= 0) { free(hits); return 0; }

    // Compact to unique positions in place; everything downstream (exposure,
    // vein connectivity) needs each block to appear exactly once.
    qsort(hits, nhits, sizeof(Pos3), cmpPos3);
    int uniq = 1;
    for (int i = 1; i < nhits; i++)
        if (cmpPos3(&hits[i], &hits[uniq-1]) != 0) hits[uniq++] = hits[i];

    if (exposed) {
        uniq = keepExposed(hits, uniq, mc, gflags, g->seed);
        qsort(hits, uniq, sizeof(Pos3), cmpPos3);   // keepExposed reorders
    }
    if (veinMax && uniq > 0) *veinMax = largestVein(hits, uniq);

    free(hits);
    return uniq;
}
