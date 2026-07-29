#include "climate.h"
#include "generator.h"
#include "finders.h"
#include <stdlib.h>
#include <string.h>

// --- planning -------------------------------------------------------------

int climatePlan(const Query *q, ClimatePlan *cp)
{
    memset(cp, 0, sizeof(*cp));
    if (q->mc <= MC_1_17) return 0;   // no climate space to filter on
    // An off switch, so "same seeds, with and without" is a command away and
    // the no-loss claim stays checkable after every future change here.
    if (getenv("SC_NO_CLIMATE")) return 0;

    const int *ext = getBiomeParaExtremes(q->mc);

    for (int k = 0; k < q->n; k++) {
        const Cond *c = &q->cond[k];
        if (c->type != CT_BIOME && c->type != CT_BIOME_AREA) continue;
        if (c->dim != DIM_OVERWORLD) continue;   // limits are overworld-only

        const int *lim = getBiomeParaLimits(q->mc, c->biomeId);
        if (!lim) continue;                      // unknown biome, no claim made

        int64_t lo = (int64_t)lim[0] - CLIMATE_MARGIN;   // temperature is the
        int64_t hi = (int64_t)lim[1] + CLIMATE_MARGIN;   // first of six pairs

        // Not every biome is defined by temperature. Mushroom fields are carved
        // out of continentalness and occur at any temperature at all; for those
        // the window spans the whole achievable range, the gate can never
        // reject, and sampling would be pure overhead on every point. Leave
        // them out rather than pay to learn nothing.
        if (ext && lo <= ext[0] && hi >= ext[1]) continue;

        cp->use[k] = 1;
        cp->lo[k]  = lo;
        cp->hi[k]  = hi;
        cp->n++;
    }
    return cp->n;
}

// --- the noise ------------------------------------------------------------

// Temperature is not sampled where you ask for it: the biome sampler first
// displaces the coordinate by a separate "shift" noise. Reproducing that is
// what makes this gate exact instead of merely close, so both noises get seeded
// and the displacement is applied the way sampleBiomeNoise applies it.
static _Thread_local BiomeNoise tl_shift, tl_temp;
static _Thread_local uint64_t tl_seed;
static _Thread_local int tl_large = -1, tl_mc = -1, tl_valid = 0;

static void climateSeed(int mc, uint64_t worldSeed, int large)
{
    if (tl_valid && tl_seed == worldSeed && tl_large == large && tl_mc == mc)
        return;
    tl_shift.mc = tl_temp.mc = mc;

    // The shift cannot go through setClimateParaSeed: NP_SHIFT and NP_DEPTH are
    // the same enum value, so asking for one seeds the other's three noises and
    // leaves the shift zeroed. That failure is quiet -- an unshifted sample is
    // only a few units off, close enough to look right -- so this seeds it
    // directly, the way init_climate_seed does.
    Xoroshiro pxr;
    xSetSeed(&pxr, worldSeed);
    uint64_t xlo = xNextLong(&pxr);
    uint64_t xhi = xNextLong(&pxr);
    static const double shift_amp[] = {1, 1, 1, 0};   // md5 "minecraft:offset"
    pxr.lo = xlo ^ 0x080518cf6af25384;
    pxr.hi = xhi ^ 0x3f3dfb40a54febd5;
    xDoublePerlinInit(&tl_shift.climate[NP_SHIFT], &pxr, tl_shift.oct,
                      shift_amp, -3, 4, -1);

    setClimateParaSeed(&tl_temp, worldSeed, large, NP_TEMPERATURE, -1);
    tl_seed = worldSeed; tl_large = large; tl_mc = mc; tl_valid = 1;
}

// The literal expression from sampleBiomeNoise, float width and all. Anything
// looser than a bit-for-bit match would make the comparison below unsafe.
static int64_t tempAt(int qx, int qz)
{
    double px = qx + sampleDoublePerlin(&tl_shift.climate[NP_SHIFT], qx, 0, qz) * 4.0;
    double pz = qz + sampleDoublePerlin(&tl_shift.climate[NP_SHIFT], qz, qx, 0) * 4.0;
    float t = (float) sampleDoublePerlin(&tl_temp.climate[NP_TEMPERATURE], px, 0, pz);
    return (int64_t)(10000.0F * t);
}

int64_t climateTempAt(int mc, uint64_t worldSeed, int large, int qx, int qz)
{
    climateSeed(mc, worldSeed, large);
    return tempAt(qx, qz);
}

// --- the gate -------------------------------------------------------------

static _Thread_local uint64_t tl_gated, tl_skipped;

uint64_t climateGated(void)   { return tl_gated; }
uint64_t climateSkipped(void) { return tl_skipped; }

int climateMayHold(const Query *q, uint64_t worldSeed, int k, int qx, int qz)
{
    if (!q->clim.use[k]) return 1;
    climateSeed(q->mc, worldSeed, q->largeBiomes ? 1 : 0);
    tl_gated++;
    int64_t t = tempAt(qx, qz);
    if (t >= q->clim.lo[k] && t <= q->clim.hi[k]) return 1;
    tl_skipped++;
    return 0;
}
