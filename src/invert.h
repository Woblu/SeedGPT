#ifndef SC_INVERT_H
#define SC_INVERT_H

#include "finders.h"
#include <stdint.h>

// Structure placement, run backwards.
//
// Every search in this project so far ENUMERATES: walk structure seeds, ask
// each one where it puts a village, keep the ones that answer well. That is
// fine when the target is common and hopeless when it is not -- our own README
// admits four huts within 160 blocks is "a many-tens-of-millions-of-seeds
// search".
//
// But placement is not a hash. For a region-based structure it is three
// invertible steps:
//
//     s  = worldSeed + regX*341873128712 + regZ*132897987541 + salt
//     s1 = ((s ^ K) * K + b) mod 2^48      ox = (s1 >> 17) % range
//     s2 = (s1 * K + b)      mod 2^48      oz = (s2 >> 17) % range
//
// so the question can be asked the other way round: which seeds put the
// structure HERE? Fixing ox pins (s1 >> 17) to one residue class mod `range`,
// which is 1 seed in `range` -- and every one of those is a hit rather than a
// candidate. Fixing oz as well costs nothing extra to test and leaves 1 in
// range^2. For a 24-chunk range that is 576 times less space to walk, and the
// saving multiplies with every additional structure a query pins down, which is
// exactly the case that is currently unaffordable.
//
// This is the generalisable half of mjtb49's meet-in-the-middle gist: the
// method is not about ruined portals, it is about any placement driven by
// Java's LCG.

// Enumerate world seeds (48-bit) that place `structType` in region
// (regX, regZ) at chunk offset (ox, oz) within that region.
//
// `cb` is called once per solution and returns non-zero to stop early. Returns
// the number of solutions emitted.
//
// The enumeration is DENSE: it visits only seeds that already satisfy the x
// constraint, so no candidate is ever tested and rejected on x.
typedef int (*InvertCb)(uint64_t worldSeed, void *arg);

uint64_t invertCount(int mc, int structType, int regX, int regZ,
                     int ox, int oz, uint64_t limit, InvertCb cb, void *arg);

// The two halves of the LCG step, exposed so a test can round-trip them.
uint64_t lcgForward(uint64_t s);    // (s*K + b) mod 2^48
uint64_t lcgBack(uint64_t s);       // the inverse of the above

// Does this structure type use the plain "feature" placement this can invert?
// Monuments and mansions average two draws per axis, outposts add a rejection
// roll, mineshafts are per-chunk -- those need their own handling, and
// pretending otherwise would silently produce wrong seeds.
int invertSupported(int mc, int structType);

#endif
