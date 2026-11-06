// popinvert -- the structure seeds behind one chunk's population seed
// (1.8 - 1.17; see src/popinvert.h).
//
//   popinvert VERSION CHUNK_X CHUNK_Z SEED [SALT]
//       SEED is the chunk's population seed, as a signed long. With SALT it is
//       instead the seed the decoration RNG was set to, pop + SALT -- from 1.13
//       a decorator's salt is index + 10000 * step. Prints every 48-bit
//       structure seed that gives it; each stands for 65536 world seeds
//       (structure seed + k * 2^48).
//   popinvert forward VERSION CHUNK_X CHUNK_Z WORLD_SEED
//       The population seed the game gives that chunk (cubiomes'
//       getPopulationSeed), for round trips.
//   popinvert self [N]
//       N random trials per version (default 100). A planted seed must come
//       back; every answer must give the target through cubiomes'
//       getPopulationSeed, a forward implementation independent of ours; and
//       ours must agree with it. Then 16 cases small enough to brute-force
//       completely, which must list exactly the seeds brute force finds.
//       Exit 0 when all of that holds.
#include "popinvert.h"
#include "lcg.h"
#include "seed.h"
#include "finders.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

static uint64_t sm(uint64_t *x)     // splitmix64
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Coordinates that stress different parts of the lift: anywhere in the world,
// near the origin, on an axis, on a diagonal (x and z with equal low bits),
// the origin itself, and powers of two (many shared trailing zeros).
static void pick(uint64_t *rs, int kind, int *cx, int *cz)
{
    int32_t a = (int32_t)(sm(rs) % 3750001) - 1875000;
    int32_t b = (int32_t)(sm(rs) % 3750001) - 1875000;
    switch (kind) {
    case 0: *cx = a; *cz = b; break;
    case 1: *cx = a % 33; *cz = b % 33; break;
    case 2: if (sm(rs) & 1) { *cx = a; *cz = 0; } else { *cx = 0; *cz = b; } break;
    case 3: *cx = a; *cz = (sm(rs) & 1) ? a : -a; break;
    case 4: *cx = 0; *cz = 0; break;
    default: {
        int s = (int)(sm(rs) % 20);
        *cx = (sm(rs) & 1 ? 1 : -1) * (1 << s);
        *cz = (sm(rs) & 1) ? 0 : (sm(rs) & 1 ? 1 : -1) * (1 << (sm(rs) % 20));
    }
    }
}

static int self(int trials)
{
    static const int MCS[] = { MC_1_8, MC_1_12, MC_1_13, MC_1_16, MC_1_17 };
    uint64_t rs = 2026, out[256];
    long bad = 0, total = 0, maxN = 0, sumN = 0;
    clock_t t0 = clock();
    for (size_t vi = 0; vi < sizeof MCS / sizeof MCS[0]; vi++) {
        int mc = MCS[vi];
        for (int i = 0; i < trials; i++) {
            int cx, cz;
            pick(&rs, i % 6, &cx, &cz);
            int x = mc >= MC_1_13 ? cx * 16 : cx, z = mc >= MC_1_13 ? cz * 16 : cz;
            uint64_t ws = sm(&rs);
            uint64_t ref = getPopulationSeed(mc, ws, x, z);
            if (popForward(mc, ws, x, z) != ref) {
                if (bad++ < 10) printf("FORWARD %s ws=%" PRId64 " x=%d z=%d\n", mc2str(mc), (int64_t)ws, x, z);
                continue;
            }
            // A planted target, then (every fourth trial) an arbitrary one --
            // most values have no seed or one; whatever comes back must be real.
            uint64_t pop = (i % 4 == 3) ? sm(&rs) : ref;
            int n = popInvert(mc, pop, x, z, out, 256);
            total++; sumN += n; if (n > maxN) maxN = n;
            int found = (i % 4 == 3);
            for (int k = 0; k < n && k < 256; k++) {
                if (out[k] == (ws & LCG_MASK)) found = 1;
                if (((getPopulationSeed(mc, out[k], x, z) ^ pop) & LCG_MASK) != 0) {
                    if (bad++ < 10) printf("WRONG %s pop=%" PRIu64 " x=%d z=%d seed=%" PRIu64 "\n",
                                           mc2str(mc), (uint64_t)(pop & LCG_MASK), x, z, out[k]);
                }
            }
            if (!found && bad++ < 10)
                printf("MISSED %s ws=%" PRIu64 " x=%d z=%d (%d answers)\n", mc2str(mc), (uint64_t)(ws & LCG_MASK), x, z, n);
        }
    }
    if (popInvert(MC_1_18, 0, 16, 16, out, 1) != -1) { bad++; printf("1.18 not refused\n"); }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%ld inversions, %.1f answers each on average, at most %ld, %.2f ms each; %ld failures\n",
           total, total ? (double)sumN / total : 0.0, maxN, total ? 1000.0 * secs / total : 0.0, bad);

    // Completeness without trusting the lift. When x and z share t trailing
    // zero bits, x*a + z*b is 0 mod 2^t, so every answer's low t bits ARE the
    // target's -- and for t >= 22 the other 48 - t bits are few enough to try
    // them all forwards. That is every answer there is, found independently;
    // the lift must list exactly that set. Odd and even multipliers, an axis,
    // x == +-z, and both formulas (1.12 and 1.13+). The coordinates are past
    // the world border, which the arithmetic does not care about.
    static const struct { int mc, t, xm, zm; } FULL[] = {
        { MC_1_16, 24, 1, 0 }, { MC_1_16, 24, 1, -1 }, { MC_1_16, 24, 1, 1 },
        { MC_1_16, 24, 3, 5 }, { MC_1_16, 24, 0, -1 }, { MC_1_16, 24, 1, 1 },
        { MC_1_12, 24, 1, 1 }, { MC_1_12, 24, -1, 0 }, { MC_1_12, 24, 3, 5 },
        { MC_1_12, 24, 1, 1 }, { MC_1_8, 24, 5, -3 }, { MC_1_8, 24, 1, -1 },
        { MC_1_16, 22, 3, 5 }, { MC_1_16, 22, 1, 1 }, { MC_1_12, 22, 5, -3 },
        { MC_1_12, 22, 1, 1 },
    };
    long exact = 0, answers = 0, multi = 0;
    for (size_t i = 0; i < sizeof FULL / sizeof FULL[0]; i++) {
        int mc = FULL[i].mc, t = FULL[i].t;
        int x = (int)((uint32_t)FULL[i].xm << t), z = (int)((uint32_t)FULL[i].zm << t);
        uint64_t pop = popForward(mc, sm(&rs), x, z) & LCG_MASK;
        uint64_t low = pop & ((1ULL << t) - 1);
        int n = popInvert(mc, pop, x, z, out, 256), brute = 0, ok = 1;
        for (uint64_t hi = 0; hi < (1ULL << (48 - t)); hi++) {
            uint64_t w = low | (hi << t);
            if (((popForward(mc, w, x, z) ^ pop) & LCG_MASK) != 0) continue;
            brute++;
            int seen = 0;
            for (int k = 0; k < n && k < 256; k++) if (out[k] == w) seen = 1;
            if (!seen) { ok = 0; if (bad++ < 20)
                printf("MISSED %s x=%d z=%d seed=%" PRIu64 " (%d listed)\n", mc2str(mc), x, z, w, n); }
        }
        if (brute != n) { ok = 0; if (bad++ < 20)
            printf("COUNT %s x=%d z=%d: %d exist, %d listed\n", mc2str(mc), x, z, brute, n); }
        exact += ok; answers += brute; multi += brute > 1;
    }
    printf("%ld of %zu exhaustive cases list exactly the %ld seeds brute force finds"
           " (%ld cases with more than one)\n",
           exact, sizeof FULL / sizeof FULL[0], answers, multi);
    return bad ? 1 : 0;
}

