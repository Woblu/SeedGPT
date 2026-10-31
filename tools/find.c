// find -- run a JSON condition-tree query through the planner and search.
//
//   usage: find <query.json> [range] [threads]
//
// Prints the PLAN before searching, so the ordering the planner chose is
// auditable, and reports the pass-1 survival rate afterwards -- the single
// number that tells you whether the query is selective enough to be fast.
#include "query.h"
#include "anyof.h"
#include "solve.h"
#include "loot.h"
#include "ore.h"
#include "explain.h"
#include "climate.h"
#include "util.h"
#include "seed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>
#include "platform.h"

#define MAX_HITS      12
// How many of a structure seed's 65536 sister world seeds to try before giving
// up on it. Structure placement and loot are fixed by the lower 48 bits; biomes
// and world spawn need the full 64, so a biome-heavy query is really asking
// "does ANY sister of this structure seed work" and this is how hard we look.
//
// 64 of 65536 is a deliberate trade -- more structure seeds against more sisters
// each -- and it is the right one when the biome part of a query is common. It
// is the WRONG one when the structure part is common and the biome part is rare:
// then most structure seeds are viable, the answer lives in a sister we never
// tried, and the search reports nothing found. That is indistinguishable from
// "no such seed exists", which is this project's most-repeated failure.
//
// FIND_UPPERS raises it. A seed-finding course tunes exactly this number by hand
// between 256 and 4000 depending on how rare its biome conditions are.
#define UPPER_SAMPLES_DEFAULT 64
static int g_uppers = UPPER_SAMPLES_DEFAULT;
#define MAX_RANK      50

// q: the alternative that held (see anyof.h). A Match means nothing without it.
typedef struct { uint64_t ws; Match m; const Query *q; } Hit;

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
    const Query *plans;        // a seed matches if ANY of these does; almost always 1
    int nplans;
    uint64_t lo, hi, stride;   // indices lo, lo+stride, ... below hi
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

// 64-bit: a permissive hit stream passes 2^31 matches in about an hour, and a
// 32-bit counter then wraps negative and every thread stops on "found enough"
// without a word.
static volatile LONG64 g_found = 0;
static volatile LONG64 g_scanned = 0;   // total seeds scanned across all threads
static LONG64 g_want = MAX_HITS;
static uint64_t g_offset = 0;   // FIND_OFFSET: index base, so runs can resume
// Stream mode (env FIND_STREAM): emit "@TICK <seed>" heartbeats so the web UI
// can show the seeds being scanned scroll past live. Time-throttled per thread
// so the rate is readable regardless of how fast the query scans.
static int g_stream = 0;
static int g_hitstream = 0;   // FIND_HITSTREAM: print every hit, uncapped

static void printHit(const Query *q, uint64_t ws, const Match *mm, FILE *tsv);
static void writeTsv(const Query *q, uint64_t ws, const Match *mm, FILE *tsv);
static void jsonHit(const Query *q, uint64_t ws, const Match *mm, int haveScore, int score);
static int g_json = 0;     // FIND_JSON: also print each hit as an @JSON line
static char *slurp(const char *path);
static int checkSeedList(const Query *plans, int nplans, const char *path, int nthreads);

static int anyWholeSeed(const Query *plans, int nplans)
{
    for (int p = 0; p < nplans; p++)
        if (plans[p].ngeom == 0) return 1;
    return 0;
}

// In stream mode a hit is printed the moment it is found, in the same SEED
// block the end-of-run report uses, so a reader can show it immediately and --
// the part that matters -- keep it when the search is stopped or killed. Before
// this, results existed only in the final report, and stopping a 30-minute run
// at minute 29 threw away everything it had found. Caller holds g_lock.
static void emitHit(const Query *q, uint64_t ws, const Match *m)
{
    printf("SEED %" PRId64 "\n", (int64_t)ws);
    printHit(q, ws, m, NULL);
    if (g_json) jsonHit(q, ws, m, 0, 0);
    fflush(stdout);
}

// Time budget (env FIND_SECONDS). A range is a poor way to ask for "keep
// looking while I make coffee": how many seeds that buys depends entirely on
// the query, and the interesting queries are the slow ones. A deadline lets the
// caller spend an amount of TIME and take whatever the search found by then --
// which is what a leaderboard wants, since it improves the longer it runs.
static ULONGLONG g_deadline = 0;          // 0 = no limit
static volatile LONG g_expired = 0;

// How often the deadline is looked at, ADAPTED to what a seed actually costs.
//
// This used to be a flat "every 1024 seeds", on the reasoning that
// GetTickCount64 is cheap but not free and the fastest queries run millions of
// seeds a second. That holds only while a seed is cheap. A stronghold overlap
// costs ~4 s per structure seed -- the ring walk is re-run for every world seed,
// since initFirstStronghold takes the WORLD seed -- so a thread could not notice
// the budget until it had finished 1024 of them. Measured: FIND_SECONDS=90 ran
// for 3867 s. A time budget that overshoots by 40x is not a budget, and the
// machine it overruns on is someone's.
//
// So the interval starts at "every seed" and only relaxes once cheap seeds have
// proved themselves. Getting this backwards is what made the bug: the expensive
// case is the one that cannot afford to wait for the first checkpoint, and it is
// also the one where a per-seed GetTickCount64 costs nothing in relative terms.
// A fast query reaches the old 1023 interval within ten seeds.
static _Thread_local uint64_t tl_chkMask = 0;
static _Thread_local ULONGLONG tl_lastChk = 0;

