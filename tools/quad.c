// quad -- find seeds putting four structures in one despawn sphere, by solving.
//
//   quad <version> <structure> <spread_chunks> [seconds] [threads]
//
// The classic quad-hut hunt. Our scanning search calls this "a many-tens-of-
// millions-of-seeds search" and it is worse than that: the true density is
// about one base seed in 10^11, so a brute force over all 2^48 structure seeds
// is days of CPU.
//
// TWO PROPERTIES MAKE IT TRACTABLE.
//
// 1. Translation. cubiomes' own note: a structure seed at region (0,0) is the
//    world seed plus a constant, and region dependence is linear. So a cluster
//    found at regions (0,0)..(1,1) can be moved anywhere with moveStructure().
//    The world only has to be searched ONCE, at the origin.
//
// 2. Inversion. Four structures round a shared region corner is eight
//    constraints, one per axis per structure. Rather than test all eight after
//    generating a seed, the first structure's two are SOLVED -- src/invert.c
//    walks only the 1-in-range^2 of seeds that already place it correctly. The
//    remaining six are then a few LCG steps each.
//
// That turns a ~2^48 walk into ~2^48/range^2 per corner offset. For swamp huts
// that is 576x less work, which is the difference between days and minutes.
//
// A full meet-in-the-middle (mjtb49's gist) would go further still by solving
// all eight constraints at once instead of two. This does not do that; it does
// the part that can be verified with the machinery already here.
#include "invert.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <windows.h>

typedef struct {
    int mc, st, spread;
    int ox, oz;                 // the corner offset this thread solves for
    StructureConfig sc;
    uint64_t visited, found;
    ULONGLONG deadline;
    CRITICAL_SECTION *lock;
} Job;

// Are the four structures around the (1,1) region corner all within `spread`
// chunks of each other? Checked against cubiomes' own placement, not ours.
static int quadFits(const Job *j, uint64_t s48, Pos out[4])
{
    static const int RX[4] = {0, 1, 0, 1}, RZ[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
        Pos p = getFeaturePos(j->sc, s48, RX[i], RZ[i]);
        out[i].x = p.x >> 4; out[i].z = p.z >> 4;
    }
    // Pairwise: every structure within `spread` chunks of every other. That is
    // stricter than "within spread of the centre" and is what a despawn sphere
    // actually needs.
    for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
            int dx = out[a].x - out[b].x, dz = out[a].z - out[b].z;
            if (dx*dx + dz*dz > j->spread * j->spread) return 0;
        }
    return 1;
}

static int onSeed(uint64_t s48, void *arg)
{
    Job *j = arg;
    j->visited++;
    if ((j->visited & 0xFFFFF) == 0 && GetTickCount64() > j->deadline) return 1;

    Pos p[4];
    if (!quadFits(j, s48, p)) return 0;

    j->found++;
    EnterCriticalSection(j->lock);
    printf("QUADBASE %" PRIu64 "   chunks", s48);
    for (int i = 0; i < 4; i++) printf(" (%d,%d)", p[i].x, p[i].z);
    // moveStructure relocates the whole cluster; the base is the portable form.
    printf("   [move with moveStructure(base, regX, regZ)]\n");
    fflush(stdout);
    LeaveCriticalSection(j->lock);
    return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job*)arg;
    invertCount(j->mc, j->st, 0, 0, j->ox, j->oz, 0, onSeed, j);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: quad <version> <structure> <spread_chunks> [seconds]\n");
        return 2;
    }
    int mc = str2mc(argv[1]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    int st = -1;
    for (int i = 0; i < 64 && st < 0; i++) {
        const char *n = struct2str(i);
        if (n && !strcmp(n, argv[2])) st = i;
    }
    if (st < 0) { fprintf(stderr, "unknown structure \"%s\"\n", argv[2]); return 2; }
    if (!invertSupported(mc, st)) {
        fprintf(stderr, "%s placement cannot be inverted\n", argv[2]);
        return 2;
    }
    int spread = atoi(argv[3]);
    int seconds = (argc > 4) ? atoi(argv[4]) : 60;

    StructureConfig sc;
    getStructureConfig(st, mc, &sc);
    int r = sc.chunkRange, R = sc.regionSize;

    // Which offsets can the region-(0,0) structure possibly take? Its partner
    // in region (1,0) sits at chunk R+ox1, so ox0 must be within `spread` of
    // that: ox0 >= R + 0 - spread. Anything further from the corner cannot
    // reach, whatever the other three do -- so those offsets are never solved
    // for at all.
    int lo = R - spread; if (lo < 0) lo = 0;
    int combos = 0;
    for (int a = lo; a < r; a++) combos++;
    if (combos <= 0) {
        printf("IMPOSSIBLE: with range %d in a %d-chunk region, four %ss can "
               "never come within %d chunks of each other.\n", r, R, argv[2], spread);
        return 1;
    }

    printf("%s: range %d, region %d chunks, spread %d\n", argv[2], r, R, spread);
    printf("corner offsets worth solving: %d of %d per axis (%d of %d pairs)\n",
           combos, r, combos * combos, r * r);
    printf("each pair solves 1 seed in %d; a brute force would walk all 2^48\n\n",
           r * r);

    CRITICAL_SECTION lock; InitializeCriticalSection(&lock);
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;

    int n = combos * combos;
    Job *jobs = calloc(n, sizeof(Job));
    HANDLE *th = calloc(n, sizeof(HANDLE));
    int k = 0;
    for (int a = lo; a < r; a++)
        for (int b = lo; b < r; b++, k++) {
            jobs[k].mc = mc; jobs[k].st = st; jobs[k].spread = spread;
            jobs[k].ox = a; jobs[k].oz = b; jobs[k].sc = sc;
            jobs[k].deadline = deadline; jobs[k].lock = &lock;
            th[k] = CreateThread(NULL, 0, worker, &jobs[k], 0, NULL);
        }
    // WaitForMultipleObjects caps at 64 handles; these are few, but be exact.
    for (int i = 0; i < n; i += 64)
        WaitForMultipleObjects((n - i > 64) ? 64 : n - i, th + i, TRUE, INFINITE);

    uint64_t vis = 0, fnd = 0;
    for (int i = 0; i < n; i++) { vis += jobs[i].visited; fnd += jobs[i].found; }
    printf("\n--- funnel ---\n");
    printf("seeds solved  : %" PRIu64 "  (all place the first %s on the corner)\n", vis, argv[2]);
    printf("quad bases    : %" PRIu64 "\n", fnd);
    printf("brute force would have walked ~%" PRIu64 " to cover the same ground\n",
           vis * (uint64_t)(r * r));
    return fnd ? 0 : 1;
}
