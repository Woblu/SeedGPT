#include "mitm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define K    0x5deece66dULL
#define B    0xbULL
#define M48  ((1ULL << 48) - 1)
#define M24  ((1u << 24) - 1)
#define KL   ((uint32_t)(K & M24))          // 0xece66d
#define KH   ((uint32_t)(K >> 24))          // 0x5de
#define HALF (1u << 24)

// ---------------------------------------------------------------------------
// small arithmetic
// ---------------------------------------------------------------------------

static uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

// x^-1 mod m by trial; m <= 64 here, so nothing cleverer is warranted.
static uint32_t modinv(uint32_t x, uint32_t m)
{
    if (m == 1) return 0;
    x %= m;
    for (uint32_t i = 1; i < m; i++) if ((uint64_t)x * i % m == 1) return i;
    return 0;
}

// v % r for v < 2^31 by multiply-shift. magic = ceil(2^36/r); the identity
// holds exactly when v*(magic*r - 2^36) < 2^36, which is checked at init --
// so this is a proven substitution, not a hopeful one.
static inline uint32_t fmodr(const MitmQuery *q, uint32_t v)
{
    if (q->fmagic) {
        uint32_t d = (uint32_t)(((uint64_t)v * q->fmagic) >> 36);
        return v - d * q->range;
    }
    return v % q->range;
}

// ---------------------------------------------------------------------------
// the concrete test -- cubiomes' own formula, offsets folded in
// ---------------------------------------------------------------------------
//
// y = seed48 + off[0], so y + D[j] = seed48 + off[j] is exactly the value
// getFeatureChunkInRegion hands to setSeed. Nothing here is approximate: this
// is the placement, computed. Every seed the solver emits has passed it.

// One draw. The two branches are the two branches of Java's nextInt: a
// remainder for a general bound, a scaling for a power of two.
static inline uint32_t drawOff(const MitmQuery *q, uint64_t s)
{
    uint32_t bits = (uint32_t)(s >> 17);
    if (q->pow2) return (uint32_t)(((uint64_t)q->range * bits) >> 31);
    return fmodr(q, bits);
}

// getStructurePos' rejection rolls, reproduced. Both are functions of the
// 48-bit seed and the chunk the structure landed in, so they cost one more
// LCG walk per candidate and nothing at all in the sieve.
static inline int rollPasses(const MitmQuery *q, uint64_t seed48, int cx, int cz)
{
    uint64_t s;
    if (q->roll == MITM_ROLL_OUTPOST) {
        s = seed48 ^ ((uint64_t)(cx >> 4) ^ ((uint64_t)(cz >> 4) << 4));
        setSeed(&s, s);
        next(&s, 31);                       // setAttemptSeed discards one draw
        return nextInt(&s, 5) == 0;
    }
    if (q->roll == MITM_ROLL_BASTION) {
        s = chunkGenerateRnd(seed48, cx, cz);
        return nextInt(&s, 5) >= 2;
    }
    return 1;
}

static inline int testY(const MitmQuery *q, uint64_t y)
{
    uint64_t seed48 = 0;
    if (q->roll || q->endCity) seed48 = (y - q->off[0]) & M48;

    for (int j = 0; j < q->n; j++) {
        uint64_t s = ((y + q->D[j]) & M48) ^ K;
        uint32_t ox, oz;
        s = (s * K + B) & M48;  ox = drawOff(q, s);
        if (q->family == MITM_LARGE) {
            s = (s * K + B) & M48;  ox += drawOff(q, s);
            s = (s * K + B) & M48;  oz  = drawOff(q, s);
            s = (s * K + B) & M48;  oz += drawOff(q, s);
            ox >>= 1; oz >>= 1;
        } else {
            s = (s * K + B) & M48;  oz = drawOff(q, s);
        }
        if (!((q->okx[j] >> ox) & 1)) return 0;
        if (!((q->okz[j] >> oz) & 1)) return 0;

        if (q->roll || q->endCity) {
            int cx = q->regX[j] * q->regionSize + (int)ox;
            int cz = q->regZ[j] * q->regionSize + (int)oz;
            if (q->endCity) {
                int64_t bx = (int64_t)cx << 4, bz = (int64_t)cz << 4;
                if (bx*bx + bz*bz < 1008LL*1008LL) return 0;
            }
            if (q->roll && !rollPasses(q, seed48, cx, cz)) return 0;
        }
    }
    return 1;
}

int mitmMatches(const MitmQuery *q, uint64_t seed48)
{
    return testY(q, (seed48 + q->off[0]) & M48);
}

