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
//
// THE SLOT IS THE CELL'S LOW BITS, and that is not a detail. A caller holds all
// four returned pointers at once, so any two cells sharing a slot would make the
// second lookup overwrite the data the first pointer still refers to -- handing
// generateColumn a duplicated corner and a quietly wrong interpolation. Under a
// hash, four cells collided about a third of the time, and the symptom was a
// surface height off by a block or two in roughly one column in five, which
// looks exactly like ordinary reimplementation drift. Indexing by (x&3, z&3)
// makes collision impossible instead of unlikely: consecutive cells always
// differ in their low two bits, so the four corners land in four distinct slots.
#define NCACHE 16
typedef struct { int cellX, cellZ; int valid; double ds[48 + 1]; } CellEntry;
static _Thread_local CellEntry g_cells[NCACHE];
static _Thread_local uint64_t  g_cellSeed  = 0;
static _Thread_local int       g_cellMc    = -1;
static _Thread_local uint32_t  g_cellFlags = 0;

static const double *noiseCell(TerrainNoise *tn, int cellX, int cellZ)
{
    CellEntry *e = &g_cells[((cellX & 3) << 2) | (cellZ & 3)];
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

// A FLOATING island: solid ground with a void beneath it AND air at the same
// height all around, so it is disconnected rather than the roof of a cave.
//
// terrainVoidBelow alone cannot tell those apart -- a cave roof also has a solid
// cap over a void, and it is attached to the world on every side. What makes an
// island an island is the horizontal gap, so that is what this measures: take
// the cap's own height, then ask whether the ring of columns at `ring` blocks
// away is AIR at that same height. Ground that continues outward disqualifies
// it, however deep the cavern underneath happens to be.
//
// Returns the void height under the cap (0 if it is not a floating island), and
// writes the cap's world Y to *capOut. `need` is how many of the 8 ring
// directions must be open: 8 is a true sky island, 5-6 allows one attached
// side, which is what most "floating" screenshots actually show.
int terrainFloating(int mc, uint64_t worldSeed, uint32_t flags, int x, int z,
                    int minVoid, int ring, int need, int *capOut)
{
    TerrainNoise *tn = terrainFor(mc, worldSeed, flags);
    if (!tn) return 0;

    int cap = 0;
    int voidH = terrainVoidBelow(mc, worldSeed, flags, x, z, 128, NULL);
    if (voidH < minVoid) return 0;

    // The cap is the lowest solid block of the surface slab -- the underside of
    // the island, not its peak. Measuring the gap at the peak would call a
    // mountain with a cave under it an island.
    {
        static _Thread_local int (*blocks)[TERRAIN_COLUMN];
        if (!blocks) blocks = malloc(16 * 16 * sizeof(*blocks));
        if (!blocks) return 0;
        int cx = x >> 4, cz = z >> 4;
        generateRegion(tn, cx, cz, 1, 1, blocks, NULL, 0);
        const int *col = blocks[(x - (cx << 4)) * 16 + (z - (cz << 4))];
        int surf = -1;
        for (int i = TERRAIN_COLUMN - 1; i >= 0; i--)
            if (col[i]) { surf = i; break; }
        if (surf < 0) return 0;
        int base = surf;
        while (base > 0 && col[base - 1]) base--;   // down through the slab
        cap = base;
    }
    if (capOut) *capOut = cap + TERRAIN_Y_MIN;

    // Is the ring open at the cap's height? Eight directions, so a peninsula
    // attached on one side is distinguishable from a true sky island.
    static const int DIR[8][2] = {{1,0},{-1,0},{0,1},{0,-1},
                                  {1,1},{1,-1},{-1,1},{-1,-1}};
    int open = 0;
    for (int d = 0; d < 8; d++) {
        int rx = x + DIR[d][0] * ring, rz = z + DIR[d][1] * ring;
        static _Thread_local int (*rb)[TERRAIN_COLUMN];
        if (!rb) rb = malloc(16 * 16 * sizeof(*rb));
        if (!rb) return 0;
        int cx = rx >> 4, cz = rz >> 4;
        generateRegion(tn, cx, cz, 1, 1, rb, NULL, 0);
        const int *rcol = rb[(rx - (cx << 4)) * 16 + (rz - (cz << 4))];
        // Two weaker tests were tried first and both called hillsides islands:
        // "air at the cap's height" passed for ~24% of outposts, and adding
        // "air 4 blocks lower" still passed 18%, because on a slope the ground
        // really is absent at those heights. What actually distinguishes a
        // floating island is that the surrounding GROUND ITSELF lies below the
        // island's underside -- there is nothing beside it at any height, not
        // merely a gap at one altitude.
        int rsurf = -1;
        for (int i = TERRAIN_COLUMN - 1; i >= 0; i--)
            if (rcol[i]) { rsurf = i; break; }
        if (rsurf < cap) open++;
    }
    if (open < need) return 0;
    return voidH;
}
