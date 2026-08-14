// Typed condition tree + cost-based query planner.
//
// The engine (cubiomes) evaluates conditions in whatever order it is handed.
// It does NO cost-based sorting -- that is the caller's job, and getting it
// wrong costs ~3650x (a structure geometry check is ~9 ns, a biome check
// ~31 us). This header is that job.
//
// Central idea: a structure condition is NOT one check. It splits into
//   - a GEOMETRY task  (pure 48-bit LCG, no biome generator, ~9 ns)
//   - a VIABILITY task (needs the full 64-bit seed + biome generator, ~42 us)
// The planner emits those into separate passes so that a seed can be rejected
// on cheap math long before anything expensive is initialised.
#pragma once

#include "finders.h"
#include "generator.h"
#include <stdio.h>
#include <stddef.h>

#define MAX_COND 16
#define ID_LEN   24

// A condition is measured from another condition's matched position, from the
// world ORIGIN (0,0), or from the actual world SPAWN. Those last two are not
// the same place: measured over 400 seeds, the median spawn is 22 blocks from
// origin but p90 is 520 and the max nearly 1000 -- 47% of seeds spawn further
// than 35 blocks out. Conflating them silently produces results that look
// right on paper and are wrong in game.
#define PARENT_ORIGIN (-1)
#define PARENT_SPAWN  (-2)

// Spawn is a 64-bit, biome-dependent quantity, so pass 1 cannot know it. For
// spawn-relative conditions pass 1 filters against origin widened by this
// margin (a conservative over-admit), and pass 2 recomputes the match around
// the true spawn exactly. Sized past the observed maximum.
#define SPAWN_MARGIN 1100

typedef enum {
    CT_STRUCTURE,   // structure of a type within `within` blocks of parent
    CT_BIOME,       // biome present within `within` blocks of parent
    CT_EYES,        // the first stronghold's end portal has >= N eyes
    CT_LOOT,        // a structure's chests hold >= N of an item
    CT_ORE,         // >= N ore blocks of a material within `within` (all depths)
    CT_SLIME,       // >= N slime chunks within `within` blocks of parent
    CT_BIOME_AREA,  // biome covers >= N% of the disc of radius `within`
    CT_HEIGHT,      // APPROXIMATE surface height peaks >= N within `within`
    CT_ISLAND,      // land at the reference, ocean covering >= N% of the disc
    CT_OVERLAP,     // two structures whose footprint boxes intersect
    CT_CACTUS,      // a cactus at least N blocks tall within `within`
} CondType;

// What a leaderboard search ranks by. AUTO picks the condition's natural
// measurement (a height condition ranks by peak, an ore condition by count),
// which is what almost every query wants; the rest are explicit overrides.
typedef enum {
    RANK_AUTO,
    RANK_HEIGHT,    // CT_HEIGHT: peak surface Y
    RANK_RELIEF,    // CT_HEIGHT: peak - valley
    RANK_VEIN,      // CT_ORE:    largest single connected vein
    RANK_COUNT,     // CT_ORE/CT_SLIME/CT_STRUCTURE/CT_LOOT: the count
    RANK_PCT,       // CT_BIOME_AREA/CT_ISLAND: percent of the disc
    RANK_SIZE,      // CT_STRUCTURE (geode): variant size
    RANK_AREA,      // CT_OVERLAP: intersection area in blocks^2
    RANK_TALL,      // CT_CACTUS: cactus height in blocks
} RankBy;

// End portal frames: 12, each independently 10% likely to hold an eye. That
// makes high counts brutally rare, and the search rate is ~98 seeds/s because
// locating a stronghold costs ~10 ms (biome checks dominate). Measured:
//
//   eyes  probability   seeds needed   time on this machine
//     6    4.9e-04            2,036    21 s
//     7    4.7e-05           21,382    4 min
//     8    3.2e-06          307,910    52 min
//     9    1.6e-07        6,235,191    18 hours
//    10    5.3e-09      187,055,742    22 days
//    11    1.1e-10    9,259,259,259    3 years
//    12    1.0e-12  999,999,999,999    323 years
//
// So 6-8 is a normal search, 9-10 is an overnight-to-fortnight commitment, and
// 11-12 is a distributed-compute problem, not a single-machine one.
#define EYE_FRAMES 12

