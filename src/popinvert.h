#ifndef SC_POPINVERT_H
#define SC_POPINVERT_H

#include <stdint.h>

// The population seed, run backwards (milestone 2 of docs/reversal-roadmap.md).
//
// Before 1.18 every chunk's decoration RNG -- dungeons, ores, trees, chest
// placement -- is seeded from
//
//     r = new Random(worldSeed);  a = r.nextLong() | 1;  b = r.nextLong() | 1;
//     pop = (x * a + z * b) ^ worldSeed
//
// (`/ 2 * 2 + 1` instead of `| 1` up to 1.12). A target read off one chunk's
// decoration therefore pins `pop`, and this lists the 48-bit structure seeds
// that give it -- typically one or two, each standing for 65536 world seeds.
//
// The trick is that it can be solved from the bottom bit up. The low k bits of
// a and b come from bits 16..16+k-1 of the RNG state, and an LCG's low bits
// depend only on its low bits, so they are fixed by the low k+16 bits of the
// seed. With x and z sharing t trailing zero bits, the low k+t bits of pop are
// then known too. So guess the seed one bit at a time, from bit 0, and test
// every bit of pop that the guess already determines: a wrong branch dies
// within a level or two and the search stays near 2^(16-t) live branches
// instead of 2^48 (the same lifting TwoChunkCarverReversal uses on carvers).
//
// From 1.18 the population seed comes from Xoroshiro over all 64 bits and does
// not reverse this way.

// Every 48-bit structure seed whose population seed at (x, z) has these low 48
// bits. x and z are what the game passes: CHUNK coordinates up to 1.12, BLOCK
// coordinates (chunk * 16) from 1.13 -- cubiomes' getPopulationSeed takes the
// same. Writes up to `max` seeds to `out`, ascending, and returns how many
// there are (which can exceed max); -1 for 1.18+.
int popInvert(int mc, uint64_t pop, int x, int z, uint64_t *out, int max);

// The forward direction, from src/lcg.h alone (no cubiomes), so a check can
// compare it with cubiomes' getPopulationSeed and the two are independent.
uint64_t popForward(int mc, uint64_t worldSeed, int x, int z);

#endif
