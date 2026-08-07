// find -- run a JSON condition-tree query through the planner and search.
//
//   usage: find <query.json> [range] [threads]
//
// Prints the PLAN before searching, so the ordering the planner chose is
// auditable, and reports the pass-1 survival rate afterwards -- the single
// number that tells you whether the query is selective enough to be fast.
#include "query.h"
#include "solve.h"
#include "loot.h"
#include "ore.h"
#include "explain.h"
#include "climate.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>
#include <windows.h>

#define MAX_HITS      12
#define UPPER_SAMPLES 64
#define MAX_RANK      50

typedef struct { uint64_t ws; Match m; } Hit;

// --- leaderboard ---------------------------------------------------------
//
// A ranked query does not stop when it has enough hits: "the best in N seeds"
// is only meaningful if all N are actually scanned. So the search runs the full
// range and keeps a small sorted table of the best scores seen. The table is
// tiny (<= 50) and writes are rare (a new entry needs to beat the current
// floor), so one lock around it costs nothing measurable.
static CRITICAL_SECTION g_lock;

typedef struct { uint64_t ws; int score; Match m; } Ranked;
static Ranked g_rank[MAX_RANK];
static int    g_nrank = 0;
static int    g_rankFloor = INT_MIN;   // score to beat once the table is full

static void rankOffer(const Query *q, uint64_t ws, const Match *m, int score)
{
    if (g_nrank >= q->rankTop && score <= g_rankFloor) return;   // unlocked fast path
    EnterCriticalSection(&g_lock);
    if (g_nrank < q->rankTop || score > g_rank[g_nrank-1].score) {
        int i = (g_nrank < q->rankTop) ? g_nrank++ : g_nrank - 1;
        for (; i > 0 && g_rank[i-1].score < score; i--) g_rank[i] = g_rank[i-1];
        g_rank[i].ws = ws; g_rank[i].score = score; g_rank[i].m = *m;
        if (g_nrank >= q->rankTop) g_rankFloor = g_rank[g_nrank-1].score;
    }
    LeaveCriticalSection(&g_lock);
}

typedef struct {
    const Query *q;
    uint64_t lo, hi;
    // When the query has a tight-cluster condition, `cand` holds structure seeds
    // that src/solve.c already knows satisfy its geometry, and [lo,hi) indexes
    // that list instead of counting seeds. Everything downstream is unchanged:
    // pass 1 still evaluates each one, so the solver decides what is TRIED, never
    // what is a hit.
    const uint64_t *cand;
    uint64_t scanned, pass1, pass2, applies, climgate, climskip;
    Hit hits[MAX_HITS];
    int nhits;
} Job;

static volatile LONG g_found = 0;
static volatile LONG64 g_scanned = 0;   // total seeds scanned across all threads
static int g_want = MAX_HITS;
static uint64_t g_offset = 0;   // FIND_OFFSET: index base, so runs can resume
// Stream mode (env FIND_STREAM): emit "@TICK <seed>" heartbeats so the web UI
// can show the seeds being scanned scroll past live. Time-throttled per thread
// so the rate is readable regardless of how fast the query scans.
static int g_stream = 0;
static int g_hitstream = 0;   // FIND_HITSTREAM: print every hit, uncapped

// Time budget (env FIND_SECONDS). A range is a poor way to ask for "keep
// looking while I make coffee": how many seeds that buys depends entirely on
// the query, and the interesting queries are the slow ones. A deadline lets the
// caller spend an amount of TIME and take whatever the search found by then --
// which is what a leaderboard wants, since it improves the longer it runs.
static ULONGLONG g_deadline = 0;          // 0 = no limit
static volatile LONG g_expired = 0;

static inline int outOfTime(uint64_t scanned)
{
    // Checked once every 1024 seeds: GetTickCount64 is cheap but not free, and
    // the fastest queries run millions of seeds a second.
    if (!g_deadline || (scanned & 1023)) return 0;
    if (GetTickCount64() < g_deadline) return 0;
    InterlockedExchange(&g_expired, 1);
    return 1;
}

