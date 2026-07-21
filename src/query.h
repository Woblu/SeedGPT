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
} CondType;

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

typedef struct {
    int  mc;
    Cond cond[MAX_COND];
    int  n;

    // --- plan output ---
    int  geom[MAX_COND]; int ngeom;   // pass 1: 48-bit, dependency order
    int  viab[MAX_COND]; int nviab;   // pass 2: 64-bit, cheapest first
    int  dims[3];        int ndims;   // distinct dimensions pass 2 must visit
    double est_cost_ns;               // estimated cost per candidate seed
} Query;

// Parse a JSON query. Returns 0 on success, non-zero on error (msg written).
int  queryParse(Query *q, const char *json, char *err, size_t errlen);

// Resolve parents, topologically order geometry, cost-order viability.
// Returns 0 on success (cycles / unknown parents are errors).
int  queryPlan(Query *q, char *err, size_t errlen);

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
} Match;

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

// Vocabulary accessors -- the NL layer reads these instead of hardcoding names.
int         queryStructureCount(void);
const char *queryStructureName(int i);
int         queryStructureType(int i);
// 0 if the engine performs no real placement check for this structure, so a
// reported position may not exist in game. See query.c for the measurement.
int         queryStructureVerified(int i);
