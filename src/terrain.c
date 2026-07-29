#include "terrain.h"
#include "generator.h"
#include <stdlib.h>

// One context per thread: setupTerrainNoise is heavy, initTerrainNoise is per
// seed, and a worker evaluates thousands of seeds in a row.
static _Thread_local TerrainNoise g_tn;
static _Thread_local int      g_setup  = 0;   // 0=unset, 1=ok, -1=unsupported
static _Thread_local int      g_mc     = -1;
static _Thread_local uint32_t g_flags  = 0;
static _Thread_local uint64_t g_seed   = 0;
static _Thread_local int      g_inited = 0;

TerrainNoise *terrainFor(int mc, uint64_t worldSeed, uint32_t flags)
{
    if (g_setup == 0 || g_mc != mc || g_flags != flags) {
        g_setup = setupTerrainNoise(&g_tn, mc, (int)flags) ? 1 : -1;
        g_mc = mc; g_flags = flags;
        g_inited = 0;
    }
    if (g_setup < 0) return NULL;
    if (!g_inited || g_seed != worldSeed) {
        if (!initTerrainNoise(&g_tn, worldSeed, DIM_OVERWORLD)) return NULL;
        g_seed = worldSeed; g_inited = 1;
    }
    return &g_tn;
}

// Noise-column cache. generateColumn needs the four columns bounding the 4x4
// cell a block sits in; adjacent lookups reuse them, and a footprint check hits
// the same handful of cells repeatedly. Direct-mapped: a miss just recomputes.
#define NCACHE 16
typedef struct { int cellX, cellZ; int valid; double ds[48 + 1]; } CellEntry;
static _Thread_local CellEntry g_cells[NCACHE];
static _Thread_local uint64_t  g_cellSeed  = 0;
static _Thread_local int       g_cellMc    = -1;
static _Thread_local uint32_t  g_cellFlags = 0;

static const double *noiseCell(TerrainNoise *tn, int cellX, int cellZ)
{
    unsigned h = (unsigned)(cellX * 0x9E3779B1u + cellZ * 0x85EBCA77u);
    CellEntry *e = &g_cells[h % NCACHE];
    if (e->valid && e->cellX == cellX && e->cellZ == cellZ) return e->ds;
    sampleNoiseColumn(tn, cellX, cellZ, e->ds);
    e->cellX = cellX; e->cellZ = cellZ; e->valid = 1;
    return e->ds;
}

int terrainSurfaceY(int mc, uint64_t worldSeed, uint32_t flags, int x, int z, int *ok)
{
    TerrainNoise *tn = terrainFor(mc, worldSeed, flags);
    if (!tn) { if (ok) *ok = 0; return 0; }
    // The cache holds noise for one seed; a new seed invalidates all of it.
    if (g_cellSeed != worldSeed || g_cellMc != mc || g_cellFlags != flags) {
        for (int i = 0; i < NCACHE; i++) g_cells[i].valid = 0;
        g_cellSeed = worldSeed; g_cellMc = mc; g_cellFlags = flags;
    }
    int cellX = x >> 2, cellZ = z >> 2;
    const double *ds00 = noiseCell(tn, cellX,     cellZ);
    const double *ds01 = noiseCell(tn, cellX,     cellZ + 1);
    const double *ds10 = noiseCell(tn, cellX + 1, cellZ);
    const double *ds11 = noiseCell(tn, cellX + 1, cellZ + 1);
    if (ok) *ok = 1;
    return generateColumn(x, z, NULL, ds00, ds01, ds10, ds11, /*flag=*/1) - TERRAIN_Y_BIAS;
}

int terrainVoidBelow(int mc, uint64_t worldSeed, uint32_t flags, int x, int z,
                     int depth, int *topOut)
{
    TerrainNoise *tn = terrainFor(mc, worldSeed, flags);
    if (!tn) return 0;
    // One chunk's worth of columns is the cheapest unit generateRegion offers
    // with the block data attached; the caller samples a few points inside a
    // structure footprint, which almost always share a chunk.
    static _Thread_local int (*blocks)[TERRAIN_COLUMN];
    if (!blocks) blocks = malloc(16 * 16 * sizeof(*blocks));
    if (!blocks) return 0;
    int cx = x >> 4, cz = z >> 4;
    generateRegion(tn, cx, cz, 1, 1, blocks, NULL, /*flag=*/0);
    int rx = x - (cx << 4), rz = z - (cz << 4);
    const int *col = blocks[rx * 16 + rz];

    // Find the surface, then the tallest run of non-solid blocks under it.
    // A cavern the structure sits over is what this measures; open air above
    // the ground is not a cave and must not count, which is why the scan
    // starts strictly below the first solid block.
    int surf = -1;
    for (int i = TERRAIN_COLUMN - 1; i >= 0; i--)
        if (col[i]) { surf = i; break; }
    if (surf < 0) return 0;
    int best = 0, run = 0, bestTop = 0;
    int lo = surf - depth; if (lo < 0) lo = 0;
    for (int i = surf - 1; i >= lo; i--) {
        if (!col[i]) {
            if (++run > best) { best = run; bestTop = i + run - 1; }
        } else {
            run = 0;
        }
    }
    if (topOut) *topOut = bestTop + TERRAIN_Y_MIN;
    return best;
}
