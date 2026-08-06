// mitm -- solve every structure constraint at once, instead of solving one and
// testing the rest.
//
//   mitm <version> <structure> quad <spread> [--nested|--join] [--limit N]
//                                            [--threads T] [--window N]
//   mitm <version> <structure> --verify
//   mitm <version> <structure> --accepts <spread>      (seeds on stdin)
//
// tools/quad.c solves the first structure's two constraints and tests the other
// six: 2^48/range^2 seeds per corner offset, so a complete enumeration costs
// MORE the looser the spread -- ~49 hours for swamp huts at spread 15, which is
// the tightest spread that has any quads at all.
// This sieves the low 24 bits of the seed against ALL eight constraints first --
// a low half that no high half can rescue is discarded having touched no high
// half at all -- and then meets the survivors with the high halves, either by
// sweeping them (obvious) or by looking them up in a bucket index (fast).
//
// Both halves of that are useless if the sieve silently drops seeds: the search
// would report "no quad huts" for configurations that exist and nothing about
// the output would look wrong. So --verify checks completeness the same way
// tools/invert.c does, by brute force, and demands zero missed.
#include "mitm.h"
#include "invert.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>

// ---------------------------------------------------------------------------
// seed sets
// ---------------------------------------------------------------------------

typedef struct { uint64_t *v; size_t n, cap; } Set;

static int setPush(Set *s, uint64_t x)
{
    if (s->n == s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1024;
        uint64_t *p = realloc(s->v, c * sizeof *p);
        if (!p) { fprintf(stderr, "out of memory\n"); exit(3); }
        s->v = p; s->cap = c;
    }
    s->v[s->n++] = x;
    return 0;
}
static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}
static void setSort(Set *s) { qsort(s->v, s->n, sizeof *s->v, cmp64); }
static void setFree(Set *s) { free(s->v); s->v = NULL; s->n = s->cap = 0; }