// A bijection on 64 bits (splitmix64's finaliser). Whole-seed mode walks an
// index and maps it through this, so a scan of N seeds is N DISTINCT world
// seeds spread across the whole range rather than 0,1,2,... -- which would be a
// corner of the space, and would look like it too.
static inline uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job*)arg;
    Generator g;
    setupGenerator(&g, j->q->mc, queryGenFlags(j->q));
    LootCache *lc = lootCacheNew();   // one per thread; loot tables are stateful
    ULONGLONG lastTick = 0;
    uint64_t  lastRep  = 0;   // this thread's scanned count at its last tick

    // With no structure conditions there is no 48-bit geometry to filter on, so
    // the two-pass funnel has nothing to do: every structure seed survives and
    // the upper bits are sampled blind. Walk WORLD seeds directly instead. One
    // index is then one world, which is both faster and the only way "best of N
    // seeds" means what it says.
    int wholeSeed = (j->q->ngeom == 0);
    if (wholeSeed) {
        for (uint64_t i = j->lo; i < j->hi; i++) {
            if (g_found >= g_want) break;
            if (outOfTime(j->scanned)) break;
            j->scanned++;
            uint64_t ws = mix64(i);

            if (g_stream) {
                ULONGLONG now = GetTickCount64();
                if (now - lastTick >= 55) {
                    lastTick = now;
                    LONG64 tot = InterlockedAdd64(&g_scanned, (LONG64)(j->scanned - lastRep));
                    lastRep = j->scanned;
                    EnterCriticalSection(&g_lock);
                    printf("@TICK %" PRIu64 " %lld\n", ws, (long long)tot);
                    fflush(stdout);
                    LeaveCriticalSection(&g_lock);
                }
            }

            Match m;
            queryStage1(j->q, ws & ((1ULL << 48) - 1), &m);   // zeroes the match
            j->pass1++; j->applies++;
            if (!queryStage2(j->q, &g, ws, &m, lc)) continue;
            j->pass2++;

            if (j->q->rankTop > 0) {
                rankOffer(j->q, ws, &m, queryScore(j->q, &m));
                InterlockedIncrement(&g_found);
                continue;
            }
            EnterCriticalSection(&g_lock);
            if (j->nhits < MAX_HITS) { j->hits[j->nhits].ws = ws; j->hits[j->nhits].m = m; j->nhits++; }
            if (g_hitstream) { printf("HIT %lld %d %d\n", (long long)ws, m.pos[0].x, m.pos[0].z); fflush(stdout); }
            InterlockedIncrement(&g_found);
            LeaveCriticalSection(&g_lock);
        }
        lootCacheFree(lc);
        return 0;
    }

    for (uint64_t idx = j->lo; idx < j->hi; idx++) {
        uint64_t s48 = j->cand ? j->cand[idx] : idx;
        if (g_found >= g_want) break;
        if (outOfTime(j->scanned)) break;
        j->scanned++;

        if (g_stream) {
            ULONGLONG now = GetTickCount64();
            if (now - lastTick >= 55) {
                lastTick = now;
                // Roll this thread's progress since its last tick into a shared
                // total, so the UI can show ONE monotonic "N scanned" figure
                // instead of 16 thread positions bouncing around.
                LONG64 tot = InterlockedAdd64(&g_scanned, (LONG64)(j->scanned - lastRep));
                lastRep = j->scanned;
                EnterCriticalSection(&g_lock);
                printf("@TICK %" PRIu64 " %lld\n", s48, (long long)tot);
                fflush(stdout);
                LeaveCriticalSection(&g_lock);
            }
        }

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

            // Ranked search: every survivor is a leaderboard candidate, and the
            // scan keeps going. Breaking here (rather than trying more upper
            // bits) keeps one structure seed from flooding the table with 64
            // near-identical variants of the same world.
            if (j->q->rankTop > 0) {
                rankOffer(j->q, ws, &m, queryScore(j->q, &m));
                InterlockedIncrement(&g_found);
                break;
            }

            EnterCriticalSection(&g_lock);
            if (j->nhits < MAX_HITS) { j->hits[j->nhits].ws = ws; j->hits[j->nhits].m = m; j->nhits++; }
            // Stream mode (FIND_HITSTREAM): emit EVERY hit as "HIT <seed> <x> <z>"
            // (condition 0's matched position), uncapped -- for tier-2 pipelines
            // that need thousands of candidates, not the 12 the array holds.
            if (g_hitstream) { printf("HIT %lld %d %d\n", (long long)ws, m.pos[0].x, m.pos[0].z); fflush(stdout); }
            InterlockedIncrement(&g_found);
            LeaveCriticalSection(&g_lock);
            break;
        }
    }
    j->climgate = climateGated();     // thread-local, so read them here
    j->climskip = climateSkipped();
    lootCacheFree(lc);
    return 0;
}