// ---------------------------------------------------------------------------
// the sieve on the low half
// ---------------------------------------------------------------------------
//
//   lowsum = yL + D_jL          c_j = lowsum >> 24      (the only thing the low
//   s0L    = (lowsum & M24) ^ KL                         half hands upward,
//   P      = s0L*K + b                                   besides P >> 24)
//   s1L    = P & M24            PH_j = (P >> 24) & M24
//
// s1 >> 17 = s1H*128 + (s1L >> 17), so pinning an offset to a set of values
// pins s1H to a set of residues mod m = r/gcd(128,r). If that set is empty the
// low half is dead -- no high half can rescue it, and none is ever tried. With
// a power-of-two range the offset does not depend on the low half at all, so
// the sieve correctly rejects nothing and the join does the work.

typedef struct {
    uint32_t cpat;                      // carry bits c_j
    uint32_t PH[MITM_MAXN];             // first draw's high-half addend
    uint64_t e0[MITM_MAXN];             // first draw's allowed residues
} MitmLow;

static inline int lowSieve(const MitmQuery *q, uint32_t yL, MitmLow *lo)
{
    uint32_t cpat = 0;
    for (int j = 0; j < q->n; j++) {
        uint32_t sum = yL + (uint32_t)(q->D[j] & M24);
        cpat |= (sum >> 24) << j;
        uint32_t sL = (sum & M24) ^ KL;
        for (int d = 0; d < q->ndraw; d++) {
            uint64_t P = (uint64_t)sL * K + B;
            sL = (uint32_t)(P & M24);
            if (!q->pow2 && !q->eTab[j][d][sL >> 17]) return 0;
            if (d == 0 && lo) {
                lo->PH[j] = (uint32_t)((P >> 24) & M24);
                lo->e0[j] = q->pow2 ? q->dmask[j][0] : q->eTab[j][0][sL >> 17];
            }
        }
    }
    if (lo) lo->cpat = cpat;
    return 1;
}

int mitmSieve(const MitmQuery *q, uint32_t yL)
{
    return lowSieve(q, yL & M24, NULL);
}

uint64_t mitmSeedOf(const MitmQuery *q, uint32_t yH, uint32_t yL)
{
    uint64_t y = ((uint64_t)(yH & M24) << 24) | (yL & M24);
    return (y - q->off[0]) & M48;
}

int mitmWouldReach(const MitmQuery *q, uint64_t seed48)
{
    uint64_t y = (seed48 + q->off[0]) & M48;
    if (!lowSieve(q, (uint32_t)(y & M24), NULL)) return 0;
    return testY(q, y);
}

// ---------------------------------------------------------------------------
// query construction
// ---------------------------------------------------------------------------

int mitmSupported(int mc, int structType)
{
    StructureConfig sc;
    if (!getStructureConfig(structType, mc, &sc)) return 0;
    if (sc.chunkRange < 2 || sc.chunkRange > 64) return 0;
    switch (structType) {
    case Desert_Pyramid: case Jungle_Pyramid: case Swamp_Hut: case Igloo:
    case Village: case Ocean_Ruin: case Shipwreck: case Ruined_Portal:
    case Ruined_Portal_N: case Trail_Ruins: case Trial_Chambers:
    case Ancient_City:                       // power-of-two range, handled
    case Outpost:                            // + a 1-in-5 rejection roll
    case Monument: case Mansion:             // two draws averaged per axis
    case End_City:                           // as above, plus the 1008 gap
        return 1;
    // Nether structures moved to the plain feature placement in 1.18; before
    // that they used getRegPos with a different draw pattern, so refuse rather
    // than invert the wrong formula.
    case Fortress: case Bastion:
        return mc >= MC_1_18;
    default:
        return 0;                            // mineshaft, treasure, stronghold,
    }                                        // decorators: different algorithms
}

// Which values may draw `d` take, given the structure must land on `ok`?
// For a single-draw axis that is `ok` itself. For an averaged pair it is the
// marginal: every value that some partner can complete into an allowed offset.
static uint64_t drawMarginal(uint64_t ok, uint32_t r, int averaged)
{
    if (!averaged) return ok;
    uint64_t m = 0;
    for (uint32_t a = 0; a < r; a++)
        for (uint32_t b = 0; b < r; b++)
            if ((ok >> ((a + b) >> 1)) & 1) { m |= 1ULL << a; break; }
    return m;
}

