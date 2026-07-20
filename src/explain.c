// explain -- estimate a query's selectivity by sampling, before committing to
// a long search. Answers "will this ever finish?" in a couple of seconds.
//
// Method, and why it is shaped this way:
//
//  1. Samples are drawn UNIFORMLY from the whole 2^48 structure-seed space via
//     splitmix64, not sequentially from 0. A sequential scan touches a
//     microscopic corner; if that corner were unrepresentative every rate
//     measured from it would be wrong. explainCompare() tests exactly that.
//
//  2. TWO PHASES, because the two stages have wildly different costs and rates.
//     Phase A samples geometry only (~9 ns/seed) so it can afford a huge N and
//     still measure a tiny p1. It also collects survivors. Phase B then spends a
//     fixed probe budget on those survivors, so p2 gets a usable sample size
//     even when p1 is minuscule. A flat "8 probes per survivor" would give 8
//     probes when 1 seed survives -- useless.
//
//  3. The time model mirrors the REAL searcher, not the sampler: it caps upper
//     probes at UPPER_SAMPLES and counts the expected number tried before a
//     success, which is what actually dominates wall clock.
#include "explain.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <windows.h>

#define MASK48 ((1ULL << 48) - 1)
#define UPPER_SAMPLES 64        // must match tools/find.c
#define MAX_KEEP 512            // survivors retained from phase A
#define PROBE_BUDGET 40000      // total phase-B upper probes

static inline uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// ---------------------------------------------------- phase A: geometry only
typedef struct {
    const Query *q;
    uint64_t n, rngState, seqBase;
    int sequential;
    uint64_t drawn, pass1;
    uint64_t keep[MAX_KEEP]; Match keepM[MAX_KEEP]; int nkeep;
} PhaseA;

static DWORD WINAPI workerA(LPVOID arg)
{
    PhaseA *s = (PhaseA*)arg;
    for (uint64_t i = 0; i < s->n; i++) {
        uint64_t s48 = s->sequential ? ((s->seqBase + i) & MASK48)
                                     : (splitmix64(&s->rngState) & MASK48);
        s->drawn++;
        Match m;
        if (!queryStage1(s->q, s48, &m)) continue;
        s->pass1++;
        if (s->nkeep < MAX_KEEP) { s->keep[s->nkeep] = s48; s->keepM[s->nkeep] = m; s->nkeep++; }
    }
    return 0;
}

// ------------------------------------------- phase B: upper-bit biome probing
typedef struct {
    const Query *q;
    const uint64_t *seeds; const Match *matches;
    int nseeds, probesEach;
    uint64_t rngState;
    uint64_t tried, passed;
} PhaseB;

static DWORD WINAPI workerB(LPVOID arg)
{
    PhaseB *s = (PhaseB*)arg;
    Generator g;
    setupGenerator(&g, s->q->mc, 0);
    for (int i = 0; i < s->nseeds; i++)
        for (int p = 0; p < s->probesEach; p++) {
            uint64_t up = (splitmix64(&s->rngState) >> 16) & 0xFFFF;
            s->tried++;
            // queryStage2 applies the seed itself, once per dimension.
            if (queryStage2(s->q, &g, (up << 48) | s->seeds[i], &s->matches[i])) s->passed++;
        }
    return 0;
}

