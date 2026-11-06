#include "popinvert.h"
#include "lcg.h"
#include "biomes.h"
#include <stdlib.h>
#include <string.h>

// `n / 2 * 2 + 1` (Java's long division truncates toward zero) is `n | 1`
// except for a negative odd n, where it is n + 2. Up to 1.12 that makes a's
// low bits depend on its sign, which only the full seed knows -- so the sign
// is guessed, the lift runs once per guess, and a finished seed is kept only
// when its real sign matches the guess (which also keeps the guesses from
// reporting the same seed twice). The guess only matters for an odd n.
static inline uint64_t oddify(uint64_t n, int legacy, int neg)
{
    uint64_t r = n | 1;
    if (legacy && neg && (n & 1)) r += 2;
    return r;
}

static int64_t legacyOdd(int64_t n) { return n / 2 * 2 + 1; }

uint64_t popForward(int mc, uint64_t worldSeed, int x, int z)
{
    uint64_t s = lcgFromSeed(worldSeed);
    int64_t a = lcgNextLong(&s), b = lcgNextLong(&s);
    if (mc >= MC_1_13) { a |= 1; b |= 1; }
    else               { a = legacyOdd(a); b = legacyOdd(b); }
    return ((uint64_t)(int64_t)x * (uint64_t)a + (uint64_t)(int64_t)z * (uint64_t)b) ^ worldSeed;
}

typedef struct {
    int mc, legacy, neg1, neg2, t;
    int64_t x, z;
    uint64_t pop;
    uint64_t *buf; int n, cap;
} Lift;

// With the seed's low L bits known (w), are the bits of pop they determine
// right? a and b are known mod 2^K, K = L - 16 -- at most 32, nextLong's low
// int; its high int needs the whole seed and is left to the final check. Both
// are odd, so K is never below 1: their bit 0 needs no seed bits at all. x*a
// and z*b are then known mod 2^(K+t), and the XOR with the seed mod 2^L.
static int consistent(const Lift *v, uint64_t w, int L)
{
    int K = L > 17 ? L - 16 : 1;
    int m = K + v->t;
    if (m > L) m = L;
    if (m == 0) return 1;
    uint64_t mk = (1ULL << m) - 1, kk = (1ULL << K) - 1;
    uint64_t s1 = lcgStep(lcgFromSeed(w)), s2 = lcgStep(s1);
    uint64_t s3 = lcgStep(s2), s4 = lcgStep(s3);
    // An even n is the same whatever its sign, so the "negative" guess would
    // repeat the "positive" one's work and find the same seeds: drop it as
    // soon as n's bit 0 is known.
    if (v->legacy && L >= 17) {
        if (v->neg1 && !((s2 >> 16) & 1)) return 0;
        if (v->neg2 && !((s4 >> 16) & 1)) return 0;
    }
    uint64_t a = oddify((s2 >> 16) & kk, v->legacy, v->neg1) & kk;
    uint64_t b = oddify((s4 >> 16) & kk, v->legacy, v->neg2) & kk;
    uint64_t q = (uint64_t)v->x * a + (uint64_t)v->z * b;
    return ((q ^ w) & mk) == (v->pop & mk);
}

static void keep(Lift *v, uint64_t w)
{
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 16;
        uint64_t *nb = realloc(v->buf, (size_t)cap * sizeof *nb);
        if (!nb) return;                    // out of memory: under-report, never crash
        v->buf = nb; v->cap = cap;
    }
    v->buf[v->n++] = w;
}

static void lift(Lift *v, uint64_t w, int L)
{
    if (L == 48) {
        if (v->legacy) {
            // Each seed once: an odd n under its true sign only (an even one
            // reached here under "positive" alone).
            uint64_t s = lcgFromSeed(w);
            int64_t a = lcgNextLong(&s), b = lcgNextLong(&s);
            if (((a & 1) && (a < 0) != v->neg1) || ((b & 1) && (b < 0) != v->neg2)) return;
        }
        if (((popForward(v->mc, w, (int)v->x, (int)v->z) ^ v->pop) & LCG_MASK) == 0)
            keep(v, w);
        return;
    }
    for (uint64_t bit = 0; bit < 2; bit++) {
        uint64_t w2 = w | (bit << L);
        if (consistent(v, w2, L + 1)) lift(v, w2, L + 1);
    }
}

static int cmpU64(const void *p, const void *q)
{
    uint64_t a = *(const uint64_t *)p, b = *(const uint64_t *)q;
    return (a > b) - (a < b);
}

int popInvert(int mc, uint64_t pop, int x, int z, uint64_t *out, int max)
{
    if (mc >= MC_1_18) return -1;
    Lift v;
    memset(&v, 0, sizeof v);
    v.mc = mc; v.legacy = mc < MC_1_13;
    v.x = x; v.z = z; v.pop = pop & LCG_MASK;
    uint32_t xz = (uint32_t)x | (uint32_t)z;
    // At the origin pop is the seed itself; t = 48 makes every level test its
    // own bit, which walks straight to it.
    v.t = 0;
    if (!xz) v.t = 48;
    else while (!((xz >> v.t) & 1)) v.t++;
    for (int g = 0; g < (v.legacy ? 4 : 1); g++) {
        v.neg1 = g & 1; v.neg2 = g >> 1;
        lift(&v, 0, 0);
    }
    qsort(v.buf, (size_t)v.n, sizeof *v.buf, cmpU64);
    for (int i = 0; i < v.n && i < max; i++) out[i] = v.buf[i];
    free(v.buf);
    return v.n;
}