int mitmQueryInit(MitmQuery *q, int mc, int st, int n,
                  const int *regX, const int *regZ,
                  const uint64_t *okx, const uint64_t *okz)
{
    if (n < 1 || n > MITM_MAXN) return 0;
    if (!mitmSupported(mc, st)) return 0;
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;

    memset(q, 0, sizeof *q);
    q->mc = mc; q->st = st; q->n = n;
    q->regionSize = sc.regionSize;
    q->range = (uint32_t)sc.chunkRange;
    uint32_t r = q->range;

    q->family = (st == Monument || st == Mansion || st == End_City)
              ? MITM_LARGE : MITM_FEATURE;
    q->ndraw  = (q->family == MITM_LARGE) ? 4 : 2;
    q->roll   = (st == Outpost) ? MITM_ROLL_OUTPOST
              : (st == Bastion) ? MITM_ROLL_BASTION : MITM_ROLL_NONE;
    q->endCity = (st == End_City);

    q->pow2 = ((r & (r - 1)) == 0);
    if (q->pow2) {
        // getLargeStructureChunkInRegion has no power-of-two branch (upstream
        // marks it TODO), so a large structure with such a range would be
        // inverted against a formula the engine does not use.
        if (q->family == MITM_LARGE) return 0;
        uint32_t k = 0; while ((1u << k) < r) k++;
        if (k > 24) return 0;
        q->pow2sh = 24 - k;
    }

    uint64_t all = (r == 64) ? ~0ULL : ((1ULL << r) - 1);
    for (int j = 0; j < n; j++) {
        q->regX[j] = regX[j]; q->regZ[j] = regZ[j];
        q->off[j] = ((uint64_t)((int64_t)regX[j] * 341873128712LL
                              + (int64_t)regZ[j] * 132897987541LL
                              + (int64_t)sc.salt)) & M48;
        q->okx[j] = (okx ? okx[j] : all) & all;
        q->okz[j] = (okz ? okz[j] : all) & all;
        if (!q->okx[j] || !q->okz[j]) return 0;   // unsatisfiable as stated
    }
    for (int j = 0; j < n; j++) q->D[j] = (q->off[j] - q->off[0]) & M48;

    // 128*s1H = d (mod r) is solvable iff g | d, and then s1H is pinned mod m.
    q->g = gcd32(128, r);
    q->m = r / q->g;
    q->tinv = modinv(128 / q->g, q->m);
    q->tw24 = (uint32_t)(HALF % q->m);
    if (q->m > 1 && (uint64_t)(128 / q->g) * q->tinv % q->m != 1) return 0;
    if (q->pow2) q->m = 1;      // the constraint is a prefix, not a residue

    // multiply-shift for %r, only if the exact bound holds
    q->fmagic = 0;
    {
        uint64_t magic = (1ULL << 36) / r + 1;
        uint64_t err   = magic * r - (1ULL << 36);
        if (magic <= (1ULL << 33) && err * ((1ULL << 31) - 1) < (1ULL << 36))
            q->fmagic = magic;
    }
    for (uint32_t t = 0; t < 4096; t++) {        // cheap belt-and-braces
        uint32_t v = t * 524287u;
        if (fmodr(q, v) != v % r) { q->fmagic = 0; break; }
    }

    // Per-draw allowed sets, then the residue table each draw needs.
    int avg = (q->family == MITM_LARGE);
    for (int j = 0; j < n; j++) {
        if (avg) {
            q->dmask[j][0] = q->dmask[j][1] = drawMarginal(q->okx[j], r, 1);
            q->dmask[j][2] = q->dmask[j][3] = drawMarginal(q->okz[j], r, 1);
        } else {
            q->dmask[j][0] = q->okx[j];
            q->dmask[j][1] = q->okz[j];
        }
        if (q->pow2) continue;                   // no low-half constraint at all
        for (int d = 0; d < q->ndraw; d++)
        for (uint32_t a = 0; a < 128; a++) {
            uint64_t mask = 0;
            for (uint32_t o = 0; o < r; o++) {
                if (!((q->dmask[j][d] >> o) & 1)) continue;
                uint32_t dd = (uint32_t)(((int)o - (int)(a % r) + (int)r) % (int)r);
                if (dd % q->g) continue;
                mask |= 1ULL << (uint32_t)((uint64_t)(dd / q->g) * q->tinv % q->m);
            }
            q->eTab[j][d][a] = mask;
        }
    }
    return 1;
}

int mitmMinSpread(int mc, int st)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;
    int gap = sc.regionSize - ((int)sc.chunkRange - 1);
    if (gap < 0) gap = 0;
    int s = 1;
    while (s * s < 2 * gap * gap) s++;
    return s;
}

