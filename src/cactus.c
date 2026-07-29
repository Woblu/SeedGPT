#include "cactus.h"
#include "terrain.h"
#include "finders.h"
#include "rng.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Tracing exists because the failure mode here is silent: a mispredicted try
// consumes the wrong number of draws and every later try in the patch drifts,
// so the visible symptom is a cactus missing somewhere else entirely.
int cactusTrace = 0;

// A patch's origin is somewhere in its own chunk (in_square: 0..15) and its
// tries reach 7 further, so a placement lands at most 22 blocks past the chunk
// corner -- 7 blocks into the next chunk. One ring of neighbours is therefore
// enough to feed any column, and a second ring would be wasted work.
#define REACH_CHUNKS 1

// ---------------------------------------------------------------- world model
//
// Only three things about a column matter to a cactus: where its terrain stops,
// whether the block on top is sand, and what cactus is already standing there.
// Everything above the surface is air until a cactus occupies it, which is
// sound here because cacti are the LAST vegetal feature desert generates
// (index 74, after dead bushes at 61 and pumpkins at 72) -- see the note in
// cactusRegion about what that costs us.
typedef struct {
    int  x0, z0, w, h;
    int      *surf;    // world Y of the top terrain block
    unsigned char *known;
    unsigned char *sand;
    short    *cact;    // cactus blocks stacked directly on the surface
    // context for filling lazily
    Generator *g;
    int mc; uint32_t gflags; uint64_t seed;
} World;

static int widx(const World *w, int x, int z)
{
    int i = x - w->x0, j = z - w->z0;
    if (i < 0 || j < 0 || i >= w->w || j >= w->h) return -1;
    return j * w->w + i;
}

// Terrain is computed only where something actually asks, because 5 chunks in 6
// fail the rarity roll before any block is needed.
static int colSurf(World *w, int x, int z, int *sandOut)
{
    int i = widx(w, x, z);
    if (i < 0) {
        int ok = 0;
        int y = terrainSurfaceY(w->mc, w->seed, w->gflags, x, z, &ok);
        if (sandOut) *sandOut = 0;
        return ok ? y : -1000;
    }
    if (!w->known[i]) {
        int ok = 0;
        w->surf[i] = terrainSurfaceY(w->mc, w->seed, w->gflags, x, z, &ok);
        if (!ok) w->surf[i] = -1000;
        // Desert and badlands surface rules put sand (or red sand) on top, and
        // BlockTags.SAND is exactly what a cactus will stand on. The biome is
        // sampled at the surface, the way the placement's own biome filter does.
        int b = getBiomeAt(w->g, 4, x >> 2, (w->surf[i] + 1) >> 2, z >> 2);
        w->sand[i] = (b == desert || b == badlands || b == eroded_badlands ||
                      b == wooded_badlands || b == beach || b == desert_lakes);
        w->known[i] = 1;
    }
    if (sandOut) *sandOut = w->sand[i];
    return w->surf[i];
}

static int colCact(const World *w, int x, int z)
{
    int i = widx(w, x, z);
    return i < 0 ? 0 : w->cact[i];
}

// Above the terrain a column is water up to sea level and air past it. Leaving
// the water out was worth 13 false positives in one 361-chunk check, all of them
// placements at y=62 -- the top water block -- where the game sees water, fails
// `matching_blocks air`, and spends no randomness at all.
#define SEA_TOP 62      // sea_level is 63 in the overworld preset; water fills
                        // to one below it

static int isWater(World *w, int x, int y, int z)
{
    int s = colSurf(w, x, z, NULL);
    return y > s + colCact(w, x, z) && y <= SEA_TOP;
}

static int isAir(World *w, int x, int y, int z)
{
    int s = colSurf(w, x, z, NULL);
    return y > s + colCact(w, x, z) && y > SEA_TOP;
}

// CactusBlock.canSurvive, transcribed:
//   no horizontally adjacent solid block (cactus IS solid -- its collision box
//   averages 0.896, over the 0.729 threshold -- so cacti are never side by side)
//   the block below is cactus or BlockTags.SAND
//   the block above is not liquid
static int canSurvive(World *w, int x, int y, int z)
{
    static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; d++) {
        int nx = x + DX[d], nz = z + DZ[d];
        int ns = colSurf(w, nx, nz, NULL);
        if (y <= ns) return 0;                          // terrain beside it
        if (y <= ns + colCact(w, nx, nz)) return 0;     // cactus beside it
    }
    // "...&& !levelReader.getBlockState(blockPos.above()).liquid()"
    if (isWater(w, x, y + 1, z)) return 0;
    int sand = 0;
    int s = colSurf(w, x, z, &sand);
    int c = colCact(w, x, z);
    if (y == s + 1) return sand;                        // standing on the ground
    if (y > s + 1 && y <= s + c + 1) return 1;          // standing on cactus
    return 0;                                           // floating, or buried
}