static double runPhaseA(const Query *q, uint64_t total, int nthreads, int sequential,
                        Estimate *out, uint64_t *keep, Match *keepM, int *nkeep)
{
    PhaseA *sp = calloc(nthreads, sizeof(PhaseA));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < nthreads; i++) {
        sp[i].q = q; sp[i].n = total / nthreads;
        sp[i].rngState = 0xC0FFEE123456789ULL + 0x9E3779B9ULL * (uint64_t)i;
        sp[i].sequential = sequential;
        sp[i].seqBase = (uint64_t)i * (total / nthreads);
        th[i] = CreateThread(NULL, 0, workerA, &sp[i], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;

    out->drawn = out->pass1 = 0;
    if (nkeep) *nkeep = 0;
    for (int i = 0; i < nthreads; i++) {
        out->drawn += sp[i].drawn; out->pass1 += sp[i].pass1;
        if (nkeep)
            for (int k = 0; k < sp[i].nkeep && *nkeep < MAX_KEEP; k++) {
                keep[*nkeep] = sp[i].keep[k]; keepM[*nkeep] = sp[i].keepM[k]; (*nkeep)++;
            }
    }
    out->p1 = out->drawn ? (double)out->pass1 / out->drawn : 0;
    free(sp); free(th);
    return sec;
}

void explainQuery(const Query *q, uint64_t samples, int nthreads, FILE *f)
{
    Estimate e; memset(&e, 0, sizeof e);
    uint64_t keep[MAX_KEEP]; Match keepM[MAX_KEEP]; int nkeep = 0;

    double secA = runPhaseA(q, samples, nthreads, 0, &e, keep, keepM, &nkeep);
    double tA = e.drawn ? secA / e.drawn : 0;              // sec per geometry check

    fprintf(f, "--- explain ---\n");
    fprintf(f, "phase A  %llu structure seeds, geometry only, %.2fs\n",
            (unsigned long long)e.drawn, secA);
    fprintf(f, "  pass 1 survival : %.5f%%   (%llu/%llu)\n",
            100.0 * e.p1, (unsigned long long)e.pass1, (unsigned long long)e.drawn);

    if (e.pass1 == 0) {
        double ub = 3.0 / (double)e.drawn;   // rule-of-three 95% upper bound
        fprintf(f, "\nVERDICT: no sampled seed satisfies the geometry.\n");
        fprintf(f, "         95%% confidence the rate is below %.2e, i.e. fewer than ~%.0f\n",
                ub, ub * 281474976710656.0);
        fprintf(f, "         matching structure seeds exist in all of 2^48.\n");
        fprintf(f, "         Loosen a radius or drop a condition.\n");
        return;
    }

    // Phase B: spend a fixed probe budget on the survivors we kept.
    int probesEach = nkeep ? (PROBE_BUDGET / nkeep) : 0;
    if (probesEach < 1) probesEach = 1;
    if (probesEach > 65536) probesEach = 65536;

    PhaseB *sp = calloc(nthreads, sizeof(PhaseB));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));
    LARGE_INTEGER fr, t0, t1; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
    int per = (nkeep + nthreads - 1) / nthreads;
    for (int i = 0; i < nthreads; i++) {
        int lo = i * per, hi = lo + per; if (hi > nkeep) hi = nkeep;
        sp[i].q = q; sp[i].seeds = keep + lo; sp[i].matches = keepM + lo;
        sp[i].nseeds = (hi > lo) ? (hi - lo) : 0;
        sp[i].probesEach = probesEach;
        sp[i].rngState = 0xBEEF0000ULL + 0x9E3779B9ULL * (uint64_t)i;
        th[i] = CreateThread(NULL, 0, workerB, &sp[i], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double secB = (double)(t1.QuadPart - t0.QuadPart) / fr.QuadPart;
    for (int i = 0; i < nthreads; i++) { e.upTried += sp[i].tried; e.upPassed += sp[i].passed; }
    free(sp); free(th);

    double tB = e.upTried ? secB * nthreads / e.upTried : 0;   // core-sec per probe
    e.p2 = e.upTried ? (double)e.upPassed / e.upTried : 0;

    fprintf(f, "phase B  %llu upper-bit probes across %d survivors (%d each), %.2fs\n",
            (unsigned long long)e.upTried, nkeep, probesEach, secB);
    fprintf(f, "  pass 2 survival : %.5f%%   (%llu/%llu)  per upper-bit variant\n",
            100.0 * e.p2, (unsigned long long)e.upPassed, (unsigned long long)e.upTried);

    if (e.upPassed == 0) {
        double ub = 3.0 / (double)e.upTried;
        fprintf(f, "\nVERDICT: geometry works, but no biome-viable world seed in %llu probes.\n",
                (unsigned long long)e.upTried);
        fprintf(f, "         95%% confidence pass-2 rate is below %.2e. With 2^16 variants per\n", ub);
        fprintf(f, "         structure seed that is at most ~%.1f%% of survivors yielding any\n",
                100.0 * (1.0 - pow(1.0 - ub, 65536.0)));
        fprintf(f, "         world seed. Probably viable but very rare -- or impossible.\n");
        return;
    }

    // The real searcher caps at UPPER_SAMPLES probes and stops at the first hit.
    e.pFamily = 1.0 - pow(1.0 - e.p2, (double)UPPER_SAMPLES);
    e.pHit    = e.p1 * e.pFamily;
    double pFull = 1.0 - pow(1.0 - e.p2, 65536.0);     // if all variants were tried

    // expected upper probes actually performed per pass-1 survivor
    double eUp = (e.p2 > 0) ? (1.0 - pow(1.0 - e.p2, (double)UPPER_SAMPLES)) / e.p2 : UPPER_SAMPLES;
    if (eUp > UPPER_SAMPLES) eUp = UPPER_SAMPLES;

    double coreSecPerSeed = tA * nthreads + e.p1 * eUp * tB;
    double secPerSeed = coreSecPerSeed / nthreads;
    double perHit = e.pHit > 0 ? 1.0 / e.pHit : INFINITY;

    fprintf(f, "\nmodel (mirrors the real searcher: cap %d upper probes, stop at first hit)\n",
            UPPER_SAMPLES);
    fprintf(f, "  P(some upper works | pass 1)  : %.4f%%   [%.4f%% if all 2^16 tried]\n",
            100.0 * e.pFamily, 100.0 * pFull);
    fprintf(f, "  P(hit per structure seed)     : %.6f%%\n", 100.0 * e.pHit);
    fprintf(f, "  expected upper probes/survivor: %.1f of %d\n", eUp, UPPER_SAMPLES);
    fprintf(f, "  structure seeds per hit       : %.0f\n", perHit);
    fprintf(f, "  matching structure seeds in 2^48 : %.3g\n", 281474976710656.0 * e.p1 * pFull);
    fprintf(f, "  predicted search rate         : %s on %d threads\n",
            humanRate(1.0 / secPerSeed), nthreads);

    double secPerHit = perHit * secPerSeed;
    fprintf(f, "\ntime to first hit : %s\n", humanTime(secPerHit));
    fprintf(f, "time to 12 hits   : %s\n", humanTime(secPerHit * 12));

    if (e.p1 > 0.5)
        fprintf(f, "\nWARNING: pass 1 keeps %.1f%% of seeds -- it is not filtering, so every\n"
                   "         seed pays full biome cost. Tighten a radius.\n", 100.0 * e.p1);
    if (secPerHit > 86400.0 * 7)      fprintf(f, "\nVERDICT: too rare to search this way.\n");
    else if (secPerHit > 600.0)       fprintf(f, "\nVERDICT: slow but feasible.\n");
    else if (secPerHit < 1.0)         fprintf(f, "\nVERDICT: very common. Instant results.\n");
    else                              fprintf(f, "\nVERDICT: tractable.\n");
}

void explainCompare(const Query *q, uint64_t samples, int nthreads, FILE *f)
{
    Estimate uni, seq; memset(&uni, 0, sizeof uni); memset(&seq, 0, sizeof seq);
    runPhaseA(q, samples, nthreads, 0, &uni, NULL, NULL, NULL);
    runPhaseA(q, samples, nthreads, 1, &seq, NULL, NULL, NULL);

    fprintf(f, "--- sampling bias check (geometry only) ---\n");
    fprintf(f, "  uniform over 2^48 : pass1 %.4f%%  (%llu/%llu)\n",
            100.0 * uni.p1, (unsigned long long)uni.pass1, (unsigned long long)uni.drawn);
    fprintf(f, "  sequential from 0 : pass1 %.4f%%  (%llu/%llu)\n",
            100.0 * seq.p1, (unsigned long long)seq.pass1, (unsigned long long)seq.drawn);
    double n1 = (double)uni.drawn, n2 = (double)seq.drawn;
    double se = sqrt(uni.p1*(1-uni.p1)/n1 + seq.p1*(1-seq.p1)/n2);
    double d = fabs(uni.p1 - seq.p1);
    fprintf(f, "  difference %.4f%%, standard error %.4f%%  -> %.1f sigma\n",
            100.0*d, 100.0*se, se > 0 ? d/se : 0.0);
    fprintf(f, "  %s\n\n", (se > 0 && d/se > 3.0)
            ? "BIASED: scanning from 0 is NOT representative -- searches are skewed."
            : "consistent: scanning from 0 looks representative for this query.");
}

const char *humanRate(double perSec)
{
    static char buf[48];
    if (perSec >= 1e6)      snprintf(buf, sizeof buf, "%.2f M seeds/s", perSec / 1e6);
    else if (perSec >= 1e3) snprintf(buf, sizeof buf, "%.0f k seeds/s", perSec / 1e3);
    else                    snprintf(buf, sizeof buf, "%.0f seeds/s", perSec);
    return buf;
}

const char *humanTime(double s)
{
    static char buf[64];
    if (!isfinite(s))   snprintf(buf, sizeof buf, "never");
    else if (s < 1)     snprintf(buf, sizeof buf, "%.0f ms", s * 1000);
    else if (s < 90)    snprintf(buf, sizeof buf, "%.1f s", s);
    else if (s < 5400)  snprintf(buf, sizeof buf, "%.1f min", s / 60);
    else if (s < 172800)snprintf(buf, sizeof buf, "%.1f hours", s / 3600);
    else if (s < 3.15e9)snprintf(buf, sizeof buf, "%.1f days", s / 86400);
    else                snprintf(buf, sizeof buf, "%.1f years", s / 3.15576e7);
    return buf;
}
