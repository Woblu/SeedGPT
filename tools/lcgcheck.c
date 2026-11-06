// lcgcheck -- verify src/lcg.h.
//
//   lcgcheck self      round trips on random states (exit 0 = all agree)
//   lcgcheck table     the same table tools/LcgCheck.java prints from the real
//                      java.util.Random; test.sh diffs the two
//
// The table covers new Random(seed) for fixed and pseudo-random seeds:
// nextInt(), nextInt(bound) for power-of-two, small, and rejection-prone
// bounds, nextLong(), and the internal state after each step.
#include "lcg.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static uint64_t sm(uint64_t *x)     // splitmix64, identical in LcgCheck.java
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static const int32_t BOUNDS[] = { 1, 2, 3, 7, 16, 100, 1 << 30, (1 << 30) + 1, 0x7fffffff };

static int table(void)
{
    uint64_t fixed[] = { 0, 1, (uint64_t)-1, 12345, 1ULL << 47, 0x5DEECE66DULL,
                         (uint64_t)-4172144997902289642LL };
    uint64_t x = 42;
    for (int i = 0; i < 7 + 200; i++) {
        uint64_t seed = i < 7 ? fixed[i] : sm(&x);
        uint64_t s = lcgFromSeed(seed);
        printf("%" PRId64, (int64_t)seed);
        printf(" %d", lcgNextInt(&s));
        for (size_t b = 0; b < sizeof BOUNDS / sizeof BOUNDS[0]; b++)
            printf(" %d", lcgNextIntBound(&s, BOUNDS[b]));
        printf(" %" PRId64, lcgNextLong(&s));
        printf(" %" PRIu64 "\n", lcgToSeed(s));      // the state, as getSeed-like value
    }
    return 0;
}

static int self(void)
{
    uint64_t x = 7, bad = 0, multi = 0, none = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t s = sm(&x) & LCG_MASK;
        if (lcgBack(lcgStep(s)) != s || lcgStep(lcgBack(s)) != s) bad++;
        int64_t n = (int64_t)(sm(&x) % 100000) - 50000;
        uint64_t t = s;
        if (n >= 0) for (int64_t k = 0; k < n; k++) t = lcgStep(t);
        else        for (int64_t k = 0; k < -n; k++) t = lcgBack(t);
        if (lcgSkip(s, n) != t) bad++;
        if (lcgSkip(lcgSkip(s, n), -n) != s) bad++;
        // nextLong reversal: the true state is among the answers, and every
        // answer really produces the value.
        uint64_t u = s;
        int64_t v = lcgNextLong(&u);
        uint64_t out[4];
        int c = lcgReverseNextLong(v, out), found = 0;
        for (int k = 0; k < c; k++) {
            uint64_t w = out[k];
            if (lcgNextLong(&w) != v) bad++;
            if (out[k] == s) found = 1;
        }
        if (!found) bad++;
        if (c > 1) multi++;
    }
    // A value no state produces must give no answer, not a wrong one.
    for (int i = 0; i < 200; i++) {
        int64_t v = (int64_t)sm(&x);
        uint64_t out[4];
        int c = lcgReverseNextLong(v, out);
        if (!c) none++;
        for (int k = 0; k < c; k++) { uint64_t w = out[k]; if (lcgNextLong(&w) != v) bad++; }
    }
    printf("lcg self-check: %" PRIu64 " failures; %" PRIu64 " of 2000 values had two "
           "source states; %" PRIu64 " of 200 random values are no nextLong at all\n",
           bad, multi, none);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "table")) return table();
    if (argc > 1 && !strcmp(argv[1], "self"))  return self();
    fprintf(stderr, "usage: lcgcheck self | table\n");
    return 2;
}
