// bedrockfit -- try to reproduce OBSERVED Bedrock structure positions with
// Java's region-grid placement formula, by brute-forcing the salt.
//
//   bedrockfit <observations.txt> [threads]
//
// Observations are "worldSeed chunkX chunkZ" lines, harvested from the real
// Bedrock server by tier3-bedrock/collect_structures.py. Java places a region
// structure as:
//
//     s = regionX*341873128712 + regionZ*132897987541 + worldSeed + salt
//     setSeed(s);  offX = nextInt(range);  offZ = nextInt(range)
//     chunk = region*spacing + off
//
// (with `range = spacing - separation`, and some structures averaging two draws
// instead of one -- the "triangular" variant). Everything there is known except
// the salt, so if Bedrock kept this scheme the salt is recoverable: 2^32
// candidates, rejected in a couple of nanoseconds each by the first
// observation, verified against the rest.
//
// A NEGATIVE RESULT IS THE POINT. Bedrock is C++ and leans on std::mt19937, so
// the likeliest outcome is that no salt fits any configuration -- which says
// the RNG itself differs and rules out the cheap path before anyone spends days
// on it. Printing "no fit" over an exhaustive search is a real finding; the
// failure mode to avoid is a half-search that leaves the question open.
#include "finders.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>

#define MAXOBS 4096

typedef struct { uint64_t seed; int cx, cz; } Obs;
static Obs g_obs[MAXOBS];
static int g_nobs = 0;

// The configuration space searched: Java's own spacings bracket what the grid
// analysis suggested (32 chunks, separation <= 4), so sweep around it rather
// than betting on one reading of sparse data.
typedef struct { int spacing, range, triangular; } Cfg;
static Cfg g_cfg[64];
static int g_ncfg = 0;

static volatile LONG64 g_hits = 0;

// cubiomes already provides floordiv(); regionOf keeps the intent local.
static inline int regionOf(int c, int spacing) { return floordiv(c, spacing); }

// Java's nextInt(bound) for a non-power-of-two bound, as cubiomes models it.
static inline int jnextInt(uint64_t *s, int n)
{
    return nextInt(s, n);
}

static int fits(uint64_t salt, Cfg c, int strict)
{
    int n = strict ? g_nobs : 1;
    for (int i = 0; i < n; i++) {
        int rx = regionOf(g_obs[i].cx, c.spacing);
        int rz = regionOf(g_obs[i].cz, c.spacing);
        uint64_t s = (uint64_t)((int64_t)rx * 341873128712LL +
                                (int64_t)rz * 132897987541LL) + g_obs[i].seed + salt;
        setSeed(&s, s);
        int ox, oz;
        if (c.triangular) {
            ox = (jnextInt(&s, c.range) + jnextInt(&s, c.range)) / 2;
            oz = (jnextInt(&s, c.range) + jnextInt(&s, c.range)) / 2;
        } else {
            ox = jnextInt(&s, c.range);
            oz = jnextInt(&s, c.range);
        }
        if (rx * c.spacing + ox != g_obs[i].cx) return 0;
        if (rz * c.spacing + oz != g_obs[i].cz) return 0;
    }
    return 1;
}

typedef struct { uint64_t lo, hi; int cfg; } Job;

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job*)arg;
    Cfg c = g_cfg[j->cfg];
    for (uint64_t salt = j->lo; salt < j->hi; salt++) {
        if (!fits(salt, c, 0)) continue;      // cheap reject on one observation
        if (!fits(salt, c, 1)) continue;      // then the whole set
        InterlockedIncrement64(&g_hits);
        printf("MATCH salt=%" PRIu64 "  spacing=%d range=%d %s  (all %d observations)\n",
               salt, c.spacing, c.range, c.triangular ? "triangular" : "uniform", g_nobs);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bedrockfit <observations.txt> [threads]\n"
                        "       each line: <worldSeed> <chunkX> <chunkZ>\n");
        return 2;
    }
    int nthreads = (argc > 2) ? atoi(argv[2]) : 16;
    // Optional "spacing:range:triangular" pins one configuration, which is how
    // the fitter gets tested against a known answer without waiting for the
    // whole sweep.
    int onlyS = 0, onlyR = 0, onlyT = 0, pinned = 0;
    if (argc > 3 && sscanf(argv[3], "%d:%d:%d", &onlyS, &onlyR, &onlyT) == 3) pinned = 1;
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    long long sd; int cx, cz;
    while (g_nobs < MAXOBS && fscanf(f, "%lld %d %d", &sd, &cx, &cz) == 3) {
        g_obs[g_nobs].seed = (uint64_t)sd;
        g_obs[g_nobs].cx = cx; g_obs[g_nobs].cz = cz;
        g_nobs++;
    }
    fclose(f);
    if (g_nobs < 2) { fprintf(stderr, "need at least 2 observations, got %d\n", g_nobs); return 2; }

    if (pinned) {
        g_cfg[g_ncfg++] = (Cfg){onlyS, onlyR, onlyT};
    } else
    for (int spacing = 24; spacing <= 40; spacing += 2)
        for (int sep = 0; sep <= 8; sep += 2) {
            int range = spacing - sep;
            if (range < 1) continue;
            for (int tri = 0; tri < 2 && g_ncfg < 64; tri++)
                g_cfg[g_ncfg++] = (Cfg){spacing, range, tri};
        }

    printf("%d observations, %d configurations, 2^32 salts each\n", g_nobs, g_ncfg);
    printf("(a configuration whose grid cannot even contain the observations is "
           "skipped before searching)\n\n");

    for (int ci = 0; ci < g_ncfg; ci++) {
        Cfg c = g_cfg[ci];
        // Skip configurations that are impossible on their face: an observation
        // whose in-region offset exceeds the range can never be produced.
        int possible = 1;
        for (int i = 0; i < g_nobs && possible; i++) {
            int rx = regionOf(g_obs[i].cx, c.spacing), rz = regionOf(g_obs[i].cz, c.spacing);
            if (g_obs[i].cx - rx * c.spacing >= c.range) possible = 0;
            if (g_obs[i].cz - rz * c.spacing >= c.range) possible = 0;
        }
        if (!possible) continue;

        LARGE_INTEGER fq, t0, t1;
        QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
        Job *jobs = calloc(nthreads, sizeof(Job));
        HANDLE *th = calloc(nthreads, sizeof(HANDLE));
        uint64_t total = 1ULL << 32, chunk = total / nthreads;
        for (int i = 0; i < nthreads; i++) {
            jobs[i].lo = (uint64_t)i * chunk;
            jobs[i].hi = (i == nthreads - 1) ? total : (uint64_t)(i + 1) * chunk;
            jobs[i].cfg = ci;
            th[i] = CreateThread(NULL, 0, worker, &jobs[i], 0, NULL);
        }
        WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
        QueryPerformanceCounter(&t1);
        printf("  spacing=%2d range=%2d %-10s : searched in %.1fs\n",
               c.spacing, c.range, c.triangular ? "triangular" : "uniform",
               (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart);
        fflush(stdout);
        free(jobs); free(th);
    }

    printf("\n%lld salt(s) reproduced every observation.\n", (long long)g_hits);
    if (!g_hits)
        printf("Java's placement formula does NOT explain Bedrock's positions for any\n"
               "salt in any of these grids -- the RNG or the region math differs.\n");
    return g_hits ? 0 : 1;
}