typedef struct {
    char     id[ID_LEN];
    CondType type;
    int      structType;    // CT_STRUCTURE
    int      biomeId;       // CT_BIOME
    int      within;        // radius, blocks
    char     ofId[ID_LEN];  // parent condition id, or "spawn"/"origin"
    int      parent;        // resolved index, or PARENT_ORIGIN / PARENT_SPAWN
    int      dim;           // DIM_OVERWORLD / DIM_NETHER / DIM_END, inferred
    int      scanStep;      // CT_BIOME: sample spacing in blocks (see below)
    int      eyesMin;       // CT_EYES: minimum filled frames required
    int      lootMin;       // CT_LOOT: minimum item count
    char     lootItem[48];  // CT_LOOT: item id, e.g. "minecraft:diamond"
    int      surfaceOnly;   // CT_STRUCTURE: reject the buried variant (ruined
                            // portals only) -- see getVariant().underground
    int      reqAbandoned;  // CT_STRUCTURE: require zombie village
    int      reqBasement;   // CT_STRUCTURE: require an igloo with a basement
    int      reqGiant;      // CT_STRUCTURE: require the giant ruined portal
    int      reqExposed;    // CT_STRUCTURE (buried_treasure): chest on dry land
                            // at/above sea level (exposed), not submerged
    int      reqShip;       // CT_STRUCTURE (end_city): require an end ship (elytra)
    int      floatVoid;     // CT_STRUCTURE: require a FLOATING island under it
    int      floatRing;     // how far out the air gap is checked (blocks)
    int      floatNeed;     // how many of 8 directions must be open
    int      caveBelow;     // CT_STRUCTURE: require an open void at least this
                            // many blocks tall UNDER the structure -- a village
                            // perched over a cavern. Real block terrain, 1.18+.
    int      structType2;   // CT_OVERLAP: the second structure type
    int      cactusMin;     // CT_CACTUS: minimum cactus height in blocks. One
                            // placement is 1-3, so anything above ~6 needs
                            // several patches stacking on one column.
    int      overlapPad;    // CT_OVERLAP: slack in blocks. 0 = the two footprint
                            // boxes must genuinely intersect; N = within N blocks
                            // of each other, for "practically on top of".
    int      geodeSize;     // CT_STRUCTURE (geode): min getVariant().size
    int      reqCracked;    // CT_STRUCTURE (geode): require the cracked variant
    int      oreMat;        // CT_ORE: index into the ore material table
    int      oreMin;        // CT_ORE: minimum ore-block count in range
    int      veinMin;       // CT_ORE: min blocks in ONE connected vein (a single
                            // mineable blob), not the scattered total
    int      oreExposed;    // CT_ORE: count only blocks open to air -- a cave
                            // wall you can see the ore in. 1.18+ overworld, SLOW.
    int      slimeMin;      // CT_SLIME: minimum slime chunks in range
    int      areaPct;       // CT_BIOME_AREA: min % of the disc that is biomeId
    int      structMin;     // CT_STRUCTURE: min instances in range (1 = single)
    int      heightMin;     // CT_HEIGHT: minimum approximate peak height in disc
    int      reliefMin;     // CT_HEIGHT: min (peak - valley) height spread in disc.
                            // A large local relief means steep terrain -- a
                            // structure on a cliff edge or a mountainside.
    int      exactTerrain;  // CT_HEIGHT: if set, use cubiomes' real block-level
                            // terrain (generateColumn) instead of the smoothed
                            // 1:4 approximation -- the only way to see a sharp
                            // drop under the footprint. 1.18+, SLOW.
    int      spread;        // CT_STRUCTURE cluster: if >0, the instances must
                            // fit within this radius of a common member (a TIGHT
                            // cluster anywhere in `within`), not just within
                            // `within` of the reference. 0 = anchored cluster.
    int      isParent;      // some other condition is measured from this one
} Cond;