// Compare two sorted sets. Returns 0 if identical; otherwise prints the first
// few differences in each direction and returns 1.
static int setDiff(const char *na, Set *a, const char *nb, Set *b)
{
    setSort(a); setSort(b);
    size_t i = 0, j = 0, onlyA = 0, onlyB = 0, shown = 0;
    while (i < a->n || j < b->n) {
        if (j >= b->n || (i < a->n && a->v[i] < b->v[j])) {
            if (shown++ < 4) printf("    only in %s: %" PRIu64 "\n", na, a->v[i]);
            onlyA++; i++;
        } else if (i >= a->n || b->v[j] < a->v[i]) {
            if (shown++ < 4) printf("    only in %s: %" PRIu64 "\n", nb, b->v[j]);
            onlyB++; j++;
        } else { i++; j++; }
    }
    if (onlyA || onlyB) {
        printf("    %s has %zu the other lacks, %s has %zu\n", na, onlyA, nb, onlyB);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// independent reference: cubiomes' own placement
// ---------------------------------------------------------------------------

// getStructurePos, not getFeaturePos: it is the engine's top-level entry point,
// so it applies the averaging for large structures, the outpost/bastion
// rejection rolls and end city's origin gap. Comparing against the raw feature
// position would have made every roll the solver honours look like a miss.
static int refMatches(const MitmQuery *q, uint64_t ws)
{
    StructureConfig sc;
    getStructureConfig(q->st, q->mc, &sc);
    for (int j = 0; j < q->n; j++) {
        Pos p;
        if (!getStructurePos(q->st, q->mc, ws, q->regX[j], q->regZ[j], &p)) return 0;
        int ox = (p.x >> 4) - q->regX[j] * sc.regionSize;
        int oz = (p.z >> 4) - q->regZ[j] * sc.regionSize;
        if (ox < 0 || ox >= (int)q->range || oz < 0 || oz >= (int)q->range) return 0;
        if (!((q->okx[j] >> ox) & 1)) return 0;
        if (!((q->okz[j] >> oz) & 1)) return 0;
    }
    return 1;
}

// Does the structure this seed places here actually EXIST? For most types
// placement is existence, but a 1.18+ fortress shares its grid slot with a
// bastion -- same salt, same region, same range -- and only one of them is
// built. getStructurePos(Fortress, ...) returns 1 unconditionally, so a "quad
// fortress" found on placement alone was three bastions and a fortress. That is
// the difference between a result and a disappointment.
//
// The tie-break needs biomes, so it needs a generator; it is only ever reached
// by a candidate that already passed the geometry, which is rare enough that
// applySeed per candidate costs nothing measurable.
static Generator *g_viab = NULL;
static int g_viabDim = 0;
static uint64_t g_viabSeed = ~0ULL;

static int reallyExists(int mc, int st, uint64_t ws, Pos p)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;
    if (!g_viab) {
        g_viab = malloc(sizeof(Generator));
        setupGenerator(g_viab, mc, 0);
        g_viabDim = sc.dim;
        g_viabSeed = ~0ULL;
    }
    if (g_viabSeed != ws) {
        applySeed(g_viab, sc.dim, ws);
        g_viabSeed = ws;
    }
    return isViableStructurePos(st, g_viab, p.x, p.z, 0) != 0;
}

// Placement alone is enough for these; anything else is checked properly.
static int placementIsExistence(int st)
{
    return st != Fortress && st != Bastion;
}

// The real cluster test: every pair inside one despawn sphere. Same rule as
// tools/quad.c, checked against cubiomes' placement rather than ours.
static int quadFits(const MitmQuery *q, uint64_t ws, int spread, Pos out[4])
{
    for (int i = 0; i < 4; i++) {
        Pos p;
        if (!getStructurePos(q->st, q->mc, ws, q->regX[i], q->regZ[i], &p)) return 0;
        out[i].x = p.x >> 4; out[i].z = p.z >> 4;
    }
    for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
            int dx = out[a].x - out[b].x, dz = out[a].z - out[b].z;
            if (dx*dx + dz*dz > spread * spread) return 0;
        }
    return 1;
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

static int collect(uint64_t ws, void *arg) { return setPush((Set*)arg, ws); }

typedef struct {
    const MitmQuery *q;
    int spread;
    uint64_t found, bad;
    int shown;
    Set *bases;
    uint64_t want;          // stop after this many BASES (0 = enumerate all)
} QuadCtx;

static int onQuad(uint64_t ws, void *arg)
{
    QuadCtx *c = arg;
    Pos p[4];
    // The solver's own --limit would count placements-on-allowed-offsets, of
    // which only ~1% are real clusters. "give me 3 quad huts" means 3 quad huts.
    if (!quadFits(c->q, ws, c->spread, p)) return 0;
    if (!placementIsExistence(c->q->st)) {
        for (int i = 0; i < 4; i++) {          // quadFits reports chunks
            Pos b = { p[i].x << 4, p[i].z << 4 };
            if (!reallyExists(c->q->mc, c->q->st, ws, b)) return 0;
        }
    }
    c->found++;
    if (c->bases) setPush(c->bases, ws);
    if (c->shown < 12) {
        printf("QUADBASE %-18" PRIu64 " chunks", ws);
        for (int i = 0; i < 4; i++) printf(" (%d,%d)", p[i].x, p[i].z);
        printf("\n");
        fflush(stdout);
        c->shown++;
    }
    return c->want && c->found >= c->want;
}

// ---------------------------------------------------------------------------
// query builders used by the harness
// ---------------------------------------------------------------------------

static uint64_t bandMask(int lo, int hi, uint32_t r)   // [lo,hi] clamped, as bits
{
    uint64_t m = 0;
    for (int o = lo; o <= hi; o++) if (o >= 0 && o < (int)r) m |= 1ULL << o;
    return m;
}

static int pass = 0, failed = 0, skipped = 0;
static int check(const char *name, int bad, const char *detail)
{
    if (bad) { failed++; printf("  \033[31mFAIL\033[0m %-34s %s\n", name, detail ? detail : ""); }
    else     { pass++;   printf("  ok   %-34s %s\n", name, detail ? detail : ""); }
    return bad;
}
// A sample that was always going to be empty proves nothing either way, so it
// is neither a pass nor a failure. Saying so beats a green tick on a vacuous
// comparison -- and beats a red one, which is what an empty slab used to give.
static int skip(const char *name, const char *detail)
{
    skipped++;
    printf("  --   %-34s %s\n", name, detail ? detail : "");
    return 0;
}

// ---------------------------------------------------------------------------
// verification
// ---------------------------------------------------------------------------

// TEST A. Does the enumeration's acceptance test agree with cubiomes on every
// seed in a slab, both ways? A seed cubiomes says matches but the sieve rejects
// is a seed the search would never visit -- the silent failure this file exists
// to rule out.
static int testPredicate(const MitmQuery *q, const char *name,
                         uint64_t lo, uint64_t count)
{
    uint64_t hits = 0, missed = 0, unsound = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t ws = (lo + i) & ((1ULL << 48) - 1);
        int want = refMatches(q, ws);
        int got  = mitmWouldReach(q, ws);
        if (want) hits++;
        if (want && !got) { if (missed < 3) printf("       MISSED %" PRIu64 "\n", ws); missed++; }
        if (!want && got) { if (unsound < 3) printf("       FALSE  %" PRIu64 "\n", ws); unsound++; }
    }
    char d[200];
    // An empty sample is not a failed test, it is an absent one -- and deciding
    // which by MODELLING the density was wrong three times running (the
    // rejection rolls, then the triangular distribution of an averaged offset,
    // then whatever correlates the rolls of neighbouring outposts). The sample
    // says all that can honestly be said: the independent reference found
    // nothing here either, so there was nothing for the sieve to miss.
    if (!hits && !unsound) {
        snprintf(d, sizeof d, "reference finds none in %" PRIu64 " seeds either, "
                 "so there is nothing here to miss", count);
        return skip(name, d);
    }
    snprintf(d, sizeof d, "%" PRIu64 " hits in %" PRIu64 " seeds, %" PRIu64
             " missed, %" PRIu64 " false", hits, count, missed, unsound);
    return check(name, missed || unsound || hits == 0, d);
}

// TEST B. Exhaustive over a window: brute force the whole window, run the
// solver restricted to the same window, demand the two sets are equal. Not
// "similar counts" -- the same seeds.
static int testWindow(const MitmQuery *q, const char *name,
                      uint64_t lo, uint64_t count, int join)
{
    Set brute = {0}, solved = {0};
    for (uint64_t i = 0; i < count; i++) {
        uint64_t ws = (lo + i) & ((1ULL << 48) - 1);
        if (refMatches(q, ws)) setPush(&brute, ws);
    }
    MitmRun run = {0};
    run.join = join; run.window = 1; run.wsLo = lo; run.wsHi = lo + count;
    mitmRun(q, &run, collect, &solved);

    int bad = setDiff("brute", &brute, join ? "join" : "nested", &solved);
    char d[200];
    int r;
    if (!bad && brute.n == 0) {
        snprintf(d, sizeof d, "brute force finds none in this window of %" PRIu64
                 ", and the solver returned none either", count);
        r = skip(name, d);
    } else {
        snprintf(d, sizeof d, "%zu seeds in window, %s", brute.n,
                 bad ? "SETS DIFFER" : "identical sets");
        r = check(name, bad || brute.n == 0, d);
    }
    setFree(&brute); setFree(&solved);
    return r;
}