// Offsets on one axis that could still satisfy the cluster rule, judged on this
// axis alone -- a necessary condition for the Euclidean test, so a safe first
// cut. Getting this cut right matters: it is what keeps the exact pass below
// affordable, and a window of 2w instead of the correct per-anchor w left the
// pairing too big to run and the offsets far looser than they needed to be.
static int axisTuples(const int *Reg, int R, int r, int w, int anchor,
                      uint8_t (*out)[4], int cap)
{
    int n = 0;
    for (int a = 0; a < r; a++)
    for (int b = 0; b < r; b++)
    for (int c = 0; c < r; c++)
    for (int d = 0; d < r; d++) {
        int v[4] = { Reg[0]*R + a, Reg[1]*R + b, Reg[2]*R + c, Reg[3]*R + d };
        int keep;
        if (anchor >= 0) {
            // Member `anchor` must have all the others within w of it. Solving
            // each anchor choice as its own query keeps every one of them tight;
            // unioning the four first makes the offsets so wide that the
            // low-half sieve has nothing left to reject.
            keep = 1;
            for (int k = 0; k < 4 && keep; k++)
                if (v[k] - v[anchor] > w || v[anchor] - v[k] > w) keep = 0;
        } else {
            int mn = v[0], mx = v[0];
            for (int i = 1; i < 4; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
            keep = (mx - mn <= w);
        }
        if (!keep) continue;
        if (n >= cap) return -1;
        out[n][0] = (uint8_t)a; out[n][1] = (uint8_t)b;
        out[n][2] = (uint8_t)c; out[n][3] = (uint8_t)d;
        n++;
    }
    return n;
}

// Do four structures at these chunk coordinates form a cluster? Two rules,
// because two callers mean different things by "within T":
//   pairwise -- every pair inside T of every other (tools/quad.c's rule)
//   anchored -- some member has the other three inside T (query.c's rule, and
//               the looser of the two: opposite members can be 2T apart)
static int cornerFits(const int *vx, const int *vz, int64_t spr2, int anchor)
{
    if (anchor < 0) {
        for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
            int64_t dx = (vx[a]-vx[b]) * 16LL, dz = (vz[a]-vz[b]) * 16LL;
            if (dx*dx + dz*dz > spr2) return 0;
        }
        return 1;
    }
    for (int b = 0; b < 4; b++) {
        int64_t dx = (vx[anchor]-vx[b]) * 16LL, dz = (vz[anchor]-vz[b]) * 16LL;
        if (dx*dx + dz*dz > spr2) return 0;
    }
    return 1;
}

static int buildCorner(MitmQuery *q, int mc, int st, int64_t spr2, int anchor)
{
    StructureConfig sc;
    if (!mitmSupported(mc, st) || !getStructureConfig(st, mc, &sc)) return 0;
    int r = (int)sc.chunkRange, R = sc.regionSize;
    if (r > 64) return 0;

    // Widest chunk separation the rule allows between two members it constrains.
    // Both rules constrain at that width -- pairwise across every pair, anchored
    // from one member outward -- so the same window serves, and axisTuples knows
    // which of the two it is applying.
    int spread = 0; while ((int64_t)spread * spread * 256 < spr2) spread++;

    // End cities do not generate within 1008 blocks of the origin, so a cluster
    // anchored at region (0,0) is empty by rule rather than by seed. Anchor
    // theirs far enough out that the question is about placement again.
    int o = (st == End_City) ? 8 : 0;
    const int RX[4] = {o, o+1, o, o+1}, RZ[4] = {o, o, o+1, o+1};

    int cap = r * r * r * r;
    uint8_t (*XT)[4] = malloc((size_t)cap * 4);
    uint8_t (*ZT)[4] = malloc((size_t)cap * 4);
    if (!XT || !ZT) { free(XT); free(ZT); return 0; }
    int nx = axisTuples(RX, R, r, spread, anchor, XT, cap);
    int nz = axisTuples(RZ, R, r, spread, anchor, ZT, cap);
    if (nx <= 0 || nz <= 0) { free(XT); free(ZT); return 0; }

    // The axis window on its own is a weak relaxation: it lets the two diagonal
    // structures sit `spread` apart on x AND `spread` apart on z, which one
    // sphere never allows. Pairing the axes and applying the real Euclidean
    // test gives the offsets a structure can ACTUALLY take. At a tight spread
    // that collapses to one value per structure -- the difference between a
    // sieve that keeps one low half and one that keeps thousands.
    uint64_t okx[4] = {0,0,0,0}, okz[4] = {0,0,0,0};
    if ((double)nx * nz <= 4e8) {
        for (int i = 0; i < nx; i++) {
            int vx[4]; for (int k = 0; k < 4; k++) vx[k] = RX[k]*R + XT[i][k];
            for (int j = 0; j < nz; j++) {
                int vz[4]; for (int k = 0; k < 4; k++) vz[k] = RZ[k]*R + ZT[j][k];
                if (!cornerFits(vx, vz, spr2, anchor)) continue;
                for (int k = 0; k < 4; k++) {
                    okx[k] |= 1ULL << XT[i][k];
                    okz[k] |= 1ULL << ZT[j][k];
                }
            }
        }
    } else {                            // too many tuples to pair up: window only
        for (int i = 0; i < nx; i++) for (int k = 0; k < 4; k++) okx[k] |= 1ULL << XT[i][k];
        for (int j = 0; j < nz; j++) for (int k = 0; k < 4; k++) okz[k] |= 1ULL << ZT[j][k];
    }
    free(XT); free(ZT);

    for (int k = 0; k < 4; k++) if (!okx[k] || !okz[k]) return 0;
    return mitmQueryInit(q, mc, st, 4, RX, RZ, okx, okz);
}

