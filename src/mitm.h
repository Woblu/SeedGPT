#ifndef SC_MITM_H
#define SC_MITM_H

#include "finders.h"
#include <stdint.h>

// Meet-in-the-middle over structure placement -- see docs/meet-in-the-middle.md.
//
// src/invert.c solves ONE structure's two constraints and tests the rest. This
// solves all of them at once. Write the 48-bit seed as two 24-bit halves; the
// low half determines, on its own, whether ANY high half can work. That test is
// a couple of multiplies, so 2^24 low halves are sieved in milliseconds and only
// the survivors ever meet a high half.
//
// A query is n structures, each pinned to a SET of allowed chunk offsets per
// axis. Sets rather than single values because the interesting targets ("four
// huts within 15 chunks") are boxes, not points -- and because a superset of
// the real condition is safe: every emitted seed is checked concretely before
// it leaves.
//
// THREE PLACEMENT FAMILIES ARE HANDLED, which between them cover every
// region-based structure in the game:
//
//   FEATURE, remainder range   ox = (s1 >> 17) % r          huts, villages, ...
//     s1>>17 is s1H*128 + (s1L>>17), so with g = gcd(128, r) the high half is
//     pinned to one residue mod r/g -- and if the low half lands where no
//     residue works, no high half can save it.
//
//   FEATURE, power-of-two range   ox = (r * (s1 >> 17)) >> 31    ancient city
//     Java scales instead of taking a remainder. For r = 2^k this reduces
//     EXACTLY to ox = s1H >> (24-k): the low half contributes at most 127 to a
//     value shifted right by 24-k+7, so it can never carry. The constraint is a
//     prefix of the high half rather than a residue -- the low-half sieve gets
//     nothing, and the bucket join gets everything.
//
//   LARGE, two draws averaged   ox = ((d1 + d2) >> 1)      monument, mansion
//     Four draws per structure instead of two. The pair constraint couples the
//     draws, so the sieve can only use each draw's marginal (sound, weaker).
//
// Plus the post-placement rejection rolls (outpost, bastion) and end city's
// distance rule, which are concrete functions of the 48-bit seed and are simply
// tested. Mineshafts, buried treasure, strongholds and the decorator features
// (geodes, wells) are NOT here: they are per-chunk or ring-based or Xoroshiro,
// i.e. different algorithms rather than the same one with a twist.

#define MITM_MAXN    8
#define MITM_MAXDRAW 4

enum { MITM_FEATURE = 0, MITM_LARGE = 1 };
enum { MITM_ROLL_NONE = 0, MITM_ROLL_OUTPOST, MITM_ROLL_BASTION };

typedef struct {
    int      mc, st, n;
    int      regionSize;
    uint32_t range;
    int      family;                    // MITM_FEATURE | MITM_LARGE
    int      pow2;                      // scaling placement instead of remainder
    uint32_t pow2sh;                    // ox = s1H >> pow2sh
    int      roll;                      // post-placement rejection roll
    int      endCity;                   // also enforce the 1008-block origin gap
    int      ndraw;                     // draws per structure: 2 or 4
    int      regX[MITM_MAXN], regZ[MITM_MAXN];
    uint64_t off[MITM_MAXN];            // regX*A + regZ*B + salt   (mod 2^48)
    uint64_t okx[MITM_MAXN];            // bit o set == chunk offset o allowed
    uint64_t okz[MITM_MAXN];

    // ---- derived (filled by mitmQueryInit) ----
    uint64_t D[MITM_MAXN];              // off[j] - off[0]: structure 0 is the split point
    uint32_t g;                         // gcd(128, r)
    uint32_t m;                         // r/g -- the modulus the high half is pinned to
    uint32_t tinv;                      // (128/g)^-1 mod m
    uint32_t tw24;                      // 2^24 mod m
    uint64_t fmagic;                    // multiply-shift divisor for %r; 0 = use %
    // Per draw slot, which offsets that draw may take. For a feature that is
    // just okx/okz; for a large structure it is the marginal of the averaged
    // pair, which is a relaxation -- the exact test still runs per candidate.
    uint64_t dmask[MITM_MAXN][MITM_MAXDRAW];
    // eTab[j][d][a] = which residues of the draw's high half keep it on an
    // allowed offset, given the low half contributed a = sL>>17. Zero means the
    // low half is dead whatever the high half does. Unused when pow2.
    uint64_t eTab[MITM_MAXN][MITM_MAXDRAW][128];
} MitmQuery;

typedef int (*MitmCb)(uint64_t seed48, void *arg);

typedef struct {
    // ---- in ----
    int      join;                      // 0 = nested sweep, 1 = hash join
    int      threads;                   // 0 = one per core
    uint64_t limit;                     // stop after this many hits; 0 = all
    int      window;                    // 1 = only seeds in [wsLo, wsHi)
    uint64_t wsLo, wsHi;
    int      quiet;
    // ---- out ----
    uint64_t survivors;                 // low halves that passed the sieve
    uint64_t candidates;                // (low, high) pairs concretely tested
    uint64_t hits;
    uint32_t patterns;                  // distinct carry patterns seen
    double   lowSec, sweepSec;
} MitmRun;

// Can this structure/version be solved for at all?
int mitmSupported(int mc, int structType);

// Build a query. `okx`/`okz` are bitmasks over chunk offsets; NULL means "any".
// Returns 0 if this structure/version cannot be handled.
int mitmQueryInit(MitmQuery *q, int mc, int st, int n,
                  const int *regX, const int *regZ,
                  const uint64_t *okx, const uint64_t *okz);

// Four structures around the (1,1) region corner, each axis relaxed to the box
// "all four within `spread` chunks". A superset of the real Euclidean cluster,
// which is what the caller then filters on.
int mitmQueryQuad(MitmQuery *q, int mc, int st, int spreadChunks);

// Same corner, but query.c's rule: some member has the other three within
// `spreadBlocks`. Looser than pairwise, and measured in blocks. That rule is a
// disjunction over which member is the anchor, so it is built ONE ANCHOR AT A
// TIME -- `anchor` is 0..3, and a caller wanting the whole rule runs all four
// and unions the results. Merging the four offset sets first would leave the
// query too loose for the low-half sieve to reject anything.
int mitmQueryCluster(MitmQuery *q, int mc, int st, int spreadBlocks, int anchor);

// Is the 2x2 corner the only arrangement that can satisfy that rule? If not,
// solving for the corner would miss seeds and the caller must scan instead.
int mitmCornerIsOnlyShape(int mc, int st, int spreadBlocks);

// The tightest spread at which a 4-cluster is geometrically possible: the two
// diagonal members are at least regionSize-(range-1) chunks apart on BOTH axes.
int mitmMinSpread(int mc, int st);

uint64_t mitmRun(const MitmQuery *q, MitmRun *run, MitmCb cb, void *arg);

// Would the enumeration reach this seed? Runs the same sieve and the same
// concrete test the sweep runs -- not a restatement of them. This is what the
// completeness harness calls, so it must share the code, or it proves nothing
// about the code that actually runs.
int mitmWouldReach(const MitmQuery *q, uint64_t seed48);

// Does this seed satisfy the query, by the plain forward formula?
int mitmMatches(const MitmQuery *q, uint64_t seed48);

// The two halves on their own, so a test can attack the sieve directly: take a
// low half the sieve threw away and try every one of the 2^24 high halves
// against it. If any of them lands, the sieve is dropping seeds.
int      mitmSieve(const MitmQuery *q, uint32_t yL);
uint64_t mitmSeedOf(const MitmQuery *q, uint32_t yH, uint32_t yL);

#endif