// Biome scan precision. A biome condition samples points across the disc; the
// spacing trades recall against cost.
//
// MEASURED (tools/biomerecall.c), MC 1.21, jungle, vs an exhaustive
// quart-resolution scan. Recall is NOT a fixed property -- it falls with the
// radius, because fewer sample points land inside a smaller disc:
//
//            radius 400            radius 300
//   step   recall   cost/seed    recall   cost/seed
//     64    90.4%      424 us     78.6%      260 us   "fast"
//     16    96.2%     5451 us     89.3%     3239 us   "fine"  (default)
//      4   100.0%   105821 us    100.0%    58595 us   "exact" (200x+)
//
// So treat these as indicative, not guarantees. Run biomerecall for the radius
// and biome you actually care about before trusting a recall number.
//
// Every setting errs in the same direction: a coarse scan yields false
// NEGATIVES (missed seeds) only -- a reported seed is always correct.
#define SCAN_FAST   64
#define SCAN_FINE   16
#define SCAN_EXACT   4

// Which conditions the climate pre-filter can reject on temperature alone, and
// the window each one needs. Filled by queryPlan; see climate.h for what it
// buys and why rejecting on it loses nothing.
typedef struct {
    int     n;                    // 0 = nothing to gate, filter idle
    char    use[MAX_COND];        // is this condition temperature-gated?
    int64_t lo[MAX_COND];         // inclusive temperature limits, 1e4-scaled,
    int64_t hi[MAX_COND];         // in the units climateToBiome is handed
} ClimatePlan;

typedef struct {
    int  mc;
    Cond cond[MAX_COND];
    int  n;

    // World type. "large_biomes": true is the Large Biomes world preset -- the
    // same generator with the biome scale multiplied, so structures, ores and
    // terrain all still work, they just land in a differently shaped world. It
    // is a property of the WORLD, not of any one condition, so it lives here
    // and every generator this query creates has to be told about it.
    int  largeBiomes;

    // --- leaderboard ("what is the biggest X anywhere?") ---
    //
    // A normal query is a FILTER: every condition is pass/fail and the search
    // stops once it has enough hits. A ranked query is an OPTIMISER: it scans a
    // fixed budget of seeds and keeps the best `rankTop` by one condition's
    // measurement. That is the difference between "a seed with a tall mountain"
    // and "the tallest mountain in 50 million seeds", and records are the
    // latter. Set the ranked condition's threshold LOW -- it still filters, and
    // a tight threshold just starves the leaderboard.
    char rankOf[ID_LEN];              // condition id to rank by ("" = filter)
    int  rankBy;                      // RankBy; RANK_AUTO = the natural metric
    int  rankTop;                     // how many to keep (0 = not ranking)
    int  rankIdx;                     // resolved index, -1 until queryPlan

    // --- plan output ---
    int  geom[MAX_COND]; int ngeom;   // pass 1: 48-bit, dependency order
    int  viab[MAX_COND]; int nviab;   // pass 2: 64-bit, cheapest first
    int  dims[3];        int ndims;   // distinct dimensions pass 2 must visit
    ClimatePlan clim;                 // pass 1.5: reject on temperature alone
    double est_cost_ns;               // estimated cost per candidate seed
} Query;

// Parse a JSON query. Returns 0 on success, non-zero on error (msg written).
int  queryParse(Query *q, const char *json, char *err, size_t errlen);

// Resolve parents, topologically order geometry, cost-order viability.
// Returns 0 on success (cycles / unknown parents are errors).
int  queryPlan(Query *q, char *err, size_t errlen);

// Can any seed satisfy this at all? Returns non-zero and explains when the
// answer is provably no -- an outpost inside a village, two of the same
// structure closer than the placement grid allows. Rarity is NOT judged here:
// "12-eye portal" is possible and `--explain` estimates it honestly. Called by
// queryPlan, so a search refuses up front instead of running forever.
int  queryFeasible(const Query *q, char *err, size_t errlen);