int mitmQueryQuad(MitmQuery *q, int mc, int st, int spreadChunks)
{
    int64_t s = (int64_t)spreadChunks * 16;
    return buildCorner(q, mc, st, s * s, -1);
}

int mitmQueryCluster(MitmQuery *q, int mc, int st, int spreadBlocks, int anchor)
{
    if (anchor < 0 || anchor > 3) return 0;
    return buildCorner(q, mc, st, (int64_t)spreadBlocks * spreadBlocks, anchor);
}

// A 2x2 region block is the ONLY way four of these can cluster within
// `spreadBlocks` of a common member -- which is what lets the solver stand in
// for the scan. Two structures two region columns apart are at least
// (2*regionSize - (range-1)) chunks apart whatever their offsets, and under the
// anchored rule two members can be 2*spread apart, so the test is on 2*spread.
// Returns 0 when a looser arrangement could also satisfy the condition, in
// which case solving for the corner would MISS seeds and must not be used.
int mitmCornerIsOnlyShape(int mc, int st, int spreadBlocks)
{
    StructureConfig sc;
    if (!getStructureConfig(st, mc, &sc)) return 0;
    int64_t gapChunks = 2LL * sc.regionSize - ((int64_t)sc.chunkRange - 1);
    if (gapChunks <= 0) return 0;
    return 2LL * spreadBlocks < gapChunks * 16;
}

// ---------------------------------------------------------------------------
// the hash join
// ---------------------------------------------------------------------------
//
//   T[v] = ((v ^ KH)*K) mod 2^24        s1H_j = (T[yH + shift_j] + PH_j) mod 2^24
//
// Bucketing yH by (T[yH+shift_j] mod m) alone is NOT enough: the "+ PH_j" is
// then truncated mod 2^24, and 2^24 is not a multiple of m, so the residue the
// low half needs depends on whether that addition wrapped. It wraps exactly
// when T >= 2^24 - PH_j -- a threshold that moves with the low half. So the key
// also carries the octant of T, which localises the threshold to one octant:
// the others have a known carry, the straddling one takes both.
//
// The same key serves the power-of-two case, where the constraint is a prefix
// of s1H rather than a residue: the valid T values are then a union of
// contiguous intervals, which is a run of octants.

typedef struct {
    uint16_t *cls;        // class of T[v], v over 2^24 -- shared by all patterns
    uint32_t *order;      // yH values, bucketed
    uint32_t *bstart;     // NB+1 offsets into order
    uint32_t *pos;        // scratch for the scatter pass
    uint32_t  NB, C, OCT, octsh;
    uint32_t  shift[MITM_MAXN];
    uint32_t  yh0, yhN;
} MitmIndex;

static void indexFree(MitmIndex *ix)
{
    free(ix->cls); free(ix->order); free(ix->bstart); free(ix->pos);
    memset(ix, 0, sizeof *ix);
}

static int indexAlloc(MitmIndex *ix, const MitmQuery *q, uint32_t yh0, uint32_t yhN)
{
    memset(ix, 0, sizeof *ix);
    ix->yh0 = yh0; ix->yhN = yhN;

    // Widest octant split whose key space stays small enough to bucket. If even
    // no split overflows it, this query cannot be joined -- the caller sweeps.
    int bits = -1;
    for (uint32_t b = 0; b <= 8; b++) {
        uint64_t C = (uint64_t)q->m << b, NB = 1;
        int over = 0;
        for (int j = 0; j < q->n; j++) { NB *= C; if (NB > (1u << 22)) { over = 1; break; } }
        if (over || C > 4096) break;
        bits = (int)b;
    }
    if (bits < 0) return 0;
    ix->octsh = 24 - (uint32_t)bits;
    ix->OCT   = 1u << (uint32_t)bits;
    ix->C     = q->m * ix->OCT;
    ix->NB    = 1;
    for (int j = 0; j < q->n; j++) ix->NB *= ix->C;

    ix->cls    = malloc((size_t)HALF * sizeof(uint16_t));
    ix->order  = malloc((size_t)yhN * sizeof(uint32_t));
    ix->bstart = malloc(((size_t)ix->NB + 1) * sizeof(uint32_t));
    ix->pos    = malloc((size_t)ix->NB * sizeof(uint32_t));
    if (!ix->cls || !ix->order || !ix->bstart || !ix->pos) { indexFree(ix); return 0; }

    for (uint32_t v = 0; v < HALF; v++) {
        uint32_t T = (uint32_t)(((uint64_t)(v ^ KH) * K) & M24);
        ix->cls[v] = (uint16_t)((T % q->m) * ix->OCT + (T >> ix->octsh));
    }
    return 1;
}