// TEST C. Attack the sieve where it is dangerous: on the low halves it THREW
// AWAY. Take rejected ones and try all 2^24 high halves against each. The sieve
// only ever rejects when some constraint's residue class is empty for every
// possible high half, so a single survivor here would be a dropped seed.
// One rejected low half against a slice of the high halves. Threaded because
// this is the most expensive check in the file: 2^24 reference placements per
// sample, and for a mansion each of those is four LCG draws per structure. Run
// serially it took 10-20 minutes PER CALL at range 60, which is why mansion and
// end city went unverified rather than unverifiable.
typedef struct {
    const MitmQuery *q;
    uint32_t yL, lo, hi;
    uint64_t escaped;
    uint64_t first;          // an escaping seed, for the failure message
} RejJob;

static DWORD WINAPI rejWorker(LPVOID arg)
{
    RejJob *j = arg;
    for (uint32_t yH = j->lo; yH < j->hi; yH++) {
        uint64_t ws = mitmSeedOf(j->q, yH, j->yL);
        if (refMatches(j->q, ws)) {
            if (!j->escaped) j->first = ws;
            j->escaped++;
        }
    }
    return 0;
}

static uint64_t sweepAllHighHalves(const MitmQuery *q, uint32_t yL, uint64_t *first)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    int nt = (int)si.dwNumberOfProcessors;
    if (nt < 1) nt = 1; if (nt > 64) nt = 64;

    RejJob job[64];
    HANDLE th[64];
    for (int i = 0; i < nt; i++) {
        job[i].q = q; job[i].yL = yL; job[i].escaped = 0; job[i].first = 0;
        job[i].lo = (uint32_t)((uint64_t)(1u << 24) * i / nt);
        job[i].hi = (uint32_t)((uint64_t)(1u << 24) * (i + 1) / nt);
        th[i] = CreateThread(NULL, 0, rejWorker, &job[i], 0, NULL);
    }
    WaitForMultipleObjects(nt, th, TRUE, INFINITE);
    uint64_t esc = 0;
    for (int i = 0; i < nt; i++) {
        CloseHandle(th[i]);
        if (job[i].escaped && first && !*first) *first = job[i].first;
        esc += job[i].escaped;
    }
    return esc;
}

static int testRejections(const MitmQuery *q, const char *name, int samples)
{
    uint64_t swept = 0, escaped = 0;
    int taken = 0;
    // Odd multiplier, so this walks all 2^24 low halves in a scattered order and
    // terminates. It has to terminate on its own: when gcd(128, range) is 1 the
    // sieve rejects NOTHING -- every low half is live for some high half -- and
    // "keep drawing until you find a rejected one" would spin forever.
    for (uint32_t i = 0; i < (1u << 24) && taken < samples; i++) {
        uint32_t yL = (uint32_t)(i * 2654435761u) & 0xFFFFFF;
        if (mitmSieve(q, yL)) continue;
        taken++;
        uint64_t first = 0;
        uint64_t esc = sweepAllHighHalves(q, yL, &first);
        if (esc && escaped < 3)
            printf("       rejected low half %u admits seed %" PRIu64 "\n", yL, first);
        escaped += esc;
        swept += 1u << 24;
    }
    char d[200];
    if (!taken)
        snprintf(d, sizeof d, "sieve rejects nothing here (gcd(128,%u)=%u, so every "
                 "low half is live) -- nothing to falsify", q->range, q->g);
    else
        snprintf(d, sizeof d, "%d rejected low halves x 2^24 high halves = %" PRIu64
                 " seeds, %" PRIu64 " should have been kept", taken, swept, escaped);
    return check(name, escaped != 0, d);
}

// How many low halves survive? Cheap, and it decides how much of the space a
// comparison can afford to cover. The sieve's strength is driven by
// gcd(128, range): swamp huts (range 24, gcd 8) keep one low half in 2^24 with
// the offsets pinned, villages (range 26, gcd 2) keep 65536 of them, so "run it
// over the whole world" is affordable for one and not the other.
static uint64_t sieveCount(const MitmQuery *q)
{
    uint64_t n = 0;
    for (uint32_t yL = 0; yL < (1u << 24); yL++) n += mitmSieve(q, yL) != 0;
    return n;
}

// Cover the whole 2^48 space if the budget allows, otherwise as much of it as
// does -- centred so `mustContain` stays inside. Returns 1 if it is the lot.
static int pickWindow(const MitmQuery *q, double targetSec, uint64_t mustContain,
                      MitmRun *a, MitmRun *b, uint64_t *survOut)
{
    // MEASURE the sweep rate rather than predicting it. Sizing the window from
    // the survivor count alone assumed a fixed cost per (low, high) pair, and
    // that assumption breaks exactly where it matters: a large range with
    // averaged draws (mansion) leaves BOTH the sieve and the bucket key unable
    // to filter, so every pair is actually tested and the estimate was orders of
    // magnitude optimistic. A short probe over a known window gives the real
    // rate for this query, whatever regime it is in.
    // Grow the probe until it is above the clock's resolution, or it covers the
    // space. A probe that finishes in "0.0s" says nothing about the rate, and
    // treating that as the floor made a query the whole 2^48 space could afford
    // settle for a thousandth of it.
    uint64_t probe = 256, surv = 0;
    double sec = 0;
    while (1) {
        MitmRun p;
        memset(&p, 0, sizeof p);
        p.join = 0;
        p.window = 1; p.wsLo = 0; p.wsHi = probe << 24;
        mitmRun(q, &p, NULL, NULL);      // no callback: this is a stopwatch
        surv = p.survivors;
        sec  = p.sweepSec;
        if (sec >= 0.05 || probe >= (1u << 24)) break;
        probe *= 16;
        if (probe > (1u << 24)) probe = 1u << 24;
    }
    if (survOut) *survOut = surv;

    double want = (sec >= 0.05) ? (double)probe * (targetSec / sec)
                                : (double)(1u << 24);
    uint64_t highs = (want >= (double)(1u << 24)) ? (1u << 24) : (uint64_t)want;
    if (highs < 16) highs = 16;
    if (highs >= (1u << 24)) return 1;

    uint64_t len = highs << 24;
    uint64_t lo = (mustContain - len / 2) & ((1ULL << 48) - 1);
    MitmRun *rs[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        if (!rs[i]) continue;
        rs[i]->window = 1; rs[i]->wsLo = lo; rs[i]->wsHi = lo + len;
    }
    return 0;
}

