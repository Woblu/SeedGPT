// findat -- seeds with a structure at an EXACT chunk, found by solving.
//
//   findat <version> <structure> <chunkX> <chunkZ> [hits] [threads]
//
// "A village exactly at spawn" is the kind of request the scanning search
// answers badly: it walks structure seeds and throws away the 675 in 676 that
// put the village somewhere else. Inversion asks the question the other way and
// walks only seeds that already place it correctly, so the work per hit drops
// by range^2.
//
// WHERE THE WIN IS, precisely. Inversion pays when the set of acceptable chunk
// positions is SMALL compared with range^2 (~576). One exact chunk is one
// position, so the whole reduction lands. "Within 500 blocks" is roughly 3000
// acceptable positions, which is more than range^2 -- the solution sets overlap
// and cover the seed space, so there is nothing to win and the ordinary scan is
// the right tool. This does the first case and says so.
//
// Position depends only on the low 48 bits, so the inverter hands back
// structure seeds that are already correct by construction. Biome viability
// needs the full 64, which is the same two-pass split the rest of the project
// uses -- upper bits are sampled per structure seed.
#include "invert.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <windows.h>

#define UPPER_TRIES 64

typedef struct {
    int mc, st, regX, regZ, ox, oz, cx, cz;
    Generator g;
    uint64_t tried, viable;
    int want, found;
    CRITICAL_SECTION *lock;
    volatile LONG *total;
} Job;

static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int onSeed(uint64_t s48, void *arg)
{
    Job *j = arg;
    j->tried++;

    // The structure is already where it was asked for -- that is what the
    // inverter guarantees. What is left is whether the biome allows it, which
    // is a 64-bit question, so sample upper bits.
    uint64_t st = mix64(s48);
    for (int t = 0; t < UPPER_TRIES; t++) {
        st = mix64(st);
        uint64_t ws = ((st & 0xFFFFULL) << 48) | s48;
        applySeed(&j->g, DIM_OVERWORLD, ws);
        if (!isViableStructurePos(j->st, &j->g, j->cx << 4, j->cz << 4, 0)) continue;

        j->viable++;
        EnterCriticalSection(j->lock);
        if (j->found < j->want) {
            printf("SEED %" PRId64 "   %s at chunk (%d,%d), block (%d,%d)\n",
                   (int64_t)ws, struct2str(j->st), j->cx, j->cz,
                   j->cx << 4, j->cz << 4);
            fflush(stdout);
            j->found++;
        }
        LeaveCriticalSection(j->lock);
        InterlockedIncrement(j->total);
        return (*j->total >= j->want);
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job*)arg;
    setupGenerator(&j->g, j->mc, 0);
    invertCount(j->mc, j->st, j->regX, j->regZ, j->ox, j->oz, 0, onSeed, j);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: findat <version> <structure> <chunkX> <chunkZ> [hits]\n");
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
        fprintf(stderr, "%s placement cannot be inverted (different draw count "
                        "or a rejection roll) -- use the scanning search\n", argv[2]);
        return 2;
    }
    int cx = atoi(argv[3]), cz = atoi(argv[4]);
    int want = (argc > 5) ? atoi(argv[5]) : 5;

    StructureConfig sc;
    getStructureConfig(st, mc, &sc);
    // Which region owns this chunk, and where in it does the chunk sit?
    int regX = (int)floor((double)cx / sc.regionSize);
    int regZ = (int)floor((double)cz / sc.regionSize);
    int ox = cx - regX * sc.regionSize;
    int oz = cz - regZ * sc.regionSize;
    if (ox >= (int)sc.chunkRange || oz >= (int)sc.chunkRange) {
        printf("IMPOSSIBLE: chunk (%d,%d) is offset (%d,%d) in its region, but "
               "%s only ever generates in the first %d chunks of one.\n"
               "No seed places it there -- this is a fact about the placement "
               "grid, not a search that failed.\n",
               cx, cz, ox, oz, argv[2], sc.chunkRange);
        return 1;
    }

    printf("%s at chunk (%d,%d) = region (%d,%d) offset (%d,%d)\n",
           argv[2], cx, cz, regX, regZ, ox, oz);
    printf("solving: 1 seed in %d places it here, and only those are visited\n\n",
           sc.chunkRange * sc.chunkRange);

    CRITICAL_SECTION lock; InitializeCriticalSection(&lock);
    volatile LONG total = 0;
    Job j; memset(&j, 0, sizeof j);
    j.mc = mc; j.st = st; j.regX = regX; j.regZ = regZ;
    j.ox = ox; j.oz = oz; j.cx = cx; j.cz = cz;
    j.want = want; j.lock = &lock; j.total = &total;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    HANDLE h = CreateThread(NULL, 0, worker, &j, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    QueryPerformanceCounter(&t1);
    double el = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;

    printf("\n--- funnel ---\n");
    printf("structure seeds solved : %" PRIu64 "  (every one places it correctly)\n", j.tried);
    printf("biome-viable           : %" PRIu64 "\n", j.viable);
    printf("time                   : %.2fs\n", el);
    printf("equivalent scan would have walked ~%" PRIu64 " seeds for the same hits\n",
           j.tried * (uint64_t)(sc.chunkRange * sc.chunkRange));
    return j.found ? 0 : 1;
}