static void place(World *w, int x, int y, int z, int n)
{
    int i = widx(w, x, z);
    if (i < 0) return;
    int s = w->surf[i];
    // Placements are always contiguous from the surface up: a cactus needs sand
    // or cactus underneath, so there is never a gap to represent.
    int top = y + n - 1 - s;
    if (top > w->cact[i]) w->cact[i] = (short)top;
}

// ---------------------------------------------------------------- one patch
//
// patch_cactus_desert, in the order the game consumes randomness. Every branch
// that does NOT consume is marked, because those are what keep the stream in
// step -- a try rejected by the block predicate costs nothing, a try that places
// costs two draws.
static void patch(World *w, int cx, int cz, int index, int rarity)
{
    uint64_t pop = getPopulationSeed(w->mc, w->seed, cx << 4, cz << 4);
    // 1.18+ decoration runs on Xoroshiro, not java.util.Random:
    //   new WorldgenRandom(new XoroshiroRandomSource(...))
    // in ChunkGenerator.applyBiomeDecoration. Building this on the legacy RNG
    // produced cacti that looked entirely plausible and were in the wrong chunk.
    CREATE_RANDOM_SOURCE(rnd, w->mc <= MC_1_17);
    rnd.setSeed(rnd.state, pop + index + 10000ULL * CACTUS_STEP);

    // rarity_filter: nextFloat() < 1/chance. Always drawn, even when it fails.
    if (!(rnd.nextFloat(rnd.state) < 1.0f / (float)rarity)) return;

    // in_square: x first, then z.
    int ox = (cx << 4) + rnd.nextInt(rnd.state, 16);
    int oz = (cz << 4) + rnd.nextInt(rnd.state, 16);
    if (cactusTrace)
        fprintf(stderr, "chunk (%d,%d) idx %d: rarity passed, origin (%d,?,%d)\n",
                cx, cz, index, ox, oz);

    // heightmap MOTION_BLOCKING: the first block that neither blocks motion nor
    // holds fluid. Water counts, so a submerged column reports sea level rather
    // than its seabed.
    int osurf = colSurf(w, ox, oz, NULL);
    int oy = (osurf > SEA_TOP ? osurf : SEA_TOP) + 1;
    if (oy < -60) return;
    if (cactusTrace) fprintf(stderr, "  origin y = %d (heightmap)\n", oy);

    // biome filter: does the biome AT THE ORIGIN carry this feature? No RNG.
    int b = getBiomeAt(w->g, 4, ox >> 2, oy >> 2, oz >> 2);
    if (index == CACTUS_INDEX_DESERT) {
        if (b != desert) return;
    } else {
        if (b != badlands && b != eroded_badlands && b != wooded_badlands) return;
    }

    const int j = CACTUS_XZ_SPREAD + 1;   // 8
    const int k = CACTUS_Y_SPREAD + 1;    // 4
    for (int t = 0; t < CACTUS_TRIES; t++) {
        // setWithOffset's arguments evaluate left to right, and each is
        // nextInt(n) - nextInt(n): six draws, in this order.
        int dx = rnd.nextInt(rnd.state, j) - rnd.nextInt(rnd.state, j);
        int dy = rnd.nextInt(rnd.state, k) - rnd.nextInt(rnd.state, k);
        int dz = rnd.nextInt(rnd.state, j) - rnd.nextInt(rnd.state, j);
        int x = ox + dx, y = oy + dy, z = oz + dz;

        // block_predicate_filter: air AND a cactus would survive. Costs nothing,
        // and skips the height draw entirely when it fails.
        int air = isAir(w, x, y, z), sur = canSurvive(w, x, y, z);
        if (cactusTrace) {
            int sd = 0; int sy = colSurf(w, x, z, &sd);
            fprintf(stderr, "  try %2d -> (%d,%d,%d)  surf=%d sand=%d cact=%d "
                    "air=%d survive=%d%s\n", t, x, y, z, sy, sd,
                    colCact(w, x, z), air, sur,
                    (air && sur) ? "  PLACE" : "  skip (no draws)");
        }
        if (!air || !sur) continue;

        // biased_to_bottom(1,3) == 1 + nextInt(nextInt(3) + 1): TWO draws.
        int inner = rnd.nextInt(rnd.state, 3);
        int want = 1 + rnd.nextInt(rnd.state, inner + 1);

        // BlockColumnFeature tests upward from y+1 and truncates at the first
        // blocked position, and the surviving height is the number of tests
        // that passed. So a column of n needs n free blocks ABOVE its base.
        int n = 0;
        while (n < want && isAir(w, x, y + 1 + n, z)) n++;
        if (cactusTrace)
            fprintf(stderr, "       height draw: want %d, fits %d\n", want, n);
        if (n > 0) place(w, x, y, z, n);
    }
}

