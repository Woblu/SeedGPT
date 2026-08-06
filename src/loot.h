// Loot search: does a structure instance's chests hold >= N of an item?
//
// getStructurePieces (and getStrongholdLoot for strongholds) fills each chest's
// loot table + loot seed. Rolling a chest is ~5 us -- cheap next to the ~42 us
// structure viability check -- so a loot condition costs about the same as a
// plain structure search. What makes a specific item RARE is the loot table,
// not our speed: a diamond in a desert pyramid is ~1 in a few hundred pyramids.
#pragma once

#include "finders.h"
#include "generator.h"

// Loot tables are init-once objects; each worker thread keeps its own cache so
// generate_loot never races. Opaque here; created/freed via the calls below.
// (query.h forward-declares the same typedef; identical typedefs are fine in C11.)
#ifndef LOOTCACHE_TYPEDEF
#define LOOTCACHE_TYPEDEF
typedef struct LootCache LootCache;
#endif

LootCache *lootCacheNew(void);
void       lootCacheFree(LootCache *lc);

// Count how many of `item` (e.g. "minecraft:diamond") the given structure
// instance yields across all its chests, on this seed/version. Returns -1 if
// the structure type has no loot support. `lc` must be this thread's cache.
int lootCountItem(LootCache *lc, int mc, uint64_t seed, int structType,
                  int blockX, int blockZ, StructureVariant *sv, const char *item);

// Is `name` a structure this module can search loot for?
int lootStructureSupported(int structType);

// True when only some of the structure's chests are modelled, so the count is a
// floor: every hit is real, but a miss is not proof of absence. Bastions are the
// one such case -- the engine simulates their guaranteed starting piece only.
int lootCountIsLowerBound(int structType);