// Human-readable plan, so the ordering is auditable rather than implicit.
void queryPrintPlan(const Query *q, FILE *f);

// --- evaluation ---

// Matched positions, one slot per condition, filled by the passes.
typedef struct {
    Pos pos[MAX_COND];
    Pos spawn;       // world spawn, filled by pass 2 iff the query needs it
    int haveSpawn;   // 0 until pass 2 resolves it
    Pos stronghold;  // first stronghold, filled iff a CT_EYES condition ran
    int eyes;        // its end portal's filled frame count
    int haveEyes;
    int lootCount[MAX_COND];  // CT_LOOT: item count found at the winning chest
    int oreCount[MAX_COND];   // CT_ORE: ore-block count found in range
    int slimeCount[MAX_COND]; // CT_SLIME: slime-chunk count found in range
    int areaCells[MAX_COND];  // CT_BIOME_AREA: matching sample cells found
    int areaTotal[MAX_COND];  // CT_BIOME_AREA: total sample cells in the disc
    int structCount[MAX_COND];// CT_STRUCTURE: viable instances found (cluster)
    int peakHeight[MAX_COND]; // CT_HEIGHT: highest approximate surface Y in disc
    int peakDrop[MAX_COND];   // CT_HEIGHT: peak - valley (local relief) in disc
    int veinMax[MAX_COND];    // CT_ORE: largest single connected vein found
    int geodeSize[MAX_COND];  // CT_STRUCTURE (geode): the matched geode's size
    int caveHeight[MAX_COND]; // CT_STRUCTURE: tallest void found under it
    int floatCap[MAX_COND];   // CT_STRUCTURE: world Y of the island's underside
    Pos floatAt[MAX_COND];    // where the island IS -- up to 16 blocks from
                              // the anchor, because five offsets are sampled
    Pos partner[MAX_COND];    // CT_OVERLAP: position of the SECOND structure
    int overlapArea[MAX_COND];// CT_OVERLAP: footprint intersection, blocks^2
    int cactusTall[MAX_COND]; // CT_CACTUS: tallest cactus found, in blocks
    int cactusBase[MAX_COND]; // CT_CACTUS: world Y its lowest block stands on
} Match;

// Leaderboard scoring. Returns the ranked condition's measurement for this
// match (higher is better), or INT_MIN when the query is a plain filter search.
// The unit is whatever queryScoreLabel names.
int         queryScore(const Query *q, const Match *m);
const char *queryScoreLabel(const Query *q);

// Pass 1: geometry only. No Generator required. Returns 1 if all geometry
// conditions are satisfiable for this 48-bit structure seed.
int  queryStage1(const Query *q, uint64_t s48, Match *m);

// Pass 2: biome-dependent checks for one full 64-bit world seed. Applies the
// seed once per dimension the query actually touches (applySeed is the
// dominant per-seed cost, so a single-dimension query pays for exactly one) --
// the caller does NOT call applySeed itself.
#ifndef LOOTCACHE_TYPEDEF
#define LOOTCACHE_TYPEDEF
typedef struct LootCache LootCache;   // defined in loot.h
#endif
int  queryStage2(const Query *q, Generator *g, uint64_t worldSeed, Match *m, LootCache *lc);

const char *condDesc(const Query *q, int i, char *buf, size_t n);
const char *dimName(int dim);

// setupGenerator flags this query needs (LARGE_BIOMES, or none). Every caller
// that builds a Generator for a query must use this rather than passing 0, or
// it silently searches the default world while claiming to search another.
uint32_t    queryGenFlags(const Query *q);

// Vocabulary accessors -- the NL layer reads these instead of hardcoding names.
int         queryStructureCount(void);
const char *queryStructureName(int i);
int         queryStructureType(int i);
// 0 if the engine performs no real placement check for this structure, so a
// reported position may not exist in game. See query.c for the measurement.
int         queryStructureVerified(int i);