// Drop the last structure from a query.
static int dropLast(MitmQuery *out, const MitmQuery *q)
{
    int rx[MITM_MAXN], rz[MITM_MAXN];
    uint64_t ox[MITM_MAXN], oz[MITM_MAXN];
    for (int j = 0; j + 1 < q->n; j++) {
        rx[j] = q->regX[j]; rz[j] = q->regZ[j];
        ox[j] = q->okx[j];  oz[j] = q->okz[j];
    }
    return mitmQueryInit(out, q->mc, q->st, q->n - 1, rx, rz, ox, oz);
}

// TEST C2. Solve for n-1 structures and test the last one with cubiomes, which
// never consults the sieve about it at all. That set must equal what the full
// n-structure solve returns. It is the check that matters when a search comes
// back EMPTY: an empty answer from a sieve is indistinguishable from a sieve
// that is broken, unless something that does not use the sieve agrees.
static int testLeaveOneOut(const MitmQuery *q, const char *name, uint64_t mustContain)
{
    MitmQuery qm;
    if (q->n < 2 || !dropLast(&qm, q)) return check(name, 1, "cannot drop a structure");

    // The reduced query is the expensive one -- one fewer constraint pair means
    // far more low halves survive -- so it sets the window, and the full query
    // is run over the same one.
    Set partial = {0}, filtered = {0}, full = {0};
    MitmRun r1 = {0}; r1.join = 1;
    // Wall-clock budget, measured per query (see pickWindow). The reduced query
    // is the expensive one -- one fewer constraint pair means far more low
    // halves survive -- so it sets the window, and the full query runs over the
    // same one.
    int whole = pickWindow(&qm, 25.0, mustContain, &r1, NULL, NULL);
    mitmRun(&qm, &r1, collect, &partial);
    for (size_t i = 0; i < partial.n; i++)
        if (refMatches(q, partial.v[i])) setPush(&filtered, partial.v[i]);

    MitmRun r2 = {0}; r2.join = 1;
    r2.window = r1.window; r2.wsLo = r1.wsLo; r2.wsHi = r1.wsHi;
    mitmRun(q, &r2, collect, &full);

    int bad = setDiff("leave-one-out", &filtered, "full solve", &full);
    char d[260];
    snprintf(d, sizeof d, "%s: %zu seeds place the first %d, %zu of those place all %d; "
             "full solve returns %zu -- %s",
             whole ? "all 2^48" : "windowed",
             partial.n, q->n - 1, filtered.n, q->n, full.n,
             bad ? "SETS DIFFER" : "identical sets");
    int rr = check(name, bad, d);
    setFree(&partial); setFree(&filtered); setFree(&full);
    return rr;
}

// TEST D. The nested sweep and the hash join must return the same set. The
// nested version is the obviously-correct one; if the join disagrees, the join
// is wrong. Run over the whole 2^48 space when the sweep can afford it, and
// over as much of it as it can afford otherwise -- and say which.
static int testJoinAgrees(const MitmQuery *q, const char *name, Set *keep,
                          uint64_t mustContain)
{
    Set a = {0}, b = {0};
    MitmRun r1 = {0}; r1.join = 0;
    MitmRun r2 = {0}; r2.join = 1;
    // Budget is wall-clock for the nested sweep: this test exists to hold the
    // join up against the obvious version, so the obvious version sets the pace.
    int whole = pickWindow(q, 25.0, mustContain, &r1, &r2, NULL);
    mitmRun(q, &r1, collect, &a);
    mitmRun(q, &r2, collect, &b);

    int bad = setDiff("nested", &a, "join", &b);
    char d[260];
    snprintf(d, sizeof d, "%" PRIu64 " low halves kept; %s; %zu seeds; nested tested %"
             PRIu64 " pairs (%.1fs), join %" PRIu64 " (%.1fs)", r1.survivors,
             whole ? "all 2^48" : "2^48 restricted to fit the sweep budget", a.n,
             r1.candidates, r1.lowSec + r1.sweepSec, r2.candidates, r2.lowSec + r2.sweepSec);
    int r = check(name, bad || a.n == 0, d);
    if (keep && !bad) { *keep = a; a.v = NULL; a.n = a.cap = 0; }
    setFree(&a); setFree(&b);
    return r;
}

// TEST E. tools/quad.c's engine, unchanged: solve the corner offset with
// invertCount, test the other six constraints. Whatever it finds, the
// meet-in-the-middle must accept -- if it does not, the MITM drops seeds.
typedef struct {
    const MitmQuery *q;
    int spread;
    ULONGLONG deadline;
    uint64_t visited;
    Set *out;
    CRITICAL_SECTION *lock;
    int ox, oz;
} QuadJob;

static int quadEngineCb(uint64_t ws, void *arg)
{
    QuadJob *j = arg;
    if ((++j->visited & 0xFFFFF) == 0 && GetTickCount64() > j->deadline) return 1;
    Pos p[4];
    if (!quadFits(j->q, ws, j->spread, p)) return 0;
    EnterCriticalSection(j->lock);
    setPush(j->out, ws);
    LeaveCriticalSection(j->lock);
    return 0;
}

static DWORD WINAPI quadEngineWorker(LPVOID arg)
{
    QuadJob *j = arg;
    invertCount(j->q->mc, j->q->st, 0, 0, j->ox, j->oz, 0, quadEngineCb, j);
    return 0;
}

