#include "solve.h"
#include <stdarg.h>
#include "mitm.h"
#include "finders.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define M48 ((1ULL << 48) - 1)

static void say(char *why, size_t n, const char *fmt, ...)
{
    if (!why || !n) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

int solvePick(const Query *q, char *why, size_t whylen)
{
    say(why, whylen, "no tight-cluster condition to solve");
    for (int k = 0; k < q->n; k++) {
        const Cond *c = &q->cond[k];
        if (c->type != CT_STRUCTURE || c->spread <= 0 || c->structMin < 2) continue;

        const char *nm = struct2str(c->structType);
        if (c->structMin != 4) {
            say(why, whylen, "cluster of %d not solved (only 4 is implemented; "
                "the corner argument is what makes 4 exact)", c->structMin);
            continue;
        }
        if (c->parent != PARENT_ORIGIN) {
            // Spawn is not known until the 64-bit stage, and a child condition's
            // anchor is not known at all in pass 1, so the region window the
            // cluster may occupy cannot be bounded here.
            say(why, whylen, "cluster is measured from \"%s\", not the origin -- "
                "its region window is not known at the 48-bit stage", c->ofId);
            continue;
        }
        if (!mitmSupported(q->mc, c->structType)) {
            say(why, whylen, "%s placement cannot be solved (per-chunk or "
                "ring-based, not region-and-offset)", nm ? nm : "structure");
            continue;
        }
        if (!mitmCornerIsOnlyShape(q->mc, c->structType, c->spread)) {
            say(why, whylen, "spread %d is wide enough that four %ss could cluster "
                "outside one region corner, which the solver does not enumerate",
                c->spread, nm ? nm : "structure");
            continue;
        }
        MitmQuery mq;
        int any = 0;
        for (int a = 0; a < 4; a++)
            if (mitmQueryCluster(&mq, q->mc, c->structType, c->spread, a)) { any = 1; break; }
        if (!any) {
            say(why, whylen, "no four %ss can be that close (spread %d)",
                nm ? nm : "structure", c->spread);
            continue;
        }
        say(why, whylen, "solving \"%s\": 4 %ss within %d blocks",
            c->id, nm ? nm : "structure", c->spread);
        return k;
    }
    return -1;
}

// ---------------------------------------------------------------------------

typedef struct {
    const Query *q;
    const Cond  *c;
    uint64_t    *out;
    uint64_t     n, want;
    int          nreg;          // region offsets each base expands into
    int          r0;            // first region offset index
    int          wr;
    uint64_t     seen;          // solver hits offered, real or not
    ULONGLONG    deadline;
    int          timedOut;
} Fill;

// query.c's rule, checked against cubiomes' own placement: some member has the
// other three within `spread`. The solver's offset sets are a relaxation of
// this, so confirming it here keeps junk out of the candidate list -- pass 1
// would reject it anyway, just later and after paying for it.
static int realCluster(const Query *q, const Cond *c, uint64_t s48, int o)
{
    Pos p[4];
    int rx[4] = {o, o+1, o, o+1}, rz[4] = {o, o, o+1, o+1};
    for (int i = 0; i < 4; i++)
        if (!getStructurePos(c->structType, q->mc, s48, rx[i], rz[i], &p[i]))
            return 0;
    int64_t spr2 = (int64_t)c->spread * c->spread;
    for (int a = 0; a < 4; a++) {
        int all = 1;
        for (int b = 0; b < 4 && all; b++) {
            int64_t dx = p[a].x - p[b].x, dz = p[a].z - p[b].z;
            if (dx*dx + dz*dz > spr2) all = 0;
        }
        if (all) return 1;
    }
    return 0;
}

static int onBase(uint64_t s48, void *arg)
{
    Fill *f = arg;
    // Checked here rather than in the solver: this callback runs under the
    // solver's emit lock, so it is the one place guaranteed to be reached.
    if ((++f->seen & 0x3FF) == 0 && GetTickCount64() > f->deadline) { f->timedOut = 1; return 1; }
    if (!realCluster(f->q, f->c, s48, f->r0)) return 0;

    // One base seed is one cluster SHAPE; moving it by whole regions puts that
    // shape somewhere else, and each destination is a different world. Emit
    // every placement whose cluster can still be inside the search radius.
    for (int rx = -f->wr; rx <= f->wr && f->n < f->want; rx++)
    for (int rz = -f->wr; rz <= f->wr && f->n < f->want; rz++)
        f->out[f->n++] = moveStructure(s48, rx - f->r0, rz - f->r0) & M48;

    return f->n >= f->want;
}

uint64_t solveFill(const Query *q, int k, uint64_t want, uint64_t *out, int threads,
                   double maxSeconds, int *timedOut)
{
    if (timedOut) *timedOut = 0;
    const Cond *c = &q->cond[k];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;

    Fill f;
    memset(&f, 0, sizeof f);
    f.q = q; f.c = c; f.out = out; f.want = want;

    // How far the corner may be moved and still land inside `within`. The
    // cluster spans about two regions, so allow that much slack on top.
    double span = sc.regionSize * 16.0;
    f.wr = (int)ceil(c->within / span) + 2;
    if (f.wr < 1) f.wr = 1;
    f.nreg = (2 * f.wr + 1) * (2 * f.wr + 1);
    f.deadline = GetTickCount64() + (ULONGLONG)(maxSeconds * 1000.0);

    // The cluster rule is "SOME member has the other three close", which is four
    // separate questions. Asking them together forces the offset sets to be the
    // union of four tight answers, and that union is loose enough to leave the
    // low-half sieve with nothing to reject -- the solver then grinds instead of
    // solving. Asked one at a time, each stays tight.
    for (int a = 0; a < 4 && f.n < want && !f.timedOut; a++) {
        MitmQuery mq;
        if (!mitmQueryCluster(&mq, q->mc, c->structType, c->spread, a)) continue;
        f.r0 = mq.regX[0];                   // where the solver anchored the corner
        MitmRun run;
        memset(&run, 0, sizeof run);
        run.join = 1;
        run.threads = threads;
        // No run.limit: the solver's offset sets are a relaxation, so most of
        // what it emits is filtered out by realCluster. Capping the solver's own
        // hit count stops it long before the candidate list is full -- which is
        // exactly what happened, and looked like "there are none". onBase ends
        // the run instead, once `want` candidates really exist.
        mitmRun(&mq, &run, onBase, &f);
    }
    if (GetTickCount64() > f.deadline) f.timedOut = 1;
    if (timedOut) *timedOut = f.timedOut;
    return f.n;
}