// One result, in the human-readable form, plus its machine-readable twin.
// Shared by the plain search and the leaderboard so the two never drift.
static void printHit(const Query *q, uint64_t ws, const Match *mm, FILE *tsv)
{
    if (mm->haveSpawn)
        printf("   %-14s x=%6d z=%6d\n", "(spawn)", mm->spawn.x, mm->spawn.z);
    if (mm->haveEyes)
        printf("   %-14s x=%6d z=%6d   %d/%d eyes\n", "(end portal)",
               mm->stronghold.x, mm->stronghold.z, mm->eyes, EYE_FRAMES);
    for (int c = 0; c < q->n; c++) {
        const Cond *cd = &q->cond[c];
        if (cd->type == CT_LOOT) {
            const char *it = cd->lootItem;
            if (!strncmp(it, "minecraft:", 10)) it += 10;
            printf("   %-14s x=%6d z=%6d   %d %s\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->lootCount[c], it);
            continue;
        }
        if (cd->type == CT_OVERLAP) {
            // Both halves, because the whole point is the pair -- and the
            // separation, which is what you check first in game.
            int dx = mm->partner[c].x - mm->pos[c].x;
            int dz = mm->partner[c].z - mm->pos[c].z;
            printf("   %-14s x=%6d z=%6d   %s\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, struct2str(cd->structType));
            printf("   %-14s x=%6d z=%6d   %s, %d blocks away, %d overlap\n", "",
                   mm->partner[c].x, mm->partner[c].z, struct2str(cd->structType2),
                   (int)sqrt((double)(dx*dx + dz*dz)), mm->overlapArea[c]);
            continue;
        }
        if (cd->type == CT_CACTUS) {
            printf("   %-14s x=%6d z=%6d   %d-block cactus, base y=%d\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->cactusTall[c], mm->cactusBase[c]);
            continue;
        }
        if (cd->type == CT_ORE) {
            int nm; const OreMaterial *tab = oreMaterials(&nm);
            printf("   %-14s x=%6d z=%6d   %d %s%s ore within %d", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->oreCount[c],
                   tab[cd->oreMat].name, cd->oreExposed ? " exposed" : "",
                   cd->within);
            if (mm->veinMax[c] > 0) printf(", biggest vein %d", mm->veinMax[c]);
            printf("\n");
            continue;
        }
        if (cd->type == CT_SLIME) {
            printf("   %-14s x=%6d z=%6d   %d slime chunks within %d\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->slimeCount[c], cd->within);
            continue;
        }
        if (cd->type == CT_HEIGHT) {
            const char *tag = cd->exactTerrain ? "" : "~";
            if (cd->reliefMin > 0)
                printf("   %-14s x=%6d z=%6d   %s%d peak, %s%d relief within %d\n",
                       cd->id, mm->pos[c].x, mm->pos[c].z,
                       tag, mm->peakHeight[c], tag, mm->peakDrop[c], cd->within);
            else
                printf("   %-14s x=%6d z=%6d   %s%d peak within %d\n", cd->id,
                       mm->pos[c].x, mm->pos[c].z, tag, mm->peakHeight[c], cd->within);
            continue;
        }
        if (cd->type == CT_BIOME_AREA) {
            int tot = mm->areaTotal[c], mat = mm->areaCells[c];
            int pct = tot > 0 ? (int)((int64_t)mat * 100 / tot) : 0;
            printf("   %-14s x=%6d z=%6d   %d%% %s within %d\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, pct,
                   biome2str(q->mc, cd->biomeId), cd->within);
            continue;
        }
        if (cd->type == CT_ISLAND) {
            int tot = mm->areaTotal[c], oc = mm->areaCells[c];
            int pct = tot > 0 ? (int)((int64_t)oc * 100 / tot) : 0;
            printf("   %-14s x=%6d z=%6d   island, %d%% ocean within %d\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, pct, cd->within);
            continue;
        }
        if (cd->type == CT_BIOME) {
            // Biomes record where they matched; report it with the distance
            // from their reference so adjacency is legible.
            Pos bp = mm->pos[c];
            int par = cd->parent;
            Pos ref = (par == PARENT_SPAWN) ? mm->spawn
                    : (par >= 0) ? mm->pos[par] : (Pos){0,0};
            const char *rl = (par == PARENT_SPAWN) ? "spawn"
                           : (par >= 0) ? q->cond[par].id : "origin";
            int64_t bdx = bp.x - ref.x, bdz = bp.z - ref.z;
            printf("   %-14s x=%6d z=%6d   %s %d from %s\n", cd->id,
                   bp.x, bp.z, biome2str(q->mc, cd->biomeId),
                   (int)sqrt((double)(bdx*bdx + bdz*bdz)), rl);
            continue;
        }
        if (cd->type != CT_STRUCTURE) continue;
        if (cd->structMin > 1) {
            // Anchored cluster: N within the radius, centred here. Tight
            // cluster: N within `spread` of the reported member.
            if (cd->spread > 0)
                printf("   %-14s x=%6d z=%6d   %d %s within %d tight\n", cd->id,
                       mm->pos[c].x, mm->pos[c].z, mm->structCount[c],
                       struct2str(cd->structType), cd->spread);
            else
                printf("   %-14s x=%6d z=%6d   %d %s within %d\n", cd->id,
                       mm->pos[c].x, mm->pos[c].z, mm->structCount[c],
                       struct2str(cd->structType), cd->within);
            continue;
        }
        Pos p = mm->pos[c];
        // Say which reference the distance is measured from: "spawn" and
        // "origin" are different places (median 22 blocks apart, p90 520), and
        // labelling one as the other is how a correct search still produces
        // results that look wrong in game.
        int spawnRel = (cd->parent == PARENT_SPAWN && mm->haveSpawn);
        int64_t dx = p.x - (spawnRel ? mm->spawn.x : 0);
        int64_t dz = p.z - (spawnRel ? mm->spawn.z : 0);
        printf("   %-14s x=%6d z=%6d   %d %s", cd->id, p.x, p.z,
               (int)sqrt((double)(dx*dx + dz*dz)),
               spawnRel ? "from spawn" : "from origin");
        if (cd->structType == Geode && mm->geodeSize[c] > 0)
            printf(", size %d", mm->geodeSize[c]);
        if (mm->caveHeight[c] > 0)
            printf(", %d-block cave below", mm->caveHeight[c]);
        printf("\n");
    }
    // machine-readable twin, for piping into verifiers
    if (tsv) {
        fprintf(tsv, "%" PRId64, (int64_t)ws);
        for (int c = 0; c < q->n; c++) {
            // Anything with a matched position: structures, loot, overlaps.
            if (q->cond[c].type != CT_STRUCTURE && q->cond[c].type != CT_LOOT &&
                q->cond[c].type != CT_OVERLAP)
                continue;
            fprintf(tsv, "\t%s\t%d\t%d", q->cond[c].id, mm->pos[c].x, mm->pos[c].z);
            if (q->cond[c].type == CT_OVERLAP)
                fprintf(tsv, "\t%s.b\t%d\t%d", q->cond[c].id,
                        mm->partner[c].x, mm->partner[c].z);
        }
        fprintf(tsv, "\n");
    }
    printf("\n");
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

    const char *secs = getenv("FIND_SECONDS");
    if (secs && atof(secs) > 0) {
        g_deadline = GetTickCount64() + (ULONGLONG)(atof(secs) * 1000.0);
        printf("time budget: %.0f seconds -- the scan stops when it expires, "
               "whatever it has found by then\n", atof(secs));
    }
    // FIND_OFFSET: where in the index space to start. Without it every run
    // walks indices 0..range and therefore rescans the SAME seeds, so "search
    // longer" meant "search the same ground again, slower". A caller that wants
    // to keep looking advances the offset by the range it just covered, and each
    // batch is fresh seeds. The index goes through mix64 either way, so a later
    // batch is no more a corner of the space than the first one.
    const char *off = getenv("FIND_OFFSET");
    g_offset = off ? strtoull(off, NULL, 10) : 0;
    if (g_offset)
        printf("offset     : starting at index %llu -- these are seeds a run "
               "from 0 would not reach\n", (unsigned long long)g_offset);
    g_stream = getenv("FIND_STREAM") != NULL;
    g_hitstream = getenv("FIND_HITSTREAM") != NULL;
    if (g_hitstream) g_want = 0x7fffffff;   // don't stop early; stream them all
    // A leaderboard is only as good as the range it covers: stopping at the
    // first N survivors would report "the best of the first few", which is the
    // one thing a record search must not do.
    if (q.rankTop > 0) {
        g_want = 0x7fffffff;
        printf("ranking the top %d by %s over %" PRIu64 " %s (%s)\n\n",
               q.rankTop, queryScoreLabel(&q), range,
               q.ngeom == 0 ? "world seeds" : "structure seeds",
               g_deadline ? "or until the time budget runs out -- the funnel "
                            "below says how many were actually scanned"
                          : "the full range is scanned -- no early stop");
    }
    // ---- solve instead of scan, where the query allows it -----------------
    // A tight cluster is about one seed in 10^11, so scanning spends all of its
    // budget on seeds that were never going to work. src/solve.c enumerates the
    // ones that do and hands them over as candidates; the budget then buys that
    // many CANDIDATES rather than that many consecutive seeds. Pass 1 still
    // judges every one, so this changes what gets tried, not what counts.
    uint64_t *cand = NULL;
    uint64_t ncand = 0;
    {
        char why[256];
        // FIND_NOSOLVE forces the plain scan, so the two paths can be compared.
        int sk = (q.ngeom > 0 && !getenv("FIND_NOSOLVE")) ? solvePick(&q, why, sizeof why) : -1;
        if (sk >= 0) {
            // A solved candidate is worth orders of magnitude more than a
            // scanned seed, so a huge `range` does not need a huge list -- and
            // asking for one would just fail the allocation. 20M is already far
            // more candidates than any cluster query gets through.
            // (capped separately from `range`, so falling back to the scan
            // still honours the budget the caller actually asked for)
            uint64_t capN = range < 20000000ULL ? range : 20000000ULL;
            cand = malloc((size_t)capN * sizeof(uint64_t));
            if (!cand) {
                printf("solver: %s -- but the candidate list would not fit; scanning\n\n", why);
            } else {
                // A loose cluster gives the low-half sieve nothing to reject, so
                // the solver grinds while the plain scan would already be
                // finding hits. Which case this is depends on the true cluster
                // rate, not on anything cheap to compute -- so measure it: build
                // under a budget and judge by the yield.
                double budget = 20.0;
                int timedOut = 0;
                LARGE_INTEGER s0, s1, sf;
                QueryPerformanceFrequency(&sf); QueryPerformanceCounter(&s0);
                ncand = solveFill(&q, sk, capN, cand, nthreads, budget, &timedOut);
                QueryPerformanceCounter(&s1);
                double sel = (double)(s1.QuadPart - s0.QuadPart) / sf.QuadPart;

                if (ncand == 0 && !timedOut) {
                    // Not "found nothing yet" -- the solver walked the whole
                    // 2^48 space. Scanning for this could only ever have run
                    // until the user gave up.
                    printf("solver: %s\n        searched ALL 2^48 structure seeds in %.1fs: "
                           "there are NONE.\n        Scanning would have looked forever and "
                           "found nothing.\n\n", why, sel);
                    free(cand); cand = NULL;
                    range = 0;
                } else if (ncand < 1000) {
                    printf("solver: %s\n        but only %" PRIu64 " candidates in %.1fs, so this "
                           "target is common enough that scanning\n        beats solving it; "
                           "scanning instead\n\n", why, ncand, sel);
                    free(cand); cand = NULL;
                } else {
                    printf("solver: %s\n", why);
                    printf("        %" PRIu64 " candidates in %.1fs -- every one already places "
                           "the cluster, so the\n        budget buys candidates instead of "
                           "consecutive seeds\n\n", ncand, sel);
                    range = ncand;
                }
            }
        } else if (q.ngeom > 0 && strstr(why, "solving") == NULL && strstr(why, "no tight") == NULL) {
            printf("solver: not used -- %s\n\n", why);
        }
    }

    InitializeCriticalSection(&g_lock);
    Job *jobs = calloc(nthreads, sizeof(Job));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));
    uint64_t chunk = range / nthreads;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    for (int i = 0; i < nthreads; i++) {
        jobs[i].q = &q;
        jobs[i].cand = cand;
        jobs[i].lo = (uint64_t)i * chunk;
        jobs[i].hi = (i == nthreads-1) ? range : (uint64_t)(i+1) * chunk;
        // The offset shifts the INDEX walk only. When `cand` is set, [lo,hi)
        // indexes the solver's candidate array rather than counting seeds, so
        // shifting it would read off the end of that array -- a resumable search
        // and a solved one are different things and must not be mixed.
        if (!cand) { jobs[i].lo += g_offset; jobs[i].hi += g_offset; }
        th[i] = CreateThread(NULL, 0, worker, &jobs[i], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    QueryPerformanceCounter(&t1);
    double el = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;

    FILE *tsv = getenv("FIND_TSV") ? fopen(getenv("FIND_TSV"), "wb") : NULL;
    uint64_t sc=0, p1=0, p2=0, ap=0, cd=0, cg=0; int shown = 0;
    for (int i = 0; i < nthreads; i++) {
        sc += jobs[i].scanned; p1 += jobs[i].pass1; p2 += jobs[i].pass2; ap += jobs[i].applies;
        cg += jobs[i].climgate; cd += jobs[i].climskip;
    }
    if (q.rankTop > 0) {
        // Leaderboard: the whole range was scanned, so these really are the
        // best seen -- not merely the first that cleared a threshold.
        printf("--- leaderboard: top %d by %s ---\n\n", g_nrank, queryScoreLabel(&q));
        for (int i = 0; i < g_nrank; i++, shown++) {
            printf("SEED %" PRId64 "   %s = %d\n", (int64_t)g_rank[i].ws,
                   queryScoreLabel(&q), g_rank[i].score);
            printHit(&q, g_rank[i].ws, &g_rank[i].m, tsv);
        }
    } else {
        for (int i = 0; i < nthreads && shown < MAX_HITS; i++)
            for (int k = 0; k < jobs[i].nhits && shown < MAX_HITS; k++, shown++) {
                Hit *h = &jobs[i].hits[k];
                printf("SEED %" PRId64 "\n", (int64_t)h->ws);
                printHit(&q, h->ws, &h->m, tsv);
            }
    }

    double r1 = 100.0 * p1 / (sc ? sc : 1);
    int wholeSeed = (q.ngeom == 0);
    printf("--- funnel ---\n");
    // Without this line a budgeted run is indistinguishable from a tiny range,
    // and "best of N seeds" would quietly mean a different N than requested.
    if (g_expired)
        printf("stopped    : time budget expired before the range was exhausted\n");
    if (wholeSeed) {
        // No geometry pass exists here, so reporting a survival rate would be
        // theatre: it is 100% by construction.
        printf("scanned    : %" PRIu64 " world seeds in %.2fs  (no structure conditions,\n"
               "             so world seeds are walked directly -- one seed, one world)\n", sc, el);
        printf("matched    : %" PRIu64 "\n", p2);
        printf("throughput : %.2f seeds/s\n", sc / (el > 0 ? el : 1));
    } else {
    printf("scanned    : %" PRIu64 " structure seeds in %.2fs\n", sc, el);
    printf("pass1 (48b): %" PRIu64 "  (%.4f%% survive)\n", p1, r1);
    printf("applySeed  : %" PRIu64 "\n", ap);
    if (cg)
        printf("temp gate  : %" PRIu64 " of %" PRIu64 " biome lookups skipped on\n"
               "             temperature alone (%.1f%%)\n",
               cd, cg, 100.0 * cd / cg);
    printf("pass2 (64b): %" PRIu64 "  full world seeds\n", p2);
    printf("throughput : %.2f M structure-seeds/s\n", sc / el / 1e6);
    if (r1 > 50.0)
        printf("\nWARNING: pass 1 rejects almost nothing (%.1f%% survive). The query is too\n"
               "         loose to filter, so every seed pays full biome cost. Tighten radii.\n", r1);
    }
    if (tsv) fclose(tsv);
    free(json);
    return shown ? 0 : 3;
}