// Run quad.c's search and return the bases it found.
static uint64_t quadEngine(const MitmQuery *q, int spread, int seconds, Set *out)
{
    StructureConfig sc;
    getStructureConfig(q->st, q->mc, &sc);
    int r = (int)sc.chunkRange, R = sc.regionSize;
    int lo = R - spread; if (lo < 0) lo = 0;
    int combos = r - lo;
    if (combos <= 0) return 0;

    CRITICAL_SECTION lock; InitializeCriticalSection(&lock);
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
    int n = combos * combos;
    QuadJob *jobs = calloc(n, sizeof *jobs);
    HANDLE *th = calloc(n, sizeof *th);
    int k = 0;
    for (int a = lo; a < r; a++)
        for (int b = lo; b < r; b++, k++) {
            jobs[k].q = q; jobs[k].spread = spread;
            jobs[k].ox = a; jobs[k].oz = b;
            jobs[k].deadline = deadline; jobs[k].out = out; jobs[k].lock = &lock;
            th[k] = CreateThread(NULL, 0, quadEngineWorker, &jobs[k], 0, NULL);
        }
    for (int i = 0; i < n; i += 64)
        WaitForMultipleObjects((n - i > 64) ? 64 : n - i, th + i, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(th[i]);
    uint64_t visited = 0;
    for (int i = 0; i < n; i++) visited += jobs[i].visited;
    free(jobs); free(th);
    DeleteCriticalSection(&lock);
    return visited;
}

// ---------------------------------------------------------------------------

// The exact offsets a known seed puts the four structures on -- used to build a
// query that is guaranteed to have at least one solution.
// Regions come from `shape` rather than being hardcoded at the origin: end
// cities do not generate within 1008 blocks of it, so the quad builder anchors
// theirs eight regions out, and an "exact" query pinned to the origin asked for
// four structures that cannot exist there.
static int exactQueryFrom(MitmQuery *q, const MitmQuery *shape, uint64_t ws)
{
    int mc = shape->mc, st = shape->st;
    const int *RX = shape->regX, *RZ = shape->regZ;
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;
    uint64_t okx[4], okz[4];
    for (int i = 0; i < 4; i++) {
        // getStructurePos: a monument's offset is the average of two draws, so
        // reading it off getFeaturePos built the "exact" query from positions
        // the engine never produces -- and then the anchor did not satisfy its
        // own query, which looked exactly like the solver dropping it.
        Pos p;
        if (!getStructurePos(st, mc, ws, RX[i], RZ[i], &p)) return 0;
        okx[i] = 1ULL << ((p.x >> 4) - RX[i] * sc.regionSize);
        okz[i] = 1ULL << ((p.z >> 4) - RZ[i] * sc.regionSize);
    }
    return mitmQueryInit(q, mc, st, 4, RX, RZ, okx, okz);
}

static int verify(int mc, int st)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) { printf("no config\n"); return 2; }
    uint32_t r = (uint32_t)sc.chunkRange;
    printf("%s: range %u, region %d chunks, salt %" PRId64 "\n",
           struct2str(st), r, sc.regionSize, (int64_t)sc.salt);

    MitmQuery q1, q2, q2x, q16;
    {   // one structure, one exact spot -- the shape invert.c verifies
        int rx[1] = {3}, rz[1] = {-5};
        uint64_t ox[1] = {1ULL << (7 % r)}, oz[1] = {1ULL << (19 % r)};
        if (!mitmQueryInit(&q1, mc, st, 1, rx, rz, ox, oz)) { printf("unsupported\n"); return 2; }
    }
    {   // two structures, a band of offsets each. The bands are fractions of the
        // range, not fixed numbers: an end city has range 9, so the old literal
        // band [9,14] selected no offset at all, the query was unsatisfiable by
        // construction, and --verify refused the structure as "unsupported".
        int rx[2] = {0, 1}, rz[2] = {0, 0};
        int w = (int)r / 3; if (w < 1) w = 1;
        uint64_t ox[2] = { bandMask(w, 2*w, r),           bandMask(0, w, r) };
        uint64_t oz[2] = { bandMask((int)r-1-w, (int)r-1, r), bandMask(w, 2*w, r) };
        if (!mitmQueryInit(&q2, mc, st, 2, rx, rz, ox, oz)) { printf("unsupported\n"); return 2; }
    }
    {   // two structures, both pinned exactly -- four exact constraints
        int rx[2] = {0, 1}, rz[2] = {0, 1};
        uint64_t ox[2] = { 1ULL << (5 % r), 1ULL << (17 % r) };
        uint64_t oz[2] = { 1ULL << (11 % r), 1ULL << (2 % r) };
        if (!mitmQueryInit(&q2x, mc, st, 2, rx, rz, ox, oz)) { printf("unsupported\n"); return 2; }
    }
    // How tight a cluster is worth asking this structure for? The two diagonal
    // members are at least regionSize-(range-1) chunks apart on BOTH axes, so
    // nothing beats that distance times sqrt(2) -- 13 for swamp huts, 23 for
    // ruined portals in their 40-chunk regions. Start there and loosen until
    // quad.c can actually produce a base to anchor on; hardcoding 16 asks
    // ruined portals for a cluster that provably cannot exist.
    int gap = sc.regionSize - ((int)r - 1);
    int minSpread = 1;
    while (minSpread * minSpread < 2 * gap * gap) minSpread++;

    int haveQuad = 0, quadSpread = 0;
    Set bases = {0};
    uint64_t walked = 0;
    for (int s = minSpread; s <= minSpread + 8 && !bases.n; s++) {
        if (!mitmQueryQuad(&q16, mc, st, s)) continue;
        haveQuad = 1; quadSpread = s;
        if (invertSupported(mc, st)) walked = quadEngine(&q16, s, 5, &bases);
        else {
            // quad.c cannot solve this placement, so look with this solver.
            // Bounded by WORK, not by emissions: a structure whose clusters do
            // not exist (mansions -- an offset of 59 needs both draws to be 59)
            // emits nothing at all, so an emission cap never fires and the
            // window is swept in full, nine times over. The window is sized to
            // the sieve instead, and shrinks when the sieve keeps everything.
            uint64_t surv = 0;
            for (uint32_t yL = 0; yL < (1u << 24); yL += 256)
                surv += mitmSieve(&q16, yL) != 0;
            surv = surv * 256 + 1;                       // sampled 1 in 256
            uint64_t highs = (uint64_t)2e9 / surv;
            if (highs < 16) highs = 16;
            if (highs > (1u << 24)) highs = 1u << 24;

            QuadCtx qc = { &q16, s, 0, 0, 99, &bases, 8 };
            MitmRun mr = {0};
            mr.join = 1; mr.limit = 400000;
            mr.window = 1; mr.wsLo = 0; mr.wsHi = highs << 24;
            mitmRun(&q16, &mr, onQuad, &qc);
        }
    }
    if (haveQuad)
        printf("tightest cluster geometry allows: spread %d; anchoring at spread %d\n",
               minSpread, quadSpread);

    printf("\nA. acceptance vs cubiomes' placement, both directions\n");
    testPredicate(&q1,  "1 structure, exact",        0, 8000000);
    testPredicate(&q1,  "1 structure, high seeds",   0x0007ffffff000000ULL, 8000000);
    testPredicate(&q2,  "2 structures, bands",       0, 8000000);
    testPredicate(&q2,  "2 structures, high seeds",  0x00c0ffee00000000ULL, 8000000);
    testPredicate(&q2x, "2 structures, exact",       0, 40000000);
    if (haveQuad) {
        testPredicate(&q16, "4 structures, quad geometry",  0, 8000000);
        testPredicate(&q16, "4 structures, high seeds",     0x0000abcdef000000ULL, 8000000);
    }

    printf("\nB. exhaustive over a window: brute force vs solver, same seeds?\n");
    testWindow(&q1,  "1 structure, nested",  0x0000123400000000ULL, 4000000, 0);
    testWindow(&q1,  "1 structure, join",    0x0000123400000000ULL, 4000000, 1);
    testWindow(&q2,  "2 structures, nested", 0x0000000fff000000ULL, 4000000, 0);
    testWindow(&q2,  "2 structures, join",   0x0000000fff000000ULL, 4000000, 1);
    if (haveQuad) {
        testWindow(&q16, "quad geometry, nested",  0x0000777700000000ULL, 16000000, 0);
        testWindow(&q16, "quad geometry, join",    0x0000777700000000ULL, 16000000, 1);
    }

    printf("\nC. low halves the sieve discarded -- try every high half anyway\n");
    testRejections(&q1,  "1 structure, exact", 8);
    testRejections(&q2x, "2 structures, exact", 8);

    // ---- the four-structure case, anchored on a seed quad.c found ----------
    printf("\nD. four structures, anchored on a real cluster\n");
    // tools/quad.c is built on src/invert.c, which only knows the plain
    // remainder placement -- it cannot produce a base for an ancient city
    // (power-of-two range) or a monument (averaged draws). Where it CAN, its
    // output is the cross-check. Where it cannot, say so and take the anchor
    // from this solver instead: every test below still measures against
    // cubiomes' getStructurePos, so the anchor's provenance changes nothing
    // except which claim section D is entitled to make.
    int quadcAble = invertSupported(mc, st);
    if (haveQuad && !quadcAble) {
        skip("cross-check vs quad.c engine",
             "quad.c cannot solve this placement (invert.c handles the plain "
             "remainder form only); anchor taken from this solver and re-verified below");
    }

    // What sections D and E actually need is a four-structure query that HAS
    // solutions -- a cluster is just the most interesting kind. Mansions have
    // none at any tight spread (an offset of 59 needs both draws to be 59, one
    // chance in 3600, and a cluster needs two such corners), so demanding one
    // failed a structure whose solver is fine. Any seed's own offsets do.
    uint64_t anchor;
    int haveCluster = (haveQuad && bases.n > 0);
    if (haveCluster) { setSort(&bases); anchor = bases.v[0]; }
    else {
        anchor = 0x1F2E3D4C5B6AULL;
        char d[200];
        snprintf(d, sizeof d, "none exists at spread %d..%d; anchoring on an "
                 "ordinary seed's own offsets instead, which is all the "
                 "8-constraint tests need", minSpread, minSpread + 8);
        skip("a four-structure cluster to anchor on", d);
    }
    {
        if (quadcAble && haveCluster) {
            uint64_t rejected = 0;
            for (size_t i = 0; i < bases.n; i++)
                if (!mitmWouldReach(&q16, bases.v[i])) {
                    if (rejected < 3) printf("       MITM would never reach %" PRIu64 "\n", bases.v[i]);
                    rejected++;
                }
            char d[200];
            snprintf(d, sizeof d, "quad.c walked %" PRIu64 " seeds, found %zu bases at spread %d;"
                     " MITM rejects %" PRIu64, walked, bases.n, quadSpread, rejected);
            check("every quad.c base is reachable", rejected != 0, d);
        }

        // Whatever produced the anchor, cubiomes has to agree it is a cluster.
        if (haveCluster) {
            Pos p[4];
            char d[120];
            snprintf(d, sizeof d, "seed %" PRIu64 " at spread %d", anchor, quadSpread);
            check("the anchor is a real cluster per cubiomes",
                  !quadFits(&q16, anchor, quadSpread, p), d);
        }
        MitmQuery qe;
        if (!exactQueryFrom(&qe, &q16, anchor)) {
            check("build exact query from base", 1, "");
        } else {
            char nm[80];
            snprintf(nm, sizeof nm, "8 exact constraints, window");
            testWindow(&qe, nm, (anchor - 2000000) & ((1ULL << 48) - 1), 4000000, 0);
            testWindow(&qe, "8 exact constraints, join", (anchor - 2000000) & ((1ULL << 48) - 1), 4000000, 1);
            testRejections(&qe, "8 exact, rejected low halves", 8);
            testLeaveOneOut(&qe, "3 solved + 1 tested == 4 solved", anchor);

            printf("\nE. nested sweep vs hash join, same set?\n");
            Set all = {0};
            testJoinAgrees(&qe, "8 exact constraints", &all, anchor);
            uint64_t bad = 0, seen = 0;
            for (size_t i = 0; i < all.n; i++) {
                if (!refMatches(&qe, all.v[i])) bad++;
                if (all.v[i] == anchor) seen = 1;
            }
            char d2[200];
            snprintf(d2, sizeof d2, "%zu seeds, %" PRIu64 " fail cubiomes' own placement", all.n, bad);
            check("every emitted seed re-verified", bad != 0, d2);
            check("the anchor base is in the result", !seen,
                  seen ? "quad.c's seed came back out of the full enumeration" : "MISSING");
            setFree(&all);
        }
    }
    setFree(&bases);

    printf("\n%d passed, %d failed", pass, failed);
    if (skipped) printf(", %d not applicable (target too rare to sample)", skipped);
    printf("\n");
    printf(failed ? "\nMITM UNSOUND OR INCOMPLETE\n" : "\nOK\n");
    return failed != 0;
}

