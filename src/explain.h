// Selectivity estimation: sample a query's funnel before searching it.
#pragma once

#include "query.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
    uint64_t drawn;      // structure seeds sampled
    uint64_t pass1;      // survived geometry
    uint64_t upTried;    // upper-bit variants probed
    uint64_t upPassed;   // ...that were biome-viable
    double   seconds;
    double   p1;         // P(pass geometry)
    double   p2;         // P(pass biome | pass1, one upper variant)
    double   pFamily;    // P(some upper of 2^16 works | pass1)
    double   pHit;       // P(structure seed yields a hit)
    double   rate;       // sampled seeds/sec
} Estimate;

// What explainAny needs from one alternative. pHit is 0 when none was seen.
typedef struct {
    double p1;          // P(a structure seed passes geometry)
    double pHit;        // P(a structure seed yields a hit)
    double secPerSeed;  // wall clock per structure seed, 0 if never measured
} ExplainResult;

// Sample uniformly across 2^48 and report the predicted funnel + wall clock.
// `res` may be NULL.
void explainQuery(const Query *q, uint64_t samples, int nthreads, FILE *f, ExplainResult *res);

// The same answer for an "any" search (anyof.h): each alternative is sampled
// alone, then combined as the search really runs them, every seed paying for
// each alternative and any one of them being enough.
void explainAny(const Query *plans, int nplans, uint64_t samples, int nthreads, FILE *f);

// Compare uniform sampling against a sequential scan from 0, to check whether
// scanning low seeds (what the searcher actually does) is representative.
void explainCompare(const Query *q, uint64_t samples, int nthreads, FILE *f);

const char *humanTime(double seconds);
const char *humanRate(double perSec);
