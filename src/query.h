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

typedef enum {
    CT_STRUCTURE,   // structure of a type within `within` blocks of parent
    CT_BIOME,       // biome present within `within` blocks of parent
} CondType;

typedef struct {
    char     id[ID_LEN];
    CondType type;
    int      structType;    // CT_STRUCTURE
    int      biomeId;       // CT_BIOME
    int      within;        // radius, blocks
    char     ofId[ID_LEN];  // parent condition id, or "spawn"/"origin"
    int      parent;        // resolved index; -1 == world origin
    int      dim;           // DIM_OVERWORLD / DIM_NETHER / DIM_END, inferred
} Cond;

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
typedef struct { Pos pos[MAX_COND]; } Match;

// Pass 1: geometry only. No Generator required. Returns 1 if all geometry
// conditions are satisfiable for this 48-bit structure seed.
int  queryStage1(const Query *q, uint64_t s48, Match *m);

// Pass 2: biome-dependent checks for one full 64-bit world seed. Applies the
// seed once per dimension the query actually touches (applySeed is the
// dominant per-seed cost, so a single-dimension query pays for exactly one) --
// the caller does NOT call applySeed itself.
int  queryStage2(const Query *q, Generator *g, uint64_t worldSeed, const Match *m);

const char *condDesc(const Query *q, int i, char *buf, size_t n);
const char *dimName(int dim);

// Vocabulary accessors -- the NL layer reads these instead of hardcoding names.
int         queryStructureCount(void);
const char *queryStructureName(int i);
int         queryStructureType(int i);