// ---------------------------------------------------------------------------

// Would src/solve.c have produced this seed? The wired search replaces a scan
// with the solver's candidate list, so the question that matters is not whether
// its candidates are good -- pass 1 judges those -- but whether the list MISSES
// clusters the scan would have found. Given a seed, find its cluster, normalise
// it back to the origin corner the solver enumerates, and ask the solver's own
// acceptance test. Anything the scan finds and this rejects is a dropped seed.
static int coversSeed(int mc, int st, int spreadBlocks, uint64_t ws, int wr)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;
    int64_t spr2 = (int64_t)spreadBlocks * spreadBlocks;

    for (int rx = -wr; rx <= wr; rx++)
    for (int rz = -wr; rz <= wr; rz++) {
        int RX[4] = {rx, rx+1, rx, rx+1}, RZ[4] = {rz, rz, rz+1, rz+1};
        Pos p[4];
        int all = 1;
        for (int i = 0; i < 4 && all; i++)
            if (!getStructurePos(st, mc, ws, RX[i], RZ[i], &p[i])) all = 0;
        if (!all) continue;

        for (int a = 0; a < 4; a++) {
            int fits = 1;
            for (int b = 0; b < 4 && fits; b++) {
                int64_t dx = p[a].x - p[b].x, dz = p[a].z - p[b].z;
                if (dx*dx + dz*dz > spr2) fits = 0;
            }
            if (!fits) continue;
            MitmQuery mq;
            if (!mitmQueryCluster(&mq, mc, st, spreadBlocks, a)) continue;
            uint64_t base = moveStructure(ws, -rx, -rz) & ((1ULL << 48) - 1);
            if (mitmWouldReach(&mq, base)) return 1;
        }
    }
    return 0;
}

