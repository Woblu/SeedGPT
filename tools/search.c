// End-to-end seed search, structure-first ordering.
//
//   Stage 1  48-bit structure seed. Pure LCG, no biome generator. ~9 ns/check.
//   Stage 2  For survivors only: upper-16 variants, biome-verified. ~31 us/seed.
//
// Structure layout depends only on the low 48 bits, so one stage-1 rejection
// kills all 65536 world seeds sharing that structure seed.
//
// The stage-1 pass rate printed in the funnel is the number that matters. If it
// is near 100%, the query is too loose to filter and every seed pays full biome
// cost -- the search is then bounded by ~32k seeds/s/core, not ~116M. Tighten
// the radii until stage 1 rejects most seeds.
//
//   usage: search [range] [threads] [mansionR] [villageR]

#include "finders.h"
#include "generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include <windows.h>

#define MC_VER        MC_1_21
#define MAX_HITS      12
#define UPPER_SAMPLES 64

static int MANSION_R = 300;
static int VILLAGE_R = 400;

typedef struct { uint64_t worldSeed; Pos mansion, village; int mdist, vdist; } Hit;

typedef struct {
    uint64_t lo, hi;
    uint64_t scanned, stage1_pass, stage2_pass, applyseeds;
    Hit hits[MAX_HITS];
    int nhits;
} Job;

static CRITICAL_SECTION g_lock;

static inline int64_t dist2(Pos a, Pos b) {
    int64_t dx = (int64_t)a.x - b.x, dz = (int64_t)a.z - b.z;
    return dx*dx + dz*dz;
}

// Stage 1: geometry only. Picks the NEAREST qualifying mansion, not the first
// one in scan order -- otherwise results skew to whichever region we scan first.
static int stage1(uint64_t s48, Pos *m_out, Pos *v_out)
{
    const Pos origin = {0, 0};
    const int64_t mr2 = (int64_t)MANSION_R * MANSION_R;
    const int64_t vr2 = (int64_t)VILLAGE_R * VILLAGE_R;

    Pos best_m; int64_t best_d = INT64_MAX; int found_m = 0;
    const int mreg = (MANSION_R / 1280) + 1;
    for (int rx = -mreg; rx <= mreg; rx++)
    for (int rz = -mreg; rz <= mreg; rz++)
    {
        Pos mp;
        if (!getStructurePos(Mansion, MC_VER, s48, rx, rz, &mp)) continue;
        int64_t d = dist2(mp, origin);
        if (d > mr2) continue;
        if (d < best_d) { best_d = d; best_m = mp; found_m = 1; }
    }
    if (!found_m) return 0;

    Pos best_v; int64_t best_vd = INT64_MAX; int found_v = 0;
    int vr0x = (int)floor((best_m.x - VILLAGE_R) / 544.0);
    int vr1x = (int)floor((best_m.x + VILLAGE_R) / 544.0);
    int vr0z = (int)floor((best_m.z - VILLAGE_R) / 544.0);
    int vr1z = (int)floor((best_m.z + VILLAGE_R) / 544.0);
    for (int vx = vr0x; vx <= vr1x; vx++)
    for (int vz = vr0z; vz <= vr1z; vz++)
    {
        Pos vp;
        if (!getStructurePos(Village, MC_VER, s48, vx, vz, &vp)) continue;
        int64_t d = dist2(best_m, vp);
        if (d > vr2) continue;
        if (d < best_vd) { best_vd = d; best_v = vp; found_v = 1; }
    }
    if (!found_v) return 0;

    *m_out = best_m; *v_out = best_v;
    return 1;
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *job = (Job*)arg;
    Generator g;
    setupGenerator(&g, MC_VER, 0);

    for (uint64_t s48 = job->lo; s48 < job->hi; s48++)
    {
        job->scanned++;
        Pos m, v;
        if (!stage1(s48, &m, &v)) continue;
        job->stage1_pass++;

        for (uint64_t up = 0; up < UPPER_SAMPLES; up++)
        {
            uint64_t ws = (up << 48) | s48;
            applySeed(&g, DIM_OVERWORLD, ws);
            job->applyseeds++;
            if (!isViableStructurePos(Mansion, &g, m.x, m.z, 0)) continue;
            if (!isViableStructurePos(Village, &g, v.x, v.z, 0)) continue;
            job->stage2_pass++;

            EnterCriticalSection(&g_lock);
            if (job->nhits < MAX_HITS) {
                Hit *h = &job->hits[job->nhits++];
                h->worldSeed = ws; h->mansion = m; h->village = v;
                h->mdist = (int)sqrt((double)dist2(m, (Pos){0,0}));
                h->vdist = (int)sqrt((double)dist2(m, v));
            }
            LeaveCriticalSection(&g_lock);
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t range = (argc > 1) ? strtoull(argv[1], NULL, 10) : 2000000;
    int nthreads   = (argc > 2) ? atoi(argv[2]) : 16;
    if (argc > 3) MANSION_R = atoi(argv[3]);
    if (argc > 4) VILLAGE_R = atoi(argv[4]);

    InitializeCriticalSection(&g_lock);
    printf("query   : mansion within %d blocks of spawn,\n", MANSION_R);
    printf("          village within %d blocks of the mansion, both viable\n", VILLAGE_R);
    printf("version : MC 1.21   scanning %" PRIu64 " structure seeds, %d threads\n\n",
           range, nthreads);

    Job *jobs = calloc(nthreads, sizeof(Job));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));
    uint64_t chunk = range / nthreads;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < nthreads; i++) {
        jobs[i].lo = (uint64_t)i * chunk;
        jobs[i].hi = (i == nthreads-1) ? range : (uint64_t)(i+1) * chunk;
        th[i] = CreateThread(NULL, 0, worker, &jobs[i], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double el = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;

    uint64_t scanned=0, p1=0, p2=0, ap=0;
    for (int i = 0; i < nthreads; i++) {
        scanned += jobs[i].scanned; p1 += jobs[i].stage1_pass;
        p2 += jobs[i].stage2_pass; ap += jobs[i].applyseeds;
    }
    int shown = 0;
    for (int i = 0; i < nthreads && shown < MAX_HITS; i++)
        for (int j = 0; j < jobs[i].nhits && shown < MAX_HITS; j++, shown++) {
            Hit *h = &jobs[i].hits[j];
            printf("SEED %" PRId64 "\n", (int64_t)h->worldSeed);
            printf("   mansion  x=%6d z=%6d   %4d from spawn\n", h->mansion.x, h->mansion.z, h->mdist);
            printf("   village  x=%6d z=%6d   %4d from mansion\n\n", h->village.x, h->village.z, h->vdist);
        }

    double s1rate = 100.0 * p1 / (scanned ? scanned : 1);
    printf("--- funnel ---\n");
    printf("scanned      : %" PRIu64 " structure seeds in %.2fs\n", scanned, el);
    printf("stage1 (48b) : %" PRIu64 "  (%.4f%% survive)  <- the number that matters\n", p1, s1rate);
    printf("applySeed    : %" PRIu64 "  biome inits actually paid for\n", ap);
    printf("stage2 (64b) : %" PRIu64 "  biome-verified world seeds\n", p2);
    printf("throughput   : %.2f M structure-seeds/s\n", scanned / el / 1e6);
    if (s1rate > 50.0)
        printf("\nWARNING: stage 1 rejects almost nothing -- query too loose to filter.\n"
               "         Every seed is paying full biome cost. Tighten the radii.\n");
    return 0;
}
