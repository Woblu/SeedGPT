// find -- run a JSON condition-tree query through the planner and search.
//
//   usage: find <query.json> [range] [threads]
//
// Prints the PLAN before searching, so the ordering the planner chose is
// auditable, and reports the pass-1 survival rate afterwards -- the single
// number that tells you whether the query is selective enough to be fast.
#include "query.h"
#include "loot.h"
#include "ore.h"
#include "explain.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <windows.h>

#define MAX_HITS      12
#define UPPER_SAMPLES 64

typedef struct { uint64_t ws; Match m; } Hit;

typedef struct {
    const Query *q;
    uint64_t lo, hi;
    uint64_t scanned, pass1, pass2, applies;
    Hit hits[MAX_HITS];
    int nhits;
} Job;

static CRITICAL_SECTION g_lock;
static volatile LONG g_found = 0;
static int g_want = MAX_HITS;

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job*)arg;
    Generator g;
    setupGenerator(&g, j->q->mc, 0);
    LootCache *lc = lootCacheNew();   // one per thread; loot tables are stateful

    for (uint64_t s48 = j->lo; s48 < j->hi; s48++) {
        if (g_found >= g_want) break;
        j->scanned++;

        Match m;
        if (!queryStage1(j->q, s48, &m)) continue;   // cheap reject
        j->pass1++;

        // Scatter the upper 16 bits. Trying up=0 first made the reported seed
        // literally equal the structure seed -- so a scan from 0 returned
        // "128", "146", ... clustered at thread starts. Any upper value is
        // equally valid, so pick them pseudo-randomly per seed.
        uint64_t ustate = s48 * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
        for (uint64_t t = 0; t < UPPER_SAMPLES; t++) {
            ustate ^= ustate >> 30; ustate *= 0xBF58476D1CE4E5B9ULL;
            ustate ^= ustate >> 27; ustate *= 0x94D049BB133111EBULL;
            ustate ^= ustate >> 31;
            uint64_t up = ustate & 0xFFFF;
            uint64_t ws = (up << 48) | s48;
            j->applies++;
            // queryStage2 applies the seed itself, once per dimension the query
            // touches -- the caller must not applySeed here.
            if (!queryStage2(j->q, &g, ws, &m, lc)) continue;
            j->pass2++;

            EnterCriticalSection(&g_lock);
            if (j->nhits < MAX_HITS) { j->hits[j->nhits].ws = ws; j->hits[j->nhits].m = m; j->nhits++; }
            InterlockedIncrement(&g_found);
            LeaveCriticalSection(&g_lock);
            break;
        }
    }
    lootCacheFree(lc);
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    size_t rd = fread(b, 1, n, f); b[rd] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: find <query.json> [range] [threads]\n"
            "       find <query.json> --explain [samples] [threads]   estimate before searching\n"
            "       find <query.json> --bias    [samples] [threads]   is scanning from 0 representative?\n");
        return 2;
    }
    int explain = (argc > 2) && !strcmp(argv[2], "--explain");
    int bias    = (argc > 2) && !strcmp(argv[2], "--bias");
    uint64_t range   = (argc > 2 && !explain && !bias) ? strtoull(argv[2], NULL, 10) : 5000000;
    uint64_t samples = ((explain || bias) && argc > 3) ? strtoull(argv[3], NULL, 10) : 200000;
    int nthreads = (explain || bias) ? ((argc > 4) ? atoi(argv[4]) : 16)
                                     : ((argc > 3) ? atoi(argv[3]) : 16);

    char *json = slurp(argv[1]);
    if (!json) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    Query q; char err[256];
    if (queryParse(&q, json, err, sizeof err)) { fprintf(stderr, "query error: %s\n", err); return 1; }
    if (queryPlan(&q, err, sizeof err))        { fprintf(stderr, "plan error: %s\n", err); return 1; }

    char buf[96];
    printf("query   : %d conditions, MC %s\n", q.n, mc2str(q.mc));
    for (int i = 0; i < q.n; i++)
        printf("   [%s] %s\n", q.cond[i].id, condDesc(&q, i, buf, sizeof buf));
    printf("\n");
    queryPrintPlan(&q, stdout);

    // Warn when a condition uses a structure the engine cannot verify. Its
    // position is exact, but whether the game actually places anything there
    // is unmodelled -- which looks exactly like a wrong answer in game.
    for (int i = 0; i < q.n; i++) {
        if (q.cond[i].type != CT_STRUCTURE) continue;
        for (int k = 0; k < queryStructureCount(); k++) {
            if (queryStructureType(k) != q.cond[i].structType) continue;
            if (queryStructureVerified(k)) continue;
            printf("NOTE: \"%s\" is a %s. The engine reports EVERY generation attempt\n"
                   "      for these without checking whether the game actually places\n"
                   "      one, so a reported hit may not exist in your world.\n"
                   "      Measured: 100%% of attempts pass, vs 0.9-31.7%% for\n"
                   "      biome-checked structures like village/mansion/monument.\n\n",
                   q.cond[i].id, queryStructureName(k));
        }
    }

    if (bias)    { explainCompare(&q, samples, nthreads, stdout); free(json); return 0; }
    if (explain) { explainQuery(&q, samples, nthreads, stdout);   free(json); return 0; }

    InitializeCriticalSection(&g_lock);
    Job *jobs = calloc(nthreads, sizeof(Job));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));
    uint64_t chunk = range / nthreads;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    for (int i = 0; i < nthreads; i++) {
        jobs[i].q = &q;
        jobs[i].lo = (uint64_t)i * chunk;
        jobs[i].hi = (i == nthreads-1) ? range : (uint64_t)(i+1) * chunk;
        th[i] = CreateThread(NULL, 0, worker, &jobs[i], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double el = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;

    FILE *tsv = getenv("FIND_TSV") ? fopen(getenv("FIND_TSV"), "wb") : NULL;
    uint64_t sc=0, p1=0, p2=0, ap=0; int shown = 0;
    for (int i = 0; i < nthreads; i++) {
        sc += jobs[i].scanned; p1 += jobs[i].pass1; p2 += jobs[i].pass2; ap += jobs[i].applies;
    }
    for (int i = 0; i < nthreads && shown < MAX_HITS; i++)
        for (int k = 0; k < jobs[i].nhits && shown < MAX_HITS; k++, shown++) {
            Hit *h = &jobs[i].hits[k];
            printf("SEED %" PRId64 "\n", (int64_t)h->ws);
            if (h->m.haveSpawn)
                printf("   %-14s x=%6d z=%6d\n", "(spawn)", h->m.spawn.x, h->m.spawn.z);
            if (h->m.haveEyes)
                printf("   %-14s x=%6d z=%6d   %d/%d eyes\n", "(end portal)",
                       h->m.stronghold.x, h->m.stronghold.z, h->m.eyes, EYE_FRAMES);
            for (int c = 0; c < q.n; c++) {
                if (q.cond[c].type == CT_LOOT) {
                    const char *it = q.cond[c].lootItem;
                    if (!strncmp(it, "minecraft:", 10)) it += 10;
                    printf("   %-14s x=%6d z=%6d   %d %s\n", q.cond[c].id,
                           h->m.pos[c].x, h->m.pos[c].z, h->m.lootCount[c], it);
                    continue;
                }
                if (q.cond[c].type == CT_ORE) {
                    int nm; const OreMaterial *tab = oreMaterials(&nm);
                    printf("   %-14s x=%6d z=%6d   %d %s ore within %d\n", q.cond[c].id,
                           h->m.pos[c].x, h->m.pos[c].z, h->m.oreCount[c],
                           tab[q.cond[c].oreMat].name, q.cond[c].within);
                    continue;
                }
                if (q.cond[c].type == CT_SLIME) {
                    printf("   %-14s x=%6d z=%6d   %d slime chunks within %d\n", q.cond[c].id,
                           h->m.pos[c].x, h->m.pos[c].z, h->m.slimeCount[c],
                           q.cond[c].within);
                    continue;
                }
                if (q.cond[c].type == CT_BIOME_AREA) {
                    int tot = h->m.areaTotal[c], mat = h->m.areaCells[c];
                    int pct = tot > 0 ? (int)((int64_t)mat * 100 / tot) : 0;
                    printf("   %-14s x=%6d z=%6d   %d%% %s within %d\n", q.cond[c].id,
                           h->m.pos[c].x, h->m.pos[c].z, pct,
                           biome2str(q.mc, q.cond[c].biomeId), q.cond[c].within);
                    continue;
                }
                if (q.cond[c].type != CT_STRUCTURE) continue;
                Pos p = h->m.pos[c];
                // Say which reference the distance is measured from: "spawn"
                // and "origin" are different places (median 22 blocks apart,
                // p90 520), and labelling one as the other is how a correct
                // search still produces results that look wrong in game.
                int spawnRel = (q.cond[c].parent == PARENT_SPAWN && h->m.haveSpawn);
                int64_t dx = p.x - (spawnRel ? h->m.spawn.x : 0);
                int64_t dz = p.z - (spawnRel ? h->m.spawn.z : 0);
                printf("   %-14s x=%6d z=%6d   %d %s\n", q.cond[c].id, p.x, p.z,
                       (int)sqrt((double)(dx*dx + dz*dz)),
                       spawnRel ? "from spawn" : "from origin");
            }
            // machine-readable twin, for piping into verifiers
            if (tsv) {
                fprintf(tsv, "%" PRId64, (int64_t)h->ws);
                for (int c = 0; c < q.n; c++) {
                    // Anything with a matched position: structures and loot.
                    if (q.cond[c].type != CT_STRUCTURE && q.cond[c].type != CT_LOOT)
                        continue;
                    fprintf(tsv, "\t%s\t%d\t%d", q.cond[c].id, h->m.pos[c].x, h->m.pos[c].z);
                }
                fprintf(tsv, "\n");
            }
            printf("\n");
        }

    double r1 = 100.0 * p1 / (sc ? sc : 1);
    printf("--- funnel ---\n");
    printf("scanned    : %" PRIu64 " structure seeds in %.2fs\n", sc, el);
    printf("pass1 (48b): %" PRIu64 "  (%.4f%% survive)\n", p1, r1);
    printf("applySeed  : %" PRIu64 "\n", ap);
    printf("pass2 (64b): %" PRIu64 "  full world seeds\n", p2);
    printf("throughput : %.2f M structure-seeds/s\n", sc / el / 1e6);
    if (r1 > 50.0)
        printf("\nWARNING: pass 1 rejects almost nothing (%.1f%% survive). The query is too\n"
               "         loose to filter, so every seed pays full biome cost. Tighten radii.\n", r1);
    if (tsv) fclose(tsv);
    free(json);
    return shown ? 0 : 3;
}