// Brute force structure seeds for clusters and check the solver would propose
// every one. This is the completeness test for src/solve.c specifically: the
// solver only enumerates a 2x2 region corner, one anchor at a time, and the
// claim is that nothing else can satisfy a tight cluster rule. A miss here means
// the wired search would silently skip seeds the plain scan finds.
static int coversScan(int mc, int st, int spread, uint64_t count)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 2;
    int64_t spr2 = (int64_t)spread * spread;
    uint64_t hits = 0, miss = 0;

    for (uint64_t ws = 0; ws < count; ws++) {
        // Any 2x2 corner near the origin -- the same shapes solve.c enumerates,
        // plus the surrounding ones, so a cluster in a shape it does NOT
        // enumerate would show up here as an uncovered hit.
        for (int rx = -2; rx <= 2; rx++)
        for (int rz = -2; rz <= 2; rz++) {
            int RX[4] = {rx, rx+1, rx, rx+1}, RZ[4] = {rz, rz, rz+1, rz+1};
            Pos p[4];
            int all = 1;
            for (int i = 0; i < 4 && all; i++)
                if (!getStructurePos(st, mc, ws, RX[i], RZ[i], &p[i])) all = 0;
            if (!all) continue;
            int fits = 0;
            for (int a = 0; a < 4 && !fits; a++) {
                int ok = 1;
                for (int b = 0; b < 4 && ok; b++) {
                    int64_t dx = p[a].x - p[b].x, dz = p[a].z - p[b].z;
                    if (dx*dx + dz*dz > spr2) ok = 0;
                }
                fits = ok;
            }
            if (!fits) continue;
            hits++;
            if (!coversSeed(mc, st, spread, ws, 4)) {
                if (miss < 4) printf("NOT COVERED seed %" PRIu64 " corner (%d,%d)\n", ws, rx, rz);
                miss++;
            }
            rx = 3; break;                  // one cluster per seed is enough
        }
    }
    printf("%" PRIu64 " clusters found by brute force in %" PRIu64 " structure seeds; "
           "%" PRIu64 " the solver would never propose\n", hits, count, miss);
    return (miss || hits == 0) ? 1 : 0;
}

static int coversMode(int mc, int st, int spread)
{
    char line[256];
    uint64_t n = 0, miss = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        int neg = 0;
        while (*p && *p != '-' && (*p < '0' || *p > '9')) p++;
        if (*p == '-') { neg = 1; p++; }
        if (!*p) continue;
        uint64_t ws = strtoull(p, NULL, 10);
        if (neg) ws = (uint64_t)(-(int64_t)ws);
        ws &= (1ULL << 48) - 1;
        n++;
        if (!coversSeed(mc, st, spread, ws, 20)) {
            printf("NOT COVERED %" PRIu64 "\n", ws);
            miss++;
        }
    }
    printf("%" PRIu64 " scan hits checked, %" PRIu64 " the solver would never propose\n",
           n, miss);
    return miss ? 1 : 0;
}

