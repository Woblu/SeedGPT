#ifndef SC_SOLVE_H
#define SC_SOLVE_H

#include "query.h"
#include <stdint.h>

// Solving instead of scanning.
//
// The search normally walks structure seeds 0,1,2,... and asks each one whether
// it happens to satisfy the query. For a TIGHT CLUSTER -- four structures all
// within a few hundred blocks -- that is a hit rate around 1 in 10^11, so the
// scan spends essentially all of its time on seeds that were never going to
// work. src/mitm.c can enumerate exactly the seeds that do.
//
// This module decides whether a query has such a condition and, if so, produces
// candidate structure seeds for it. The candidates are only PROPOSALS: the
// normal pass-1/pass-2 pipeline still evaluates every one of them, so a wrong
// proposal is rejected like any other seed. What the solver changes is which
// seeds get proposed -- not what counts as a hit.
//
// It refuses whenever solving could not stand in for the scan. In particular a
// loose `spread` lets four structures cluster in arrangements other than a 2x2
// region corner, and the solver only enumerates the corner -- so past that
// width it declines and the scan runs unchanged.

// Can a condition be solved rather than scanned? Returns its index, or -1.
// `why` gets a one-line explanation either way, for the plan output.
int solvePick(const Query *q, char *why, size_t whylen);

// Fill `out` with up to `want` structure seeds whose geometry satisfies
// condition `k`, each already confirmed against cubiomes' own placement.
// Returns how many were written. `threads` of 0 means one per core.
// `maxSeconds` bounds the build. Solving is not always the better tool: a loose
// cluster is common enough that the plain scan finds it sooner than the solver
// can enumerate, because a loose target leaves the low-half sieve nothing to
// reject. Rather than predict which case a query is in -- the honest estimate
// needs the real cluster rate, not the relaxed one the solver bounds -- it is
// measured: build for this long, and the caller decides from the yield.
// `*timedOut` distinguishes the two ways of returning nothing, which mean
// opposite things: a completed search that found none PROVES there are none,
// while a search that ran out of time proves only that this target is not tight
// enough for solving to beat scanning.
uint64_t solveFill(const Query *q, int k, uint64_t want, uint64_t *out, int threads,
                   double maxSeconds, int *timedOut);

#endif