static inline int outOfTime(uint64_t scanned)
{
    if (!g_deadline) return 0;
    if (scanned & tl_chkMask) return 0;
    ULONGLONG now = GetTickCount64();
    if (tl_lastChk) {
        ULONGLONG d = now - tl_lastChk;
        if (d > 250) { tl_chkMask >>= 1; }
        else if (d < 25 && tl_chkMask < 1023) { tl_chkMask = (tl_chkMask << 1) | 1; }
    }
    tl_lastChk = now;
    if (now < g_deadline) return 0;
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
    const Query *plans = j->plans;
    // Version and world type belong to the whole query, so every alternative
    // shares them and one generator serves all of them.
    const Query *q0 = plans;
    Generator g;
    setupGenerator(&g, q0->mc, queryGenFlags(q0));
    LootCache *lc = lootCacheNew();   // one per thread; loot tables are stateful
    ULONGLONG lastTick = 0;
    uint64_t  lastRep  = 0;   // this thread's scanned count at its last tick

    // With no structure conditions there is no 48-bit geometry to filter on, so
    // the two-pass funnel has nothing to do: every structure seed survives and
    // the upper bits are sampled blind. Walk WORLD seeds directly instead. One
    // index is then one world, which is both faster and the only way "best of N
    // seeds" means what it says. One such alternative decides it for all of them.
    int wholeSeed = anyWholeSeed(plans, j->nplans);
    if (wholeSeed) {
        for (uint64_t i = j->lo; i < j->hi; i += j->stride) {
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
            const Query *held = NULL;
            j->pass1++;
            for (int p = 0; p < j->nplans && !held; p++) {
                // Stage 1 zeroes the match; a structure alternative in a mixed
                // query also has to pass it, since nothing filtered on it here.
                const Query *q = &plans[p];
                int s1 = queryStage1(q, ws & ((1ULL << 48) - 1), &m);
                if (q->ngeom && !s1) continue;
                j->applies++;
                if (queryStage2(q, &g, ws, &m, lc)) held = q;
            }
            if (!held) continue;
            j->pass2++;

            if (held->rankTop > 0) {
                // Not counted against g_want: a leaderboard never stops early.
                rankOffer(held, ws, &m, queryScore(held, &m));
                continue;
            }
            EnterCriticalSection(&g_lock);
            if (j->nhits < MAX_HITS) { j->hits[j->nhits].ws = ws; j->hits[j->nhits].m = m; j->hits[j->nhits].q = held; j->nhits++; }
            if (g_hitstream) { printf("HIT %lld %d %d\n", (long long)ws, m.pos[0].x, m.pos[0].z); fflush(stdout); }
            else if (g_stream && g_found < g_want) emitHit(held, ws, &m);
            InterlockedIncrement64(&g_found);
            LeaveCriticalSection(&g_lock);
        }
        lootCacheFree(lc);
        return 0;
    }

    for (uint64_t idx = j->lo; idx < j->hi; idx += j->stride) {
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

        Match pm[MAX_ALT];
        int live[MAX_ALT], nlive = 0;
        for (int p = 0; p < j->nplans; p++)
            if (queryStage1(&plans[p], s48, &pm[p])) live[nlive++] = p;   // cheap reject
        if (!nlive) continue;
        j->pass1++;

        // Scatter the upper 16 bits. Trying up=0 first made the reported seed
        // literally equal the structure seed -- so a scan from 0 returned
        // "128", "146", ... clustered at thread starts. Any upper value is
        // equally valid, so visit them in a pseudo-random order per seed.
        //
        // The order is a PERMUTATION of the 65536 sisters: start + t*step with
        // an odd step is a bijection mod 2^16, so the first N tried are N
        // different worlds. Drawing each one independently, as this used to,
        // repeated ~3% of the default 64 and left over a third of the sisters
        // unvisited even at FIND_UPPERS=65536.
        uint64_t h = s48 * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
        h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 29;
        uint64_t ustart = h & 0xFFFF, ustep = ((h >> 16) & 0xFFFF) | 1;

        // Ranked search scores EVERY sister tried, then offers only the best of
        // them. Stopping at the first sister that passed made the board "the
        // first viable world per structure seed", which for ore, height, loot or
        // biome area -- anything the upper 16 bits change -- is not the best of
        // anything. One entry per structure seed still keeps a single layout
        // from flooding the table with near-identical variants.
        int ranked = q0->rankTop > 0;
        Match m, best;
        int bestScore = INT_MIN; uint64_t bestWs = 0; int haveBest = 0;
        for (int t = 0; t < g_uppers; t++) {
            uint64_t up = (ustart + (uint64_t)t * ustep) & 0xFFFF;
            uint64_t ws = (up << 48) | s48;
            const Query *held = NULL;
            j->applies++;
            // queryStage2 applies the seed itself, once per dimension the query
            // touches -- the caller must not applySeed here. It overwrites the
            // match on success, so each sister starts from pass 1's.
            for (int l = 0; l < nlive && !held; l++) {
                m = pm[live[l]];
                if (queryStage2(&plans[live[l]], &g, ws, &m, lc)) held = &plans[live[l]];
            }
            if (!held) continue;
            j->pass2++;

            if (ranked) {
                int sc = queryScore(held, &m);
                if (!haveBest || sc > bestScore) {
                    bestScore = sc; bestWs = ws; best = m; haveBest = 1;
                }
                if (outOfTime(j->scanned)) break;
                continue;
            }

            EnterCriticalSection(&g_lock);
            if (j->nhits < MAX_HITS) { j->hits[j->nhits].ws = ws; j->hits[j->nhits].m = m; j->hits[j->nhits].q = held; j->nhits++; }
            // Stream mode (FIND_HITSTREAM): emit EVERY hit as "HIT <seed> <x> <z>"
            // (condition 0's matched position), uncapped -- for tier-2 pipelines
            // that need thousands of candidates, not the 12 the array holds.
            if (g_hitstream) { printf("HIT %lld %d %d\n", (long long)ws, m.pos[0].x, m.pos[0].z); fflush(stdout); }
            else if (g_stream && g_found < g_want) emitHit(held, ws, &m);
            InterlockedIncrement64(&g_found);
            LeaveCriticalSection(&g_lock);
            break;
        }
        if (haveBest) rankOffer(q0, bestWs, &best, bestScore);
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
        if (cd->negate) {
            // An absence has no coordinates; say what was checked, from where.
            char ref[48];
            if (cd->parent == PARENT_SPAWN)      snprintf(ref, sizeof ref, "spawn");
            else if (cd->parent == PARENT_AT)    snprintf(ref, sizeof ref, "%d,%d", cd->atX, cd->atZ);
            else if (cd->parent >= 0)            snprintf(ref, sizeof ref, "%s", q->cond[cd->parent].id);
            else                                 snprintf(ref, sizeof ref, "origin");
            printf("   %-14s none within %d of %s\n", cd->id, cd->within, ref);
            continue;
        }
        if (cd->type == CT_LOOT) {
            const char *it = cd->lootItem;
            if (!strncmp(it, "minecraft:", 10)) it += 10;
            printf("   %-14s x=%6d z=%6d   %d %s\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->lootCount[c], it);
            continue;
        }
        if (cd->type == CT_STRONGHOLD) {
            // The roof AND the depth. The roof alone is not the find -- a roof
            // at y=60 is a metre down on a plain and buried inside a mountain --
            // so the number that decides whether it is worth flying to is how
            // far under the surface it sits.
            int d = mm->shDepth[c];
            printf("   %-14s x=%6d z=%6d   stronghold, roof y=%d, %s\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, mm->shRoof[c],
                   d <= 0 ? "BREAKS THE SURFACE"
                          : (d <= 4 ? "just under the surface" : "underground"));
            // WHERE it is exposed to. A roof open to water is still "exposed",
            // and flying to one and finding ocean is exactly how this project's
            // ruined portals earned their reputation. Measured on a hit reported
            // EXPOSED at roof y=51: water sat directly on 466 of its 546 roof
            // columns -- genuinely open, genuinely underwater.
            int surf = mm->shRoof[c] + d;
            // Above y=52 the roof is the structure's own height: vanilla pins a
            // tall stronghold at span+1 instead of rolling a height for it. So
            // any surplus piece the generator invents raises the roof one for
            // one, and this is the band where we have seen it happen.
            //
            // Measured against end portal frames on a real 1.16.5 server:
            // 15 of 15 exact below y=52, 2 of 3 above it. The miss was a
            // stronghold whose real vertical span was 59 where the model built
            // 67 -- same bottom, eight blocks of extra corridor on top.
            //
            // An exposed stronghold on land needs a roof near y=60, so this
            // search lives in that band by construction. The coordinate is
            // worth flying to; it is not yet worth trusting.
            if (mm->shRoof[c] > 52)
                printf("   %-14s NOTE: tall stronghold (roof above y=52). Heights "
                       "here come out right about 2 times in 3 -- verify before "
                       "believing it.\n", "");
            // The teleport goes to the SHALLOWEST PIECE, not the stronghold's
            // start chunk. Those are different places -- up to 112 blocks apart
            // -- and the start is often deep while the exposed part is not, so a
            // coordinate taken from the start lands you on ordinary ground.
            printf("   %-14s /tp %d %d %d -- the shallowest part, %d block(s) of "
                   "ground over it, %s (surface height is an estimate "
                   "before 1.18)\n", "",
                   mm->partner[c].x, mm->shRoof[c] + 1, mm->partner[c].z,
                   d > 0 ? d : 0,
                   surf < 62 ? "ON THE SEA FLOOR, water above it" : "on dry land");
            continue;
        }
        if (cd->type == CT_OVERLAP) {
            // Both halves, because the whole point is the pair -- and the
            // separation, which is what you check first in game.
            int dx = mm->partner[c].x - mm->pos[c].x;
            int dz = mm->partner[c].z - mm->pos[c].z;
            printf("   %-14s x=%6d z=%6d   %s\n", cd->id,
                   mm->pos[c].x, mm->pos[c].z, struct2str(cd->structType));
            // Say WHICH distance this is. It is origin-to-origin, and a mansion
            // sprawls far past its origin marker -- so a pair reported "57
            // blocks away" had buildings about 15-20 apart, and the number read
            // as evidence against the overlap on the very same line.
            printf("   %-14s x=%6d z=%6d   %s, %d blocks origin-to-origin "
                   "(buildings sit closer), %d blocks of footprint overlap\n", "",
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
        int atRel    = (cd->parent == PARENT_AT);
        int condRel  = (cd->parent >= 0);
        int64_t refx = spawnRel ? mm->spawn.x : atRel ? cd->atX
                     : condRel ? mm->pos[cd->parent].x : 0;
        int64_t refz = spawnRel ? mm->spawn.z : atRel ? cd->atZ
                     : condRel ? mm->pos[cd->parent].z : 0;
        int64_t dx = p.x - refx, dz = p.z - refz;
        // The label has to name the reference actually used. A search centred a
        // million blocks out reported "1441772 from origin" -- true, and useless:
        // it reads as a miss when the structure is 400 blocks from where it was
        // asked for. The same went for "village within 300 of the mansion",
        // which printed the village's distance from origin under that label.
        char refname[48];
        if (spawnRel)     snprintf(refname, sizeof refname, "from spawn");
        else if (atRel)   snprintf(refname, sizeof refname, "from %d,%d", cd->atX, cd->atZ);
        else if (condRel) snprintf(refname, sizeof refname, "from %s", q->cond[cd->parent].id);
        else              snprintf(refname, sizeof refname, "from origin");
        printf("   %-14s x=%6d z=%6d   %d %s", cd->id, p.x, p.z,
               (int)sqrt((double)(dx*dx + dz*dz)), refname);
        if (cd->structType == Geode && mm->geodeSize[c] > 0)
            printf(", size %d", mm->geodeSize[c]);
        if (mm->caveHeight[c] > 0) {
            // A floating island and a cave roof both fill caveHeight, and they
            // are very different finds -- so say which one this is rather than
            // letting "cave below" stand for both.
            if (cd->floatVoid > 0)
                printf(", FLOATING island at (%d,%d): %d-block gap, underside y=%d",
                       mm->floatAt[c].x, mm->floatAt[c].z,
                       mm->caveHeight[c], mm->floatCap[c]);
            else
                printf(", %d-block cave below", mm->caveHeight[c]);
        }
        printf("\n");
    }
    writeTsv(q, ws, mm, tsv);
    printf("\n");
}

// ---- FIND_JSON: one machine-readable line per hit ------------------------
//
// The web server used to recover results by regex-matching the text above, one
// pattern per line shape -- and every time a line's wording improved, a result
// type silently vanished from the UI (stronghold rows were never parsed at all;
// the overlap partner stopped parsing when its wording changed). With FIND_JSON
// set, each hit is ALSO printed as "@JSON {...}" in the shape the server hands
// the browser, built from the Match itself rather than from prose. Seeds are
// strings: a 64-bit seed does not survive a JavaScript number.
static void jstr(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch < 0x20) printf("\\u%04x", ch);
        else putchar(ch);
    }
    putchar('"');
}

static const char *refName(const Query *q, const Cond *cd, char *buf, size_t n)
{
    if (cd->parent == PARENT_SPAWN)   snprintf(buf, n, "spawn");
    else if (cd->parent == PARENT_AT) snprintf(buf, n, "%d,%d", cd->atX, cd->atZ);
    else if (cd->parent >= 0)         snprintf(buf, n, "%s", q->cond[cd->parent].id);
    else                              snprintf(buf, n, "origin");
    return buf;
}

static Pos refPos(const Query *q, const Cond *cd, const Match *mm)
{
    if (cd->parent == PARENT_SPAWN) return mm->haveSpawn ? mm->spawn : (Pos){0, 0};
    if (cd->parent == PARENT_AT)    return (Pos){cd->atX, cd->atZ};
    if (cd->parent >= 0)            return mm->pos[cd->parent];
    return (Pos){0, 0};
}

static int dist2i(Pos a, Pos b)
{
    int64_t dx = (int64_t)a.x - b.x, dz = (int64_t)a.z - b.z;
    return (int)sqrt((double)(dx * dx + dz * dz));
}

// Result kinds, one JSON array each, emitted kind by kind so arrays never
// interleave. The keys are the ones the server's old regex parser produced.
enum { K_PLACES, K_LOOT, K_SH, K_OVL, K_CAC, K_ORE, K_SLIME, K_HGT, K_AREA,
       K_ISL, K_BIO, K_CLU, K_ABS, K_N, K_NONE = -1 };
static const char *const KIND_KEY[K_N] = {
    "places", "loot", "stronghold", "overlap", "cactus", "ore", "slime",
    "height", "area", "island", "biome", "cluster", "absent" };

static int kindOf(const Cond *cd)
{
    if (cd->negate) return K_ABS;
    switch (cd->type) {
    case CT_LOOT:        return K_LOOT;
    case CT_STRONGHOLD:  return K_SH;
    case CT_OVERLAP:     return K_OVL;
    case CT_CACTUS:      return K_CAC;
    case CT_ORE:         return K_ORE;
    case CT_SLIME:       return K_SLIME;
    case CT_HEIGHT:      return K_HGT;
    case CT_BIOME_AREA:  return K_AREA;
    case CT_ISLAND:      return K_ISL;
    case CT_BIOME:       return K_BIO;
    case CT_STRUCTURE:   return cd->structMin > 1 ? K_CLU : K_PLACES;
    default:             return K_NONE;    // eyes: reported as "portal"
    }
}

// One condition's entry, as a JSON object.
static void jsonEntry(const Query *q, const Match *mm, int c)
{
    const Cond *cd = &q->cond[c];
    Pos p = mm->pos[c];
    char rb[48];
    printf("{\"id\":"); jstr(cd->id);
    switch (kindOf(cd)) {
    case K_ABS:
        printf(",\"within\":%d,\"of\":", cd->within); jstr(refName(q, cd, rb, sizeof rb));
        break;
    case K_LOOT: {
        const char *it = cd->lootItem;
        if (!strncmp(it, "minecraft:", 10)) it += 10;
        printf(",\"x\":%d,\"z\":%d,\"count\":%d,\"item\":", p.x, p.z, mm->lootCount[c]);
        jstr(it);
        printf(",\"structure\":"); jstr(struct2str(cd->structType));
        printf(",\"dim\":"); jstr(dimName(cd->dim));
        break;
    }
    case K_SH: {
        int d = mm->shDepth[c], surf = mm->shRoof[c] + d;
        printf(",\"x\":%d,\"z\":%d,\"roof\":%d,\"state\":", p.x, p.z, mm->shRoof[c]);
        jstr(d <= 0 ? "BREAKS THE SURFACE" : (d <= 4 ? "just under the surface" : "underground"));
        printf(",\"tp\":{\"x\":%d,\"y\":%d,\"z\":%d},\"depth\":%d,\"where\":",
               mm->partner[c].x, mm->shRoof[c] + 1, mm->partner[c].z, d > 0 ? d : 0);
        jstr(surf < 62 ? "on the sea floor, water above it" : "on dry land");
        printf(",\"tall\":%s", mm->shRoof[c] > 52 ? "true" : "false");
        break;
    }
    case K_OVL:
        printf(",\"x\":%d,\"z\":%d,\"structure\":", p.x, p.z);
        jstr(struct2str(cd->structType));
        printf(",\"x2\":%d,\"z2\":%d,\"structure2\":", mm->partner[c].x, mm->partner[c].z);
        jstr(struct2str(cd->structType2));
        printf(",\"gap\":%d,\"area\":%d,\"dim\":", dist2i(p, mm->partner[c]), mm->overlapArea[c]);
        jstr(dimName(cd->dim));
        break;
    case K_CAC:
        printf(",\"x\":%d,\"z\":%d,\"height\":%d,\"base_y\":%d",
               p.x, p.z, mm->cactusTall[c], mm->cactusBase[c]);
        break;
    case K_ORE: {
        int nm; const OreMaterial *tab = oreMaterials(&nm);
        printf(",\"x\":%d,\"z\":%d,\"count\":%d,\"material\":", p.x, p.z, mm->oreCount[c]);
        jstr(tab[cd->oreMat].name);
        printf(",\"within\":%d", cd->within);
        if (cd->oreExposed) printf(",\"exposed\":true");
        if (mm->veinMax[c] > 0) printf(",\"vein\":%d", mm->veinMax[c]);
        break;
    }
    case K_SLIME:
        printf(",\"x\":%d,\"z\":%d,\"count\":%d,\"within\":%d",
               p.x, p.z, mm->slimeCount[c], cd->within);
        break;
    case K_HGT:
        printf(",\"x\":%d,\"z\":%d,\"peak\":%d,\"within\":%d,\"exact\":%s",
               p.x, p.z, mm->peakHeight[c], cd->within, cd->exactTerrain ? "true" : "false");
        if (cd->reliefMin > 0) printf(",\"relief\":%d", mm->peakDrop[c]);
        break;
    case K_AREA: case K_ISL: {
        int tot = mm->areaTotal[c], hit = mm->areaCells[c];
        int pct = tot > 0 ? (int)((int64_t)hit * 100 / tot) : 0;
        printf(",\"x\":%d,\"z\":%d,\"pct\":%d,\"within\":%d", p.x, p.z, pct, cd->within);
        if (cd->type == CT_BIOME_AREA) {
            printf(",\"biome\":");
            jstr(cd->biomeIsSet ? cd->biomeLabel : biome2str(q->mc, cd->biomeId));
        }
        break;
    }
    case K_BIO:
        printf(",\"x\":%d,\"z\":%d,\"biome\":", p.x, p.z);
        jstr(cd->biomeIsSet ? cd->biomeLabel : biome2str(q->mc, cd->biomeId));
        printf(",\"dist\":%d,\"ref\":", dist2i(p, refPos(q, cd, mm)));
        jstr(refName(q, cd, rb, sizeof rb));
        printf(",\"dim\":"); jstr(dimName(cd->dim));
        break;
    case K_CLU:
        printf(",\"x\":%d,\"z\":%d,\"count\":%d,\"structure\":", p.x, p.z, mm->structCount[c]);
        jstr(struct2str(cd->structType));
        if (cd->spread > 0) printf(",\"spread\":%d,\"tight\":true", cd->spread);
        else printf(",\"within\":%d", cd->within);
        printf(",\"dim\":"); jstr(dimName(cd->dim));
        break;
    case K_PLACES:
        printf(",\"x\":%d,\"z\":%d,\"structure\":", p.x, p.z);
        jstr(struct2str(cd->structType));
        printf(",\"dim\":"); jstr(dimName(cd->dim));
        printf(",\"dist\":%d,\"ref\":", dist2i(p, refPos(q, cd, mm)));
        jstr(refName(q, cd, rb, sizeof rb));
        if (cd->structType == Geode && mm->geodeSize[c] > 0) printf(",\"size\":%d", mm->geodeSize[c]);
        if (mm->caveHeight[c] > 0) {
            if (cd->floatVoid > 0)
                printf(",\"floating\":{\"x\":%d,\"z\":%d,\"gap\":%d,\"underside\":%d}",
                       mm->floatAt[c].x, mm->floatAt[c].z, mm->caveHeight[c], mm->floatCap[c]);
            else
                printf(",\"cave\":%d", mm->caveHeight[c]);
        }
        break;
    default:
        break;
    }
    putchar('}');
}

static void jsonHit(const Query *q, uint64_t ws, const Match *mm,
                    int haveScore, int score)
{
    printf("@JSON {\"seed\":\"%" PRId64 "\"", (int64_t)ws);
    if (haveScore) {
        printf(",\"score\":%d,\"metric\":", score);
        jstr(queryScoreLabel(q));
    }
    if (mm->haveSpawn) printf(",\"spawn\":{\"x\":%d,\"z\":%d}", mm->spawn.x, mm->spawn.z);
    if (mm->haveEyes)
        printf(",\"portal\":{\"x\":%d,\"z\":%d,\"eyes\":%d}",
               mm->stronghold.x, mm->stronghold.z, mm->eyes);
    for (int k = 0; k < K_N; k++) {
        int any = 0;
        for (int c = 0; c < q->n; c++) {
            if (kindOf(&q->cond[c]) != k) continue;
            printf(any ? "," : ",\"%s\":[", KIND_KEY[k]);
            jsonEntry(q, mm, c);
            any = 1;
        }
        if (any) putchar(']');
        else if (k == K_PLACES) printf(",\"places\":[]");   // always present
    }
    printf("}\n");
}

// ---- --seeds FILE: check a list of seeds ---------------------------------
//
// "Which of MY seeds have this?" -- a list from a friend, a server, another
// finder's export. One process, all threads, instead of one process per seed.
// Per seed: "@CHECK {...}" with the verdict, then (for a yes) the hit exactly
// as a search prints it. Results come in completion order; "index" is the
// seed's position in the list (blank lines and #comments are skipped), so a
// reader can restore the input order.
typedef struct {
    const Query *plans;
    int nplans;
    char **lines;
    int n;
    volatile LONG next;
    volatile LONG yes;
} SeedList;

// Does this one seed hold, and under which alternative? *placed says whether any
// alternative got past the structure pass, so a no can say which kind of no.
static const Query *matchSeed(const Query *plans, int nplans, Generator *g, uint64_t ws,
                              Match *m, LootCache *lc, int *placed)
{
    *placed = 0;
    for (int p = 0; p < nplans; p++) {
        if (!queryStage1(&plans[p], ws & ((1ULL << 48) - 1), m)) continue;
        *placed = 1;
        if (queryStage2(&plans[p], g, ws, m, lc)) return &plans[p];
    }
    return NULL;
}

static DWORD WINAPI seedListWorker(LPVOID arg)
{
    SeedList *sl = (SeedList *)arg;
    Generator g;
    setupGenerator(&g, sl->plans->mc, queryGenFlags(sl->plans));
    LootCache *lc = lootCacheNew();
    for (;;) {
        LONG i = InterlockedIncrement(&sl->next) - 1;
        if (i >= sl->n) break;
        uint64_t ws;
        if (!parseWorldSeed(sl->lines[i], &ws)) continue;
        Match m;
        int s1;
        const Query *q = matchSeed(sl->plans, sl->nplans, &g, ws, &m, lc, &s1);
        int s2 = q != NULL;
        if (s2) InterlockedIncrement(&sl->yes);
        EnterCriticalSection(&g_lock);
        printf("@CHECK {\"index\":%ld,\"input\":", (long)i);
        jstr(sl->lines[i]);
        printf(",\"seed\":\"%" PRId64 "\",\"verdict\":\"%s\",\"why\":", (int64_t)ws,
               s2 ? "yes" : "no");
        jstr(s2 ? "" : !s1 ? "the structures this query needs are not placed in this seed"
                           : "placed, but a biome, viability, terrain or loot condition fails");
        printf("}\n");
        if (s2) {
            printf("SEED %" PRId64 "\n", (int64_t)ws);
            printHit(q, ws, &m, NULL);
            if (g_json) jsonHit(q, ws, &m, 0, 0);
        }
        fflush(stdout);
        LeaveCriticalSection(&g_lock);
    }
    if (lc) lootCacheFree(lc);
    return 0;
}

static int checkSeedList(const Query *plans, int nplans, const char *path, int nthreads)
{
    char *text = slurp(path);
    if (!text) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    int cap = 1024, n = 0;
    char **lines = malloc(sizeof(char *) * cap);
    for (char *ln = strtok(text, "\r\n"); ln; ln = strtok(NULL, "\r\n")) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (!*ln || *ln == '#') continue;              // blank lines and comments
        if (n == cap) { cap *= 2; lines = realloc(lines, sizeof(char *) * cap); }
        lines[n++] = ln;
        if (n >= 1000000) break;
    }
    SeedList sl = { plans, nplans, lines, n, 0, 0 };
    InitializeCriticalSection(&g_lock);
    HANDLE th[64];
    for (int i = 0; i < nthreads; i++) th[i] = CreateThread(NULL, 0, seedListWorker, &sl, 0, NULL);
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    for (int i = 0; i < nthreads; i++) CloseHandle(th[i]);
    printf("--- checked %d seeds: %ld satisfy the query ---\n", n, (long)sl.yes);
    free(lines); free(text);
    return sl.yes ? 0 : 3;
}