static void indexBuild(MitmIndex *ix, const MitmQuery *q, uint32_t cpat)
{
    for (int j = 0; j < q->n; j++)
        ix->shift[j] = (uint32_t)((q->D[j] >> 24) + ((cpat >> j) & 1)) & M24;

    memset(ix->bstart, 0, ((size_t)ix->NB + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < ix->yhN; i++) {
        uint32_t yH = (ix->yh0 + i) & M24, key = 0;
        for (int j = 0; j < q->n; j++)
            key = key * ix->C + ix->cls[(yH + ix->shift[j]) & M24];
        ix->bstart[key + 1]++;
    }
    for (uint32_t k = 0; k < ix->NB; k++) ix->bstart[k + 1] += ix->bstart[k];

    uint32_t *pos = ix->pos;
    memcpy(pos, ix->bstart, (size_t)ix->NB * sizeof(uint32_t));
    for (uint32_t i = 0; i < ix->yhN; i++) {
        uint32_t yH = (ix->yh0 + i) & M24, key = 0;
        for (int j = 0; j < q->n; j++)
            key = key * ix->C + ix->cls[(yH + ix->shift[j]) & M24];
        ix->order[pos[key]++] = yH;
    }
}

// Mark every octant a contiguous run of T values touches, with wraparound.
static void markRun(uint8_t *oct, const MitmIndex *ix, uint32_t lo, uint32_t len)
{
    if (len >= HALF) { memset(oct, 1, ix->OCT); return; }
    uint32_t first = lo >> ix->octsh;
    uint32_t last  = ((lo + len - 1) & M24) >> ix->octsh;
    uint32_t o = first;
    for (;;) {
        oct[o] = 1;
        if (o == last) break;
        o = (o + 1) & (ix->OCT - 1);
    }
}

// Which classes of T can still satisfy structure j, given this low half?
static int classesFor(const MitmQuery *q, const MitmIndex *ix,
                      uint32_t PH, uint64_t e0, uint16_t *out)
{
    uint32_t nb = 0;

    if (q->pow2) {
        // s1H = (T + PH) mod 2^24 must have its top bits in `e0`, so T lies in
        // a union of contiguous intervals -- a run of octants each.
        uint8_t oct[256];
        memset(oct, 0, ix->OCT);
        uint32_t width = 1u << q->pow2sh;
        for (uint32_t o = 0; o < q->range; o++) {
            if (!((e0 >> o) & 1)) continue;
            markRun(oct, ix, ((o << q->pow2sh) - PH) & M24, width);
        }
        for (uint32_t o = 0; o < ix->OCT; o++) if (oct[o]) out[nb++] = (uint16_t)o;
        return (int)nb;
    }

    // T = e - PH + carry*2^24 (mod m), with the carry known per octant except
    // in the one the threshold falls in.
    uint32_t t = PH ? (HALF - PH) : HALF;      // T >= t  <=>  the add wrapped
    uint32_t S = 1u << ix->octsh;
    uint32_t phm = PH % q->m;
    for (uint32_t o = 0; o < ix->OCT; o++) {
        uint32_t lo = o * S, hi = lo + S;
        uint64_t res = 0;
        for (uint32_t carry = 0; carry < 2; carry++) {
            if (carry == 0 && !(lo < t)) continue;
            if (carry == 1 && !(hi > t)) continue;
            uint32_t add = carry ? q->tw24 : 0;
            for (uint32_t e = 0; e < q->m; e++) {
                if (!((e0 >> e) & 1)) continue;
                res |= 1ULL << ((e + q->m - phm + add) % q->m);
            }
        }
        for (uint32_t e = 0; e < q->m; e++)
            if ((res >> e) & 1) out[nb++] = (uint16_t)(e * ix->OCT + o);
    }
    return (int)nb;
}

// ---------------------------------------------------------------------------
// running it
// ---------------------------------------------------------------------------

typedef struct {
    const MitmQuery *q;
    MitmRun         *run;
    MitmCb           cb;
    void            *arg;
    CRITICAL_SECTION lock;
    volatile LONG    stop;
    uint64_t         hits;
    uint32_t         yh0, yhN;

    const uint32_t  *surv;          // survivors, ascending by yL
    uint64_t         nsurv;
    volatile LONG    next;          // work index
    uint64_t         lo, hi;        // survivor slice for this phase
    const MitmIndex *ix;
} Shared;

static inline void emitSeed(Shared *sh, uint64_t y)
{
    uint64_t ws = (y - sh->q->off[0]) & M48;
    if (sh->run->window && ((ws - sh->run->wsLo) & M48) >= sh->run->wsHi - sh->run->wsLo)
        return;
    EnterCriticalSection(&sh->lock);
    sh->hits++;
    if (sh->cb && sh->cb(ws, sh->arg)) sh->stop = 1;
    if (sh->run->limit && sh->hits >= sh->run->limit) sh->stop = 1;
    LeaveCriticalSection(&sh->lock);
}

// ---- phase 1: sieve the low halves -----------------------------------------

typedef struct {
    const MitmQuery *q;
    uint32_t  lo, hi;
    uint32_t *out;
    uint32_t  n;
} SieveJob;

static DWORD WINAPI sieveWorker(LPVOID arg)
{
    SieveJob *j = arg;
    uint32_t n = 0;
    for (uint32_t yL = j->lo; yL < j->hi; yL++)
        if (lowSieve(j->q, yL, NULL)) j->out[n++] = yL;
    j->n = n;
    return 0;
}

// ---- phase 2a: nested -- every survivor against every high half ------------

static DWORD WINAPI nestedWorker(LPVOID arg)
{
    Shared *sh = arg;
    const MitmQuery *q = sh->q;
    uint64_t cand = 0;
    for (;;) {
        LONG i = InterlockedIncrement(&sh->next) - 1;
        if ((uint64_t)i >= sh->nsurv || sh->stop) break;
        uint64_t yL = sh->surv[i];
        for (uint32_t k = 0; k < sh->yhN; k++) {
            uint64_t y = ((uint64_t)((sh->yh0 + k) & M24) << 24) | yL;
            if (testY(q, y)) emitSeed(sh, y);
        }
        cand += sh->yhN;
    }
    EnterCriticalSection(&sh->lock);
    sh->run->candidates += cand;
    LeaveCriticalSection(&sh->lock);
    return 0;
}

// ---- phase 2b: join -- look the high halves up instead ---------------------

typedef struct {
    Shared          *sh;
    const MitmIndex *ix;
    uint16_t        *V[MITM_MAXN];
    int              nv[MITM_MAXN];
    uint64_t         yL;
    uint64_t         cand;
} Walk;

static void walk(Walk *w, int j, uint32_t key)
{
    const MitmIndex *ix = w->ix;
    if (j == w->sh->q->n) {
        uint32_t a = ix->bstart[key], b = ix->bstart[key + 1];
        w->cand += b - a;
        for (uint32_t i = a; i < b; i++) {
            uint64_t y = ((uint64_t)ix->order[i] << 24) | w->yL;
            if (testY(w->sh->q, y)) emitSeed(w->sh, y);
        }
        return;
    }
    for (int t = 0; t < w->nv[j]; t++)
        walk(w, j + 1, key * ix->C + w->V[j][t]);
}

static DWORD WINAPI joinWorker(LPVOID arg)
{
    Shared *sh = arg;
    const MitmQuery *q = sh->q;
    const MitmIndex *ix = sh->ix;
    Walk w; memset(&w, 0, sizeof w);
    w.sh = sh; w.ix = ix;
    static const int VMAX = 4096;                   // >= ix->C, bounded at build
    uint16_t *buf = malloc((size_t)MITM_MAXN * VMAX * sizeof(uint16_t));
    if (!buf) return 0;
    for (int j = 0; j < q->n; j++) w.V[j] = buf + (size_t)j * VMAX;

    for (;;) {
        LONG i = InterlockedIncrement(&sh->next) - 1;
        uint64_t idx = sh->lo + (uint64_t)i;
        if (idx >= sh->hi || sh->stop) break;

        uint32_t yL = sh->surv[idx];

        // Decide BEFORE building the class sets. classesFor costs O(m * octants)
        // per structure per survivor -- 3840 iterations each for a mansion --
        // and the guard below only ran after paying that. On a windowed run with
        // millions of surviving low halves and a handful of high halves, the
        // lookup machinery cost eleven minutes to avoid sweeping two values.
        // A sweep of yhN can never lose to a walk that costs n*C just to set up.
        if (ix->yhN <= (uint32_t)q->n * ix->C) {
            for (uint32_t k = 0; k < ix->yhN; k++) {
                uint64_t y = ((uint64_t)((ix->yh0 + k) & M24) << 24) | yL;
                if (testY(q, y)) emitSeed(sh, y);
            }
            w.cand += ix->yhN;
            continue;
        }

        MitmLow lo;
        if (!lowSieve(q, yL, &lo)) continue;    // cannot happen; cheap to be sure
        w.yL = yL;

        uint64_t prod = 1;
        for (int j = 0; j < q->n; j++) {
            w.nv[j] = classesFor(q, ix, lo.PH[j], lo.e0[j], w.V[j]);
            prod *= (uint64_t)w.nv[j];
        }
        // If the lookup would touch more buckets than there are high halves,
        // the sweep is cheaper -- and identical in what it returns.
        if (prod == 0) continue;
        if (prod >= ix->yhN || prod > (1u << 20)) {
            for (uint32_t k = 0; k < ix->yhN; k++) {
                uint64_t y = ((uint64_t)((ix->yh0 + k) & M24) << 24) | yL;
                if (testY(q, y)) emitSeed(sh, y);
            }
            w.cand += ix->yhN;
        } else {
            walk(&w, 0, 0);
        }
    }
    EnterCriticalSection(&sh->lock);
    sh->run->candidates += w.cand;
    LeaveCriticalSection(&sh->lock);
    free(buf);
    return 0;
}

// ---------------------------------------------------------------------------

static int nthreads(int want)
{
    if (want > 0) return want > 64 ? 64 : want;
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n < 1 ? 1 : (n > 64 ? 64 : n);
}

static void runThreads(LPTHREAD_START_ROUTINE fn, void *arg, int nt)
{
    HANDLE h[64];
    for (int i = 0; i < nt; i++) h[i] = CreateThread(NULL, 0, fn, arg, 0, NULL);
    WaitForMultipleObjects(nt, h, TRUE, INFINITE);
    for (int i = 0; i < nt; i++) CloseHandle(h[i]);
}

uint64_t mitmRun(const MitmQuery *q, MitmRun *run, MitmCb cb, void *arg)
{
    int nt = nthreads(run->threads);
    run->survivors = run->candidates = run->hits = 0;
    run->patterns = 0;

    // The high halves this run is allowed to visit.
    uint32_t yh0 = 0, yhN = HALF;
    if (run->window) {
        if (run->wsHi <= run->wsLo) return 0;
        uint64_t ylo = (run->wsLo + q->off[0]) & M48;
        uint64_t len = run->wsHi - run->wsLo;
        uint64_t cnt = (((ylo & M24) + len - 1) >> 24) + 1;
        if (cnt > HALF) cnt = HALF;
        yh0 = (uint32_t)(ylo >> 24);
        yhN = (uint32_t)cnt;
    }

    // ---- phase 1 -----------------------------------------------------------
    ULONGLONG t0 = GetTickCount64();
    SieveJob *sj = calloc(nt, sizeof *sj);
    for (int i = 0; i < nt; i++) {
        sj[i].q  = q;
        sj[i].lo = (uint32_t)((uint64_t)HALF * i / nt);
        sj[i].hi = (uint32_t)((uint64_t)HALF * (i + 1) / nt);
        sj[i].out = malloc((size_t)(sj[i].hi - sj[i].lo) * sizeof(uint32_t));
        if (!sj[i].out) { for (int k = 0; k < i; k++) free(sj[k].out); free(sj); return 0; }
    }
    {
        HANDLE h[64];
        for (int i = 0; i < nt; i++) h[i] = CreateThread(NULL, 0, sieveWorker, &sj[i], 0, NULL);
        WaitForMultipleObjects(nt, h, TRUE, INFINITE);
        for (int i = 0; i < nt; i++) CloseHandle(h[i]);
    }
    uint64_t ns = 0;
    for (int i = 0; i < nt; i++) ns += sj[i].n;
    uint32_t *surv = malloc((size_t)(ns ? ns : 1) * sizeof(uint32_t));
    uint64_t p = 0;
    for (int i = 0; i < nt; i++) {           // blocks are ascending, so surv is sorted
        memcpy(surv + p, sj[i].out, (size_t)sj[i].n * sizeof(uint32_t));
        p += sj[i].n;
        free(sj[i].out);
    }
    free(sj);
    run->survivors = ns;
    run->lowSec = (GetTickCount64() - t0) / 1000.0;

    if (!ns) { free(surv); return 0; }

    // ---- phase 2 -----------------------------------------------------------
    t0 = GetTickCount64();
    Shared sh; memset(&sh, 0, sizeof sh);
    InitializeCriticalSection(&sh.lock);
    sh.q = q; sh.run = run; sh.cb = cb; sh.arg = arg;
    sh.surv = surv; sh.nsurv = ns; sh.yh0 = yh0; sh.yhN = yhN;

    MitmIndex ix;
    int joined = run->join && indexAlloc(&ix, q, yh0, yhN);
    if (joined) {
        // c_j flips 0 -> 1 once as yL rises, so the carry pattern is constant on
        // runs of survivors. Build the index once per run, not once per survivor.
        uint64_t i = 0;
        while (i < ns && !sh.stop) {
            MitmLow lo;
            lowSieve(q, surv[i], &lo);
            uint32_t pat = lo.cpat;
            uint64_t j = i + 1;
            while (j < ns) {
                MitmLow l2; lowSieve(q, surv[j], &l2);
                if (l2.cpat != pat) break;
                j++;
            }
            indexBuild(&ix, q, pat);
            sh.ix = &ix; sh.lo = i; sh.hi = j; sh.next = 0;
            runThreads(joinWorker, &sh, nt);
            run->patterns++;
            i = j;
        }
        indexFree(&ix);
    }
    if (!joined) {                       // no index: sweep, same answers
        sh.hits = 0; sh.stop = 0; sh.next = 0;
        run->candidates = 0; run->patterns = 1;
        runThreads(nestedWorker, &sh, nt);
    }

    run->sweepSec = (GetTickCount64() - t0) / 1000.0;
    run->hits = sh.hits;
    DeleteCriticalSection(&sh.lock);
    free(surv);
    return sh.hits;
}