static int acceptsMode(int mc, int st, int spread)
{
    MitmQuery q;
    if (!mitmQueryQuad(&q, mc, st, spread)) { fprintf(stderr, "bad query\n"); return 2; }
    char line[256];
    uint64_t n = 0, rej = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (!*p) continue;
        uint64_t ws = strtoull(p, NULL, 10);
        n++;
        if (!mitmWouldReach(&q, ws)) {
            printf("REJECTED %" PRIu64 "\n", ws);
            rej++;
        }
    }
    printf("%" PRIu64 " seeds checked, %" PRIu64 " the MITM would never reach\n", n, rej);
    return rej ? 1 : 0;
}

int main(int argc, char **argv)
{
    // The verify pass runs for minutes; under a pipe a fully-buffered stdout
    // would show nothing until it finished, which makes a slow check look like
    // a hang. Windows' CRT maps _IOLBF onto full buffering, so it has to be
    // unbuffered to get progress out at all.
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 4) {
        fprintf(stderr,
            "usage: mitm <version> <structure> quad <spread> [options]\n"
            "         --nested       sweep every high half instead of the bucket join\n"
            "         --limit N      stop once N quad bases are found (threads finish\n"
            "                        their current block, so you get at least N)\n"
            "         --threads T    default: one per core\n"
            "         --window N     only seeds 0..N, for a bounded run\n"
            "       mitm <version> <structure> --verify\n"
            "       mitm <version> <structure> --accepts <spread>   (seeds on stdin)\n");
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
    if (!mitmSupported(mc, st)) {
        fprintf(stderr, "%s does not use the invertible placement\n", argv[2]);
        return 2;
    }

    if (!strcmp(argv[3], "--verify")) return verify(mc, st);
    if (!strcmp(argv[3], "--covers")) {
        if (argc < 5) { fprintf(stderr, "need a spread in blocks\n"); return 2; }
        if (argc > 5) return coversScan(mc, st, atoi(argv[4]), strtoull(argv[5], NULL, 10));
        return coversMode(mc, st, atoi(argv[4]));
    }
    if (!strcmp(argv[3], "--accepts")) {
        if (argc < 5) { fprintf(stderr, "need a spread\n"); return 2; }
        return acceptsMode(mc, st, atoi(argv[4]));
    }
    if (strcmp(argv[3], "quad") || argc < 5) {
        fprintf(stderr, "unknown mode \"%s\"\n", argv[3]);
        return 2;
    }

    int spread = atoi(argv[4]);
    uint64_t wantBases = 0;
    MitmRun run = {0};
    run.join = 1;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--nested")) run.join = 0;
        else if (!strcmp(argv[i], "--join")) run.join = 1;
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) wantBases = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) run.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            run.window = 1; run.wsLo = 0; run.wsHi = strtoull(argv[++i], NULL, 10);
        }
    }

    MitmQuery q;
    if (!mitmQueryQuad(&q, mc, st, spread)) {
        StructureConfig sc; getStructureConfig(st, mc, &sc);
        printf("IMPOSSIBLE: with range %d in a %d-chunk region, four %ss can never "
               "come within %d chunks of each other.\n",
               (int)sc.chunkRange, sc.regionSize, argv[2], spread);
        return 1;
    }

    StructureConfig sc; getStructureConfig(st, mc, &sc);
    int nx = 0; for (uint32_t o = 0; o < q.range; o++) nx += (q.okx[0] >> o) & 1;
    printf("%s: range %u, region %d chunks, spread %d\n",
           argv[2], q.range, q.regionSize, spread);
    printf("corner offsets worth solving: %d of %u per axis; the sieve pins the high\n"
           "half to one residue mod %u per constraint, eight constraints at once\n\n",
           nx, q.range, q.m);

    QuadCtx ctx = { &q, spread, 0, 0, 0, NULL, wantBases };
    mitmRun(&q, &run, onQuad, &ctx);

    printf("\n--- funnel ---\n");
    printf("low halves      : %" PRIu64 " of 16777216 survive the sieve (%.1fs)\n",
           run.survivors, run.lowSec);
    printf("pairs tested    : %" PRIu64 "  (%s, %.1fs)\n",
           run.candidates, run.join ? "hash join" : "nested sweep", run.sweepSec);
    printf("placements hit  : %" PRIu64 "  (all four on allowed offsets)\n", run.hits);
    printf("quad bases      : %" PRIu64 "  (all four in one despawn sphere)\n", ctx.found);
    printf("quad.c would have walked ~%" PRIu64 " seeds to cover the same ground\n",
           (uint64_t)((1ULL << 48) / ((uint64_t)q.range * q.range)));

    // An empty answer from a sieve looks exactly like a broken sieve. When the
    // search finds nothing, solve for three of the four and test the fourth
    // with cubiomes -- a path that never asks the sieve about that structure.
    if (run.hits == 0 && !run.window) {
        MitmQuery qm;
        double expect = (double)(1ULL << 48);
        for (int j = 0; j + 1 < q.n; j++) {
            int cx = 0, cz = 0;
            for (uint32_t o = 0; o < q.range; o++) { cx += (q.okx[j] >> o) & 1; cz += (q.okz[j] >> o) & 1; }
            expect *= (double)cx * cz / ((double)q.range * q.range);
        }
        if (expect < 2e8 && dropLast(&qm, &q)) {
            printf("\nnothing found -- corroborating without the sieve's opinion on the\n"
                   "fourth structure: solve for three, test the fourth with cubiomes\n");
            Set partial = {0};
            MitmRun r = {0}; r.join = 1;
            mitmRun(&qm, &r, collect, &partial);
            uint64_t both = 0;
            for (size_t i = 0; i < partial.n; i++) if (refMatches(&q, partial.v[i])) both++;
            printf("  %zu seeds place the first three exactly; %" PRIu64
                   " of them also place the fourth\n", partial.n, both);
            printf("  %s\n", both == 0
                   ? "independent agreement: there is no such cluster in 2^48 seeds"
                   : "DISAGREEMENT -- the sieve dropped seeds");
            setFree(&partial);
        }
    }
    return ctx.found ? 0 : 1;
}