// The machine-readable twin of a hit, for piping into verifiers.
static void writeTsv(const Query *q, uint64_t ws, const Match *mm, FILE *tsv)
{
    if (!tsv) return;
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
    // --seed: does THIS world seed satisfy the query?
    //
    // FIND_OFFSET cannot answer that. It is an index that goes through mix64
    // before becoming a seed, so there is no offset you can pass to reach a
    // particular seed. Checking a seed someone hands you -- which is most of
    // what verifying a known find consists of -- had no path through this tool
    // at all, and the alternative was to search for it, which is absurd.
    int seedmode = (argc > 3) && !strcmp(argv[2], "--seed");
    // --seeds FILE [threads]: check every seed in a list (checkSeedList).
    int listmode = (argc > 3) && !strcmp(argv[2], "--seeds");
    uint64_t oneseed = 0;
    // The seed exactly as Minecraft reads it: a number if it is a valid long,
    // otherwise the hash of the text (src/seed.h).
    if (seedmode && !parseWorldSeed(argv[3], &oneseed)) {
        fprintf(stderr, "--seed: empty seed\n");
        return 2;
    }
    uint64_t range   = (argc > 2 && !explain && !bias && !seedmode) ? strtoull(argv[2], NULL, 10) : 5000000;
    uint64_t samples = ((explain || bias) && argc > 3) ? strtoull(argv[3], NULL, 10) : 200000;
    int nthreads = (explain || bias || listmode) ? ((argc > 4) ? atoi(argv[4]) : 16)
                                     : ((argc > 3) ? atoi(argv[3]) : 16);
    // 0 divided the range by zero; more than 64 (MAXIMUM_WAIT_OBJECTS) made the
    // wait return at once and the results were read while workers still ran.
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    char *json = slurp(argv[1]);
    if (!json) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    Query *plans; char err[1024];
    int nplans = queryCompile(json, &plans, err, sizeof err);
    if (nplans <= 0) {
        fprintf(stderr, "%s error: %s\n", nplans < 0 ? "plan" : "query", err);
        return 1;
    }
    // Everything that belongs to the whole query reads the first alternative:
    // the version, the world type and a leaderboard (which has only one).
    const Query *q = plans;

    char buf[96];
    if (nplans > 1)
        printf("search  : any of %d alternatives, MC %s\n\n", nplans, mc2str(q->mc));
    for (int p = 0; p < nplans; p++) {
        const Query *a = &plans[p];
        if (nplans > 1) printf("alternative %d of %d\n", p + 1, nplans);
        if (a->warn[0] && (!p || strcmp(a->warn, plans[p-1].warn)))
            printf("NOTE: %s\n\n", a->warn);
        printf("query   : %d conditions, MC %s\n", a->n, mc2str(a->mc));
        for (int i = 0; i < a->n; i++)
            printf("   [%s] %s\n", a->cond[i].id, condDesc(a, i, buf, sizeof buf));
        printf("\n");
        queryPrintPlan(a, stdout);

        // Warn when a condition uses a structure the engine cannot verify. Its
        // position is exact, but whether the game actually places anything there
        // is unmodelled -- which looks exactly like a wrong answer in game.
        for (int i = 0; i < a->n; i++) {
            if (a->cond[i].type != CT_STRUCTURE) continue;
            for (int k = 0; k < queryStructureCount(); k++) {
                if (queryStructureType(k) != a->cond[i].structType) continue;
                if (queryStructureVerified(k)) continue;
                printf("NOTE: \"%s\" is a %s. The engine reports EVERY generation attempt\n"
                       "      for these without checking whether the game actually places\n"
                       "      one, so a reported hit may not exist in your world.\n"
                       "      Measured: 100%% of attempts pass, vs 0.9-31.7%% for\n"
                       "      biome-checked structures like village/mansion/monument.\n\n",
                       a->cond[i].id, queryStructureName(k));
            }
        }
    }

    if (bias) {
        for (int p = 0; p < nplans; p++) explainCompare(&plans[p], samples, nthreads, stdout);
        free(json); return 0;
    }
    if (explain) {
        if (nplans > 1) explainAny(plans, nplans, samples, nthreads, stdout);
        else            explainQuery(q, samples, nthreads, stdout, NULL);
        free(json); return 0;
    }

    g_json = getenv("FIND_JSON") != NULL;
    if (listmode) {
        int rc = checkSeedList(plans, nplans, argv[3], nthreads);
        free(json);
        return rc;
    }
    if (seedmode) {
        Generator g;
        setupGenerator(&g, q->mc, queryGenFlags(q));
        Match m;
        // Stage 1 works on the lower 48 bits, stage 2 on the whole seed. Both
        // must be reported separately: "the structures are placed but a biome
        // rules it out" and "the placement never happened" are different
        // answers, and collapsing them into one no would hide which.
        LootCache *lc = lootCacheNew();
        int s1;
        const Query *held = matchSeed(plans, nplans, &g, oneseed, &m, lc, &s1);
        printf("--- seed %" PRId64 " ---\n", (int64_t)oneseed);
        if (!s1) {
            printf("NO -- fails the 48-bit structure pass: the placements this query\n"
                   "      needs are not in this seed at all.\n");
        } else if (!held) {
            printf("NO -- placements exist, but the full-seed pass rejects it\n"
                   "      (biome, viability, terrain or a Y/loot condition).\n");
        } else {
            printf("YES -- this seed satisfies the query.\n\n");
            printHit(held, oneseed, &m, NULL);
            if (g_json) jsonHit(held, oneseed, &m, 0, 0);
        }
        if (lc) lootCacheFree(lc);
        free(json);
        return held ? 0 : 3;
    }

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
    const char *up = getenv("FIND_UPPERS");
    if (up) {
        long v = strtol(up, NULL, 10);
        // Capped at 65536 because that is all the sisters there are; below 1
        // would silently disable pass 2 and pass every structure seed.
        if (v < 1) v = 1;
        if (v > 65536) v = 65536;
        g_uppers = (int)v;
        printf("uppers     : trying %d of 65536 sister seeds per structure seed "
               "(default %d)\n", g_uppers, UPPER_SAMPLES_DEFAULT);
    }
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
    if (q->rankTop > 0) {
        g_want = 0x7fffffff;
        printf("ranking the top %d by %s over %" PRIu64 " %s (%s)\n\n",
               q->rankTop, queryScoreLabel(q), range,
               q->ngeom == 0 ? "world seeds" : "structure seeds",
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
        // A candidate list is specific to one cluster, so alternatives always
        // scan: a seed that only the other alternative would match is not on it.
        why[0] = 0;
        int sk = (nplans == 1 && q->ngeom > 0 && !getenv("FIND_NOSOLVE"))
               ? solvePick(q, why, sizeof why) : -1;
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
                ncand = solveFill(q, sk, capN, cand, nthreads, budget, &timedOut);
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
                } else if (ncand < 1000 && timedOut) {
                    // Only a TIMED-OUT solver with a thin yield is a sign that
                    // scanning would do better. A solver that finished has
                    // enumerated every structure seed in 2^48 that places the
                    // cluster, so a short list is the complete answer and
                    // scanning instead would throw it away for a search that
                    // can only find a subset of it.
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
        } else if (why[0] && q->ngeom > 0 && strstr(why, "solving") == NULL && strstr(why, "no tight") == NULL) {
            printf("solver: not used -- %s\n\n", why);
        }
    }

    InitializeCriticalSection(&g_lock);
    Job *jobs = calloc(nthreads, sizeof(Job));
    HANDLE *th = calloc(nthreads, sizeof(HANDLE));

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    for (int i = 0; i < nthreads; i++) {
        jobs[i].plans = plans;
        jobs[i].nplans = nplans;
        jobs[i].cand = cand;
        // INTERLEAVED, not one contiguous block per thread. With blocks, a run
        // stopped by its time budget left an unscanned tail inside every block,
        // so there was no single place to resume from: a hunt that advanced by
        // the whole range silently skipped those tails, and one that advanced
        // by the count scanned re-ran most of the range. Interleaved, everything
        // below base + nthreads * (fewest seeds any thread scanned) is covered,
        // which is a clean resume point (printed as "covered" below).
        jobs[i].lo = (uint64_t)i;
        jobs[i].hi = range;
        jobs[i].stride = (uint64_t)nthreads;
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
    if (q->rankTop > 0) {
        // Leaderboard: the whole range was scanned, so these really are the
        // best seen -- not merely the first that cleared a threshold.
        printf("--- leaderboard: top %d by %s ---\n\n", g_nrank, queryScoreLabel(q));
        for (int i = 0; i < g_nrank; i++, shown++) {
            printf("SEED %" PRId64 "   %s = %d\n", (int64_t)g_rank[i].ws,
                   queryScoreLabel(q), g_rank[i].score);
            printHit(q, g_rank[i].ws, &g_rank[i].m, tsv);
            if (g_json) jsonHit(q, g_rank[i].ws, &g_rank[i].m, 1, g_rank[i].score);
        }
    } else {
        // Stream mode already printed each hit as it was found; printing them
        // again would show every seed twice. The TSV twin is still written.
        for (int i = 0; i < nthreads && shown < MAX_HITS; i++)
            for (int k = 0; k < jobs[i].nhits && shown < MAX_HITS; k++, shown++) {
                Hit *h = &jobs[i].hits[k];
                if (g_stream && !g_hitstream) { writeTsv(h->q, h->ws, &h->m, tsv); continue; }
                printf("SEED %" PRId64 "\n", (int64_t)h->ws);
                printHit(h->q, h->ws, &h->m, tsv);
                if (g_json) jsonHit(h->q, h->ws, &h->m, 0, 0);
            }
    }

    double r1 = 100.0 * p1 / (sc ? sc : 1);
    int wholeSeed = anyWholeSeed(plans, nplans);
    printf("--- funnel ---\n");
    // Without this line a budgeted run is indistinguishable from a tiny range,
    // and "best of N seeds" would quietly mean a different N than requested.
    if (g_expired)
        printf("stopped    : time budget expired before the range was exhausted\n");
    // Where a follow-up run should start so that nothing is skipped. Every
    // index below this was scanned (see the interleaving note above); a few
    // just past it may have been too, which costs a little duplicate work and
    // never a gap. Solver runs index a candidate list, not the seed space, so
    // there is no offset to resume from.
    if (!cand) {
        uint64_t minScan = UINT64_MAX;
        for (int i = 0; i < nthreads; i++)
            if ((uint64_t)i < range && jobs[i].scanned < minScan) minScan = jobs[i].scanned;
        if (minScan == UINT64_MAX) minScan = 0;
        uint64_t covered = minScan * (uint64_t)nthreads;
        if (!g_expired && g_found < g_want) covered = range;   // ran to the end
        if (covered > range) covered = range;
        printf("covered    : indices %" PRIu64 "..%" PRIu64 " -- resume with "
               "FIND_OFFSET=%" PRIu64 "\n", g_offset, g_offset + covered,
               g_offset + covered);
    }
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