// Chunk coordinates as the game passes them: chunk coordinates up to 1.12,
// block coordinates after. 0 with a message when they are not usable.
static int coords(int mc, const char *sx, const char *sz, int *x, int *z)
{
    int64_t cx, cz;
    if (!parseJavaLong(sx, &cx) || !parseJavaLong(sz, &cz)) {
        fprintf(stderr, "chunk coordinates must be integers\n");
        return 0;
    }
    if (cx < -1875000 || cx > 1875000 || cz < -1875000 || cz > 1875000) {
        fprintf(stderr, "chunk coordinates are out of the world\n");
        return 0;
    }
    *x = mc >= MC_1_13 ? (int)cx * 16 : (int)cx;
    *z = mc >= MC_1_13 ? (int)cz * 16 : (int)cz;
    return 1;
}

static int version(const char *s)
{
    int mc = str2mc(s);
    if (mc <= MC_UNDEF) { fprintf(stderr, "unknown version\n"); return -1; }
    if (mc >= MC_1_18) {
        fprintf(stderr, "%s seeds the population RNG with Xoroshiro over all 64 bits;"
                        " it does not reverse to a structure seed (1.17 and earlier only)\n", s);
        return -1;
    }
    return mc;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "self"))
        return self(argc > 2 ? atoi(argv[2]) : 100);
    if (argc == 6 && !strcmp(argv[1], "forward")) {
        // The other direction, through cubiomes: what the game seeds the chunk
        // with, as the signed long it would print.
        int mc = version(argv[2]), x, z;
        uint64_t ws;
        if (mc < 0 || !coords(mc, argv[3], argv[4], &x, &z)) return 2;
        if (!parseWorldSeed(argv[5], &ws)) { fprintf(stderr, "no seed given\n"); return 2; }
        printf("%" PRId64 "\n", (int64_t)getPopulationSeed(mc, ws, x, z));
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage: popinvert VERSION CHUNK_X CHUNK_Z SEED [SALT]\n"
                        "       popinvert forward VERSION CHUNK_X CHUNK_Z WORLD_SEED\n"
                        "       popinvert self [N]\n");
        return 2;
    }
    int mc = version(argv[1]), x, z;
    if (mc < 0 || !coords(mc, argv[2], argv[3], &x, &z)) return 2;
    int64_t seed, salt = 0;
    if (!parseJavaLong(argv[4], &seed) || (argc > 5 && !parseJavaLong(argv[5], &salt))) {
        fprintf(stderr, "the seed and salt must be integers\n");
        return 2;
    }
    enum { MAXOUT = 4096 };
    static uint64_t out[MAXOUT];
    uint64_t pop = (uint64_t)seed - (uint64_t)salt;
    int n = popInvert(mc, pop, x, z, out, MAXOUT);
    for (int i = 0; i < n && i < MAXOUT; i++)
        printf("%" PRIu64 "\n", out[i]);
    if (n > MAXOUT) fprintf(stderr, "(only the first %d printed)\n", MAXOUT);
    fprintf(stderr, "%d structure seed%s (each + k * 2^48 for k = 0..65535 is a world seed)\n",
            n, n == 1 ? "" : "s");
    return 0;
}
