#ifndef SC_CLIMATE_H
#define SC_CLIMATE_H

#include "query.h"
#include <stdint.h>

// The climate pre-filter.
//
// Finding out whether a point is jungle means sampling six climate noises and
// running a nearest-neighbour search in six dimensions. But a biome only exists
// inside a fixed box in that space, so ONE parameter is often enough to prove a
// point cannot hold it -- no amount of humidity or erosion puts a jungle
// somewhere freezing. Temperature costs about a third of the full sample, so
// checking it first and skipping the rest is a straight saving on every point
// that fails, which in a cold-biome search is most of them.
//
// WHERE IT RUNS MATTERS, and the first version got it wrong. Run as a gate
// before pass 2 it walked the whole search disc up front, spending milliseconds
// to save one 35us applySeed -- and spending them on seeds that a cheap
// structure check would have killed in microseconds anyway. Measured: 79% of
// seeds rejected, 10% faster. So it runs fused INTO the biome scan instead, one
// point at a time, right where the expensive lookup it replaces would happen.
// Nothing is walked twice and nothing is paid for early.
//
// The filter is EXACT, not a heuristic, and the distinction is the whole point.
// It samples through the same shift noise at the same coordinate and produces
// the identical int64 temperature climateToBiome would be handed, then compares
// against the biome's own published limits. An approximate filter here would
// silently drop records, which is worse than being slow.

// One caveat, and it is the only thing standing between this and a proof.
// climateToBiome picks the NEAREST biome in six-dimensional climate space, not
// the one whose box contains the point. So a biome can generate a little
// outside its own temperature box when every alternative is worse on some other
// axis. Each window is therefore widened by this many 1e4-scaled units.
// tools/climatecheck hunts for the largest real excursion and fails if it comes
// near this value; over 256k samples the worst was a single unit, so the margin
// is 512 times the measured worst case. Bands are thousands of units wide, so
// the filtering power given up is negligible.
#define CLIMATE_MARGIN 512

// Decide which conditions are temperature-gated, and the window each needs.
// Called once by queryPlan. Returns the number of gated conditions.
int climatePlan(const Query *q, ClimatePlan *cp);

// Could condition k's target biome occur at this quart coordinate? A 0 means
// provably not, so the caller may skip the full biome lookup and move on; a 1
// means the lookup still has to run. Ungated conditions always answer 1, at the
// cost of one array read.
//
// Keeps its noise in thread-local storage and re-seeds only when the world seed
// changes, so the seeding cost falls on the first gated point of a seed and
// never on seeds that die before reaching one.
int climateMayHold(const Query *q, uint64_t worldSeed, int k, int qx, int qz);

// Biome lookups this thread was asked for, and how many temperature alone
// disposed of. Reported by the search: an optimisation whose effect nobody can
// see is one nobody can tell has broken.
uint64_t climateGated(void);
uint64_t climateSkipped(void);

// The exact 1e4-scaled temperature the biome lookup would use at a quart
// coordinate. Exposed so a test can diff it against the full generator rather
// than take any of the above on trust.
int64_t climateTempAt(int mc, uint64_t worldSeed, int large, int qx, int qz);

#endif
