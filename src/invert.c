#include "invert.h"
#include <string.h>

#define K   0x5deece66dULL
#define B   0xbULL
#define M   ((1ULL << 48) - 1)

// K^-1 mod 2^48, computed rather than quoted: Newton's iteration doubles the
// number of correct bits each round, and 6 rounds covers 64.
static uint64_t kinv(void)
{
    uint64_t x = 1;
    for (int i = 0; i < 6; i++) x *= 2 - K * x;
    return x & M;
}

uint64_t lcgForward(uint64_t s) { return (s * K + B) & M; }
uint64_t lcgBack(uint64_t s)    { return ((s - B) * kinv()) & M; }

int invertSupported(int mc, int structType)
{
    StructureConfig sc;
    if (!getStructureConfig(structType, mc, &sc)) return 0;
    // Only the single-draw-per-axis "feature" placement is inverted here.
    // Everything else takes a different number of draws or adds a rejection
    // roll, and inverting the wrong formula yields seeds that look right and
    // are not.
    switch (structType) {
    case Desert_Pyramid: case Jungle_Pyramid: case Swamp_Hut: case Igloo:
    case Village: case Ocean_Ruin: case Shipwreck: case Ruined_Portal:
    case Ruined_Portal_N: case Ancient_City: case Trail_Ruins:
    case Trial_Chambers:
        break;
    default:
        return 0;
    }
    // The power-of-two path scales instead of taking a remainder, so the
    // residue-class trick below does not apply as written.
    uint64_t r = sc.chunkRange;
    return (r & (r - 1)) != 0;
}

uint64_t invertCount(int mc, int structType, int regX, int regZ,
                     int ox, int oz, uint64_t limit, InvertCb cb, void *arg)
{
    StructureConfig sc;
    if (!invertSupported(mc, structType)) return 0;
    if (!getStructureConfig(structType, mc, &sc)) return 0;
    uint64_t r = sc.chunkRange;
    if (ox < 0 || oz < 0 || (uint64_t)ox >= r || (uint64_t)oz >= r) return 0;

    // Undo the region and salt offsets once; the rest of the loop works in
    // s1-space and converts back only when a solution is found.
    uint64_t off = (uint64_t)regX * 341873128712ULL
                 + (uint64_t)regZ * 132897987541ULL
                 + (uint64_t)sc.salt;

    uint64_t found = 0;
    // ox pins (s1 >> 17) to a residue class mod r. Walk that class rather than
    // walking every seed and testing -- this is the whole point.
    for (uint64_t u = (uint64_t)ox; u < (1ULL << 31); u += r) {
        uint64_t base = u << 17;
        for (uint64_t low = 0; low < (1ULL << 17); low++) {
            uint64_t s1 = base | low;
            // z costs one LCG step to test and needs no search of its own.
            uint64_t s2 = lcgForward(s1);
            if ((uint64_t)((uint32_t)(s2 >> 17)) % r != (uint64_t)oz) continue;

            // s1 = ((ws + off) ^ K) * K + b   ->   ws = (back(s1) ^ K) - off
            uint64_t ws = ((lcgBack(s1) ^ K) - off) & M;
            found++;
            if (cb && cb(ws, arg)) return found;
            if (limit && found >= limit) return found;
        }
    }
    return found;
}