// ---------------------------------------------------------------- region
int cactusRegion(Generator *g, int mc, uint32_t gflags, uint64_t worldSeed,
                 int cx0, int cz0, int cx1, int cz1, Cactus *out, int maxOut)
{
    if (mc < MC_1_18) return -1;          // no block-level terrain before this
    if (!terrainFor(mc, worldSeed, gflags)) return -1;

    int scx0 = cx0 - REACH_CHUNKS, scx1 = cx1 + REACH_CHUNKS;
    int scz0 = cz0 - REACH_CHUNKS, scz1 = cz1 + REACH_CHUNKS;

    World w;
    memset(&w, 0, sizeof w);
    w.x0 = scx0 << 4; w.z0 = scz0 << 4;
    w.w = (scx1 - scx0 + 1) << 4;
    w.h = (scz1 - scz0 + 1) << 4;
    size_t n = (size_t)w.w * w.h;
    w.surf  = malloc(n * sizeof *w.surf);
    w.known = calloc(n, 1);
    w.sand  = calloc(n, 1);
    w.cact  = calloc(n, sizeof *w.cact);
    w.g = g; w.mc = mc; w.gflags = gflags; w.seed = worldSeed;
    if (!w.surf || !w.known || !w.sand || !w.cact) {
        free(w.surf); free(w.known); free(w.sand); free(w.cact);
        return -1;
    }

    // Chunk order decides who stacks on whom, and stacking is the whole story:
    // if the patch that would sit on top runs first it finds nothing to stand on
    // and places nothing. Row-major is the order this was verified in against a
    // real server; changing it is not cosmetic.
    for (int cz = scz0; cz <= scz1; cz++)
        for (int cx = scx0; cx <= scx1; cx++) {
            patch(&w, cx, cz, CACTUS_INDEX_DESERT,   CACTUS_RARITY_DESERT);
            patch(&w, cx, cz, CACTUS_INDEX_BADLANDS, CACTUS_RARITY_BADLANDS);
        }

    int found = 0;
    for (int z = cz0 << 4; z < ((cz1 + 1) << 4); z++)
        for (int x = cx0 << 4; x < ((cx1 + 1) << 4); x++) {
            int i = widx(&w, x, z);
            if (i < 0 || w.cact[i] <= 0) continue;
            if (out && found < maxOut) {
                out[found].x = x; out[found].z = z;
                out[found].baseY = w.surf[i] + 1;
                out[found].height = w.cact[i];
            }
            found++;
        }

    free(w.surf); free(w.known); free(w.sand); free(w.cact);
    return found;
}

int cactusTallest(Generator *g, int mc, uint32_t gflags, uint64_t worldSeed,
                  int x, int z, int radius, Cactus *where)
{
    int cx0 = (x - radius) >> 4, cx1 = (x + radius) >> 4;
    int cz0 = (z - radius) >> 4, cz1 = (z + radius) >> 4;

    int cap = 4096;
    Cactus *buf = malloc(cap * sizeof *buf);
    if (!buf) return 0;
    int n = cactusRegion(g, mc, gflags, worldSeed, cx0, cz0, cx1, cz1, buf, cap);
    int best = 0;
    if (n > 0) {
        if (n > cap) n = cap;
        int64_t lim = (int64_t)radius * radius;
        for (int i = 0; i < n; i++) {
            int64_t dx = buf[i].x - x, dz = buf[i].z - z;
            if (dx * dx + dz * dz > lim) continue;      // the disc, not the box
            if (buf[i].height > best) {
                best = buf[i].height;
                if (where) *where = buf[i];
            }
        }
    }
    free(buf);
    return best;
}
