// java.util.Random's LCG, forwards AND backwards.
//
// Milestone 1 of docs/reversal-roadmap.md. Every reversal technique the
// professional seedfinders use -- chunk-seed to world-seed, carver and
// decorator inversion, LattiCG's inequality solving -- is built from these:
// stepping the 48-bit state back as easily as forward, skipping n states in
// O(log n), and recovering the state that produced a given nextLong. They are
// checked against the real java.util.Random (tools/LcgCheck.java via
// tools/lcgcheck.c), not against themselves.
#pragma once

#include <stdint.h>

#define LCG_MULT     0x5DEECE66DULL
#define LCG_ADD      0xBULL
#define LCG_MASK     ((1ULL << 48) - 1)
#define LCG_MULT_INV 0xDFE05BCB1365ULL     // LCG_MULT^-1 mod 2^48

// new Random(seed) / setSeed(seed): the internal state, and back again.
static inline uint64_t lcgFromSeed(uint64_t seed)  { return (seed ^ LCG_MULT) & LCG_MASK; }
static inline uint64_t lcgToSeed(uint64_t state)   { return (state ^ LCG_MULT) & LCG_MASK; }

static inline uint64_t lcgStep(uint64_t s) { return (s * LCG_MULT + LCG_ADD) & LCG_MASK; }
static inline uint64_t lcgBack(uint64_t s) { return ((s - LCG_ADD) * LCG_MULT_INV) & LCG_MASK; }

// n steps at once (n may be negative: the period is exactly 2^48). The affine
// map s -> a*s + c composed with itself n times, by squaring.
static inline uint64_t lcgSkip(uint64_t s, int64_t n)
{
    uint64_t k = (uint64_t)n & LCG_MASK;              // -n == 2^48 - n steps
    uint64_t a = 1, c = 0;                            // accumulated map
    uint64_t pa = LCG_MULT, pc = LCG_ADD;             // map for 2^i steps
    while (k) {
        if (k & 1) { a = (a * pa) & LCG_MASK; c = (c * pa + pc) & LCG_MASK; }
        pc = (pc * (pa + 1)) & LCG_MASK;              // (pa,pc) composed with itself
        pa = (pa * pa) & LCG_MASK;
        k >>= 1;
    }
    return (s * a + c) & LCG_MASK;
}

static inline int32_t lcgNext(uint64_t *s, int bits)
{
    *s = lcgStep(*s);
    return (int32_t)(*s >> (48 - bits));
}
static inline int32_t lcgNextInt(uint64_t *s) { return lcgNext(s, 32); }

// nextInt(bound) exactly as Java does it, rejection loop included (cubiomes'
// own nextInt omits the loop; it differs about once in 2.7e8 calls).
static inline int32_t lcgNextIntBound(uint64_t *s, int32_t bound)
{
    if ((bound & -bound) == bound)
        return (int32_t)(((int64_t)bound * (int64_t)lcgNext(s, 31)) >> 31);
    int32_t bits, val;
    do {
        bits = lcgNext(s, 31);
        val = bits % bound;
    } while (bits - val + (bound - 1) < 0);
    return val;
}

static inline int64_t lcgNextLong(uint64_t *s)
{
    int64_t hi = lcgNext(s, 32);
    int64_t lo = lcgNext(s, 32);
    return (int64_t)((uint64_t)hi << 32) + lo;
}

// Every internal state from which nextLong() returns v. The high int fixes the
// top 32 bits of the state after the first call; its low 16 bits are tried
// (65536 candidates) against the second int. Usually one state, sometimes two,
// occasionally none (not every 64-bit value is a nextLong). Returns the count.
static inline int lcgReverseNextLong(int64_t v, uint64_t out[4])
{
    int32_t lo = (int32_t)(uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(((uint64_t)v - (uint64_t)(int64_t)lo) >> 32);
    int n = 0;
    for (uint64_t low = 0; low < 65536; low++) {
        uint64_t s1 = ((uint64_t)hi << 16) | low;
        if ((uint32_t)(lcgStep(s1) >> 16) == (uint32_t)lo && n < 4)
            out[n++] = lcgBack(s1);
    }
    return n;
}
