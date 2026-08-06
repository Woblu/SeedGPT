// invert -- solve for seeds that put a structure at an exact spot, and prove it.
//
//   invert <version> <structure> <regX> <regZ> <ox> <oz> [count]
//   invert <version> <structure> --verify
//
// The first form enumerates world seeds placing `structure` in region
// (regX,regZ) at chunk offset (ox,oz) inside it, checking every one against
// cubiomes' own getStructurePos before printing it.
//
// The second form is the one that matters. An inverter that silently misses
// solutions is worse than no inverter -- a search built on it would report
// "nothing found" for seeds that exist. So --verify checks both directions:
//
//   SOUND     every seed it emits really does place the structure there,
//             according to the forward code it does not share any lines with.
//
//   COMPLETE  brute-force a range of world seeds, keep the ones that land on
//             the target, and confirm the inverter's own constraint accepts
//             every single one. A gap in the residue-class reasoning shows up
//             here as a hit the inverter would never have visited.
#include "invert.h"
#include "mitm.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define K 0x5deece66dULL
#define M ((1ULL << 48) - 1)

typedef struct { int mc, st, regX, regZ, ox, oz; int bad; int shown; } Ctx;

static int emit(uint64_t ws, void *arg)
{
    Ctx *c = arg;
    Pos p;
    // Independent forward check: cubiomes' placement, not ours.
    if (!getStructurePos(c->st, c->mc, ws, c->regX, c->regZ, &p)) { c->bad++; return 0; }
    StructureConfig sc; getStructureConfig(c->st, c->mc, &sc);
    int gotx = (p.x >> 4) - c->regX * sc.regionSize;
    int gotz = (p.z >> 4) - c->regZ * sc.regionSize;
    if (gotx != c->ox || gotz != c->oz) {
        if (c->bad < 5)
            printf("  MISMATCH seed %" PRIu64 " -> offset (%d,%d), wanted (%d,%d)\n",
                   ws, gotx, gotz, c->ox, c->oz);
        c->bad++;
        return 0;
    }
    if (c->shown < 8) {
        printf("  seed %-20" PRIu64 "  chunk (%d,%d)  block (%d,%d)\n",
               ws, p.x >> 4, p.z >> 4, p.x, p.z);
        c->shown++;
    }
    return 0;
}

static int verify(int mc, int st)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) { printf("no config\n"); return 2; }
    uint64_t r = sc.chunkRange;
    printf("%s: range %" PRIu64 ", salt %" PRId64 ", region %d chunks\n",
           struct2str(st), r, (int64_t)sc.salt, sc.regionSize);

    int fail = 0;

    // ---- soundness ----------------------------------------------------
    int sound = 0, unsound = 0;
    for (int t = 0; t < 12; t++) {
        int regX = (t * 7) - 40, regZ = (t * 13) - 25;
        int ox = (t * 5) % (int)r, oz = (t * 11) % (int)r;
        Ctx c = { mc, st, regX, regZ, ox, oz, 0, 99 };
        uint64_t n = invertCount(mc, st, regX, regZ, ox, oz, 40, emit, &c);
        if (n == 0) { printf("  no solutions for region (%d,%d) off (%d,%d)\n", regX, regZ, ox, oz); unsound++; }
        else if (c.bad) unsound++;
        else sound++;
    }
    printf("soundness: %d/%d targets, every emitted seed re-verified\n", sound, sound + unsound);
    if (unsound) fail = 1;

    // ---- completeness --------------------------------------------------
    // Brute force a slab of world seeds; every one that lands on the target
    // must satisfy the constraint the enumeration walks.
    const int regX = 3, regZ = -5;
    const uint64_t SLAB = 4000000;
    uint64_t off = (uint64_t)regX * 341873128712ULL
                 + (uint64_t)regZ * 132897987541ULL + (uint64_t)sc.salt;
    uint64_t hits = 0, missed = 0;
    int tox = 7 % (int)r, toz = 19 % (int)r;

    // The meet-in-the-middle solver (src/mitm.c) answers the same question for
    // many structures at once, and it can fail the same silent way. Rather than
    // stand up a second brute force for it, point this one at it too: the same
    // hits, the same demand that none be missed.
    MitmQuery mq;
    int mrx[1] = { regX }, mrz[1] = { regZ };
    uint64_t mokx[1] = { 1ULL << tox }, mokz[1] = { 1ULL << toz };
    int haveMitm = mitmQueryInit(&mq, mc, st, 1, mrx, mrz, mokx, mokz);
    uint64_t mitmMissed = 0;

    for (uint64_t ws = 0; ws < SLAB; ws++) {
        Pos p;
        if (!getStructurePos(st, mc, ws, regX, regZ, &p)) continue;
        int gx = (p.x >> 4) - regX * sc.regionSize;
        int gz = (p.z >> 4) - regZ * sc.regionSize;
        if (gx != tox || gz != toz) continue;
        hits++;
        if (haveMitm && !mitmWouldReach(&mq, ws)) mitmMissed++;
        // Would the enumeration have reached this seed? Reproduce its s1 and
        // check the residue class, then round-trip back to the seed.
        uint64_t s1 = lcgForward((ws + off) ^ K);
        if ((uint64_t)((uint32_t)(s1 >> 17)) % r != (uint64_t)tox) { missed++; continue; }
        uint64_t back = ((lcgBack(s1) ^ K) - off) & M;
        if (back != (ws & M)) { missed++; continue; }
    }
    printf("completeness: %" PRIu64 " brute-force hits in %" PRIu64 " seeds, "
           "%" PRIu64 " the enumeration would have missed\n", hits, SLAB, missed);
    if (missed || hits == 0) fail = 1;

    if (haveMitm) {
        printf("completeness (meet-in-the-middle): the same %" PRIu64 " hits, %"
               PRIu64 " the MITM sieve would have discarded\n", hits, mitmMissed);
        if (mitmMissed) fail = 1;
    } else {
        printf("completeness (meet-in-the-middle): not applicable to this placement\n");
    }

    // ---- what it buys ---------------------------------------------------
    double expect = (double)SLAB / ((double)r * r);
    printf("density: %" PRIu64 " hits vs %.0f expected at 1-in-range^2 "
           "(%.0fx less space to search than scanning)\n",
           hits, expect, (double)r * r);

    printf(fail ? "\nINVERTER UNSOUND OR INCOMPLETE\n" : "\nOK\n");
    return fail;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: invert <version> <structure> <regX> <regZ> <ox> <oz> [count]\n"
            "       invert <version> <structure> --verify\n");
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
        fprintf(stderr, "%s does not use the invertible placement "
                        "(different draw count or a rejection roll)\n", argv[2]);
        return 2;
    }
    if (argc > 3 && !strcmp(argv[3], "--verify")) return verify(mc, st);
    if (argc < 7) { fprintf(stderr, "need regX regZ ox oz\n"); return 2; }

    int regX = atoi(argv[3]), regZ = atoi(argv[4]);
    int ox = atoi(argv[5]), oz = atoi(argv[6]);
    uint64_t want = (argc > 7) ? strtoull(argv[7], NULL, 10) : 8;

    Ctx c = { mc, st, regX, regZ, ox, oz, 0, 0 };
    uint64_t n = invertCount(mc, st, regX, regZ, ox, oz, want, emit, &c);
    printf("%" PRIu64 " seeds emitted, %d failed re-verification\n", n, c.bad);
    return c.bad ? 1 : 0;
}
