#include "query.h"
#include "util.h"
#include "cJSON.h"
#include "features/stronghold.h"
#include "terrain.h"
#include "loot.h"
#include "ore.h"
#include "climate.h"
#include "cactus.h"
#include "biomeat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

// ---------------------------------------------------------------- cost model
//
// Numbers are measured on this machine (i7-10700K, clang -O3), not guessed --
// and re-measured at MC 1.21, where two of them had drifted badly:
//   getStructurePos                   ~9 ns
//   isViableStructurePos             ~5.1 us   (was 42 us in the model: 8x off.
//                                     For most structures this is ONE biome
//                                     sample, not the survey the old number
//                                     implied.)
//   getBiomeAt (seed already applied) ~5.5 us   (was 2.3 us: 2.4x off. Measured
//                                     the way a scan actually uses it, on a
//                                     clustered lattice rather than scattered
//                                     points -- 1961 samples over a radius-400
//                                     disc cost 10.7 ms, which is what a single
//                                     biome condition really costs per seed.)
// applySeed (~25-42 us) is paid once per world seed per dimension, not once per
// condition, so it is not attributed to any single condition here -- but it is
// the single largest item in a search: 84% of the cost of testing one
// upper-bit variant, which the search does ~50 times per structure seed.
#define NS_STRUCT_POS    9.0
#define NS_VIABLE      5100.0
#define NS_BIOME_AT    5460.0
#define NS_SPAWN    2300000.0   // getSpawn: ~437/s
#define NS_EYES    10250000.0   // locate stronghold + pieces + loot: ~98/s
// Walking the stronghold rings for an overlap, MEASURED rather than assumed:
// 524288 world seeds in 69.9s on 8 threads with within=2000 is 1.07 ms of
// wall-clock per world seed per thread. The first guess here was NS_EYES, which
// overstated it by 10x -- eyes also rolls pieces and loot, and this does not.
#define NS_SH_RINGS 1070000.0
// Water fills to y=62 (sea level 63 means the top water block is 62), so a
// surface below this is under the ocean.
#define SEA_LEVEL_Y 62
#define NS_LOOT       47000.0   // structure viability + a ~5us loot roll
#define NS_ORE_CHUNK   4000.0   // per chunk: one biome probe + config scan + gen
#define NS_SLIME_CHUNK    6.0   // per chunk: one Java-RNG isSlimeChunk call
#define NS_CACTUS_CHUNK 90000.0  // 1 chunk in 6 rolls through; those that do
                                 // need ~50 terrain columns for the tries and
                                 // their neighbours
#define NS_TERRAIN_CELL 200000.0 // exact terrain: one heavy noise column per 4x4
#define SEA_LEVEL          63    // overworld sea level: at/above = dry land
// How far below the surface a "cave under this structure" may be. Deep enough
// to catch the big 1.18 cheese caverns, shallow enough that an ordinary
// mineshaft-depth void two hundred blocks down does not count as "underneath".
#define CAVE_SCAN_DEPTH    80

// Real block-level terrain relief under a footprint, using cubiomes' actual
// 1.18+ terrain (generateColumn / generateRegion) -- not the smoothed 1:4
// approximation. This is what tells a genuinely dramatic placement (a structure
// right on a cliff edge, a village clinging to a mountainside) apart from one
// merely standing on high ground. Returns 1 with peak/relief written, 0 if
// unavailable. Heights come back in WORLD Y (see terrain.h: generateRegion
// itself reports column indices, 65 higher).
static int exactFootprint(int mc, uint32_t gflags, uint64_t ws, Pos centre, int within,
                          int *peakOut, int *reliefOut)
{
    TerrainNoise *tn = terrainFor(mc, ws, gflags);
    if (!tn) return 0;
    // Chunk region covering the footprint square [centre +- within].
    int cxMin = (centre.x - within) >> 4, cxMax = (centre.x + within) >> 4;
    int czMin = (centre.z - within) >> 4, czMax = (centre.z + within) >> 4;
    int chunkW = cxMax - cxMin + 1, chunkH = czMax - czMin + 1;
    int blockH = chunkH << 4;
    int *ys = malloc((size_t)(chunkW << 4) * (chunkH << 4) * sizeof(int));
    if (!ys) return 0;
    generateRegion(tn, cxMin, czMin, chunkW, chunkH, NULL, ys, /*flag=*/1);
    int bx0 = cxMin << 4, bz0 = czMin << 4;
    int64_t lim = (int64_t)within * within;
    int peak = -100000, valley = 100000, found = 0;
    for (int dx = -within; dx <= within; dx++)
    for (int dz = -within; dz <= within; dz++) {
        if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
        int rx = (centre.x + dx) - bx0, rz = (centre.z + dz) - bz0;
        int y = ys[rx * blockH + rz] - TERRAIN_Y_BIAS;   // column index -> world Y
        found = 1;
        if (y > peak)   peak = y;
        if (y < valley) valley = y;
    }
    free(ys);
    if (!found) return 0;
    *peakOut = peak; *reliefOut = peak - valley;
    return 1;
}

static int isPopulationFeature(int stype);   // defined with the vocabulary
static int placementAllowed(int mc, uint64_t s48, int stype, Pos p);

// Minecraft refuses to place a pillager outpost within this many chunks of a
// village placement site (pillager_outposts.json: exclusion_zone). Declared
// here because both the feasibility check and the placement rule need it.
#define OUTPOST_VILLAGE_CHUNKS 10
static int hasPlacementRule(int mc, int stype);   // 1.18+ surface rules, below

static double geomCost(const Query *q, int i)
{
    const Cond *c = &q->cond[i];
    StructureConfig sc;
    // A stronghold has no region config. Price the pair off the PARTNER's grid,
    // which is the only part pass 1 can actually filter on. Without this the
    // lookup fails and the condition is costed at a single structure probe --
    // the cheapest thing in the query -- so the scheduler would run the most
    // expensive condition we have first.
    if (c->type == CT_OVERLAP && c->structType == Stronghold) {
        if (!getStructureConfig(c->structType2, q->mc, &sc)) return NS_STRUCT_POS;
    } else if (c->type == CT_OVERLAP && c->structType2 == Stronghold) {
        if (!getStructureConfig(c->structType, q->mc, &sc)) return NS_STRUCT_POS;
    } else if (!getStructureConfig(c->structType, q->mc, &sc)) return NS_STRUCT_POS;
    // regions we must scan to cover a disc of radius `within`
    double span = sc.regionSize * 16.0;
    double reach = c->within + (c->parent == PARENT_SPAWN ? SPAWN_MARGIN : 0);
    double n = 2.0 * (reach / span) + 1.0;
    // An overlap pair also probes the second structure's regions around each
    // candidate of the first -- a 3x3 region window is enough, since a
    // footprint collision is a few dozen blocks at most.
    if (c->type == CT_OVERLAP) return n * n * 10.0 * NS_STRUCT_POS;
    return n * n * NS_STRUCT_POS;
}

static double viabCost(const Query *q, int i)
{
    const Cond *c = &q->cond[i];
    if (c->type == CT_EYES) return NS_EYES;
    if (c->type == CT_LOOT) return NS_LOOT;
    if (c->type == CT_CACTUS) {
        // Every chunk in the box, plus a ring, has to roll its patch.
        double n = 2.0 * (c->within / 16.0) + 3.0;
        return n * n * NS_CACTUS_CHUNK;
    }
    if (c->type == CT_ORE) {
        // A per-chunk cost over the disc's bounding square -- the most expensive
        // condition, so it sorts last and runs only for earlier survivors.
        double n = 2.0 * (c->within / 16.0) + 1.0;
        double cost = n * n * NS_ORE_CHUNK;
        // Exposure generates a real terrain column set for every chunk that
        // holds any of this ore: 16 heavy cells per chunk, dwarfing the ore
        // generation itself. Assume half the chunks in range contain some.
        if (c->oreExposed) cost += 0.5 * n * n * 16.0 * NS_TERRAIN_CELL;
        return cost + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_SLIME) {
        double n = 2.0 * (c->within / 16.0) + 1.0;   // one RNG call per chunk
        return n * n * NS_SLIME_CHUNK + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_STRUCTURE) {
        double cost = NS_VIABLE;
        // 1.18+ surface rule: four terrain columns, each needing the four noise
        // columns around its cell -- 16 at worst, fewer when they share cells.
        // Sorts this structure behind everything that needs only biomes.
        if (hasPlacementRule(q->mc, c->structType))
            cost += 16.0 * NS_TERRAIN_CELL;
        // A cave check generates real block columns for five points across the
        // footprint: by far the most expensive thing a structure condition can
        // ask for, so the planner must see it.
        if (c->caveBelow > 0) cost += 5.0 * 16.0 * NS_TERRAIN_CELL;
        // centre column plus 8 ring columns, each its own chunk gen --
        // nearly twice cave_below, so the planner must order it later.
        if (c->floatVoid > 0) cost += 9.0 * 16.0 * NS_TERRAIN_CELL;
        // A population-seeded feature is also LOCATED here, chunk by chunk,
        // because pass 1 could not place it.
        if (isPopulationFeature(c->structType)) {
            StructureConfig sc;
            if (getStructureConfig(c->structType, q->mc, &sc)) {
                double n = 2.0 * (c->within / (sc.regionSize * 16.0)) + 1.0;
                cost += n * n * NS_STRUCT_POS;
            }
        }
        return cost + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_STRONGHOLD)
        return NS_SH_RINGS + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    if (c->type == CT_OVERLAP) {
        // Walking the stronghold rings costs a biome-heavy nextStronghold per
        // ring, the same order as resolving an end portal -- roughly four
        // orders of magnitude above a structure position lookup. Charging it
        // honestly is what keeps this condition scheduled last.
        if (c->structType == Stronghold || c->structType2 == Stronghold)
            return NS_SH_RINGS + 2.0 * NS_VIABLE +
                   (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
        return 2.0 * NS_VIABLE + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_HEIGHT) {
        if (c->exactTerrain) {
            // Real per-block terrain: ~one heavy noise column per 4x4 cell over
            // the footprint region. Enormous vs everything else, so it sorts
            // dead last and runs only for survivors of every cheaper filter.
            double cells = 2.0 * (c->within / 4.0) + 3.0;
            return cells * cells * NS_TERRAIN_CELL + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
        }
        // A mapApproxHeight sample per lattice point, like a biome scan.
        double step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
        double n = 2.0 * (c->within / step) + 1.0;
        return n * n * NS_BIOME_AT + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_ISLAND) {
        // ocean-fraction sample over the disc, like a biome-area scan, plus the
        // world-spawn resolve it almost always needs.
        double step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
        double n = 2.0 * (c->within / step) + 1.0;
        return n * n * NS_BIOME_AT + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    // biome scan: samples on a `scanStep` lattice across the disc
    double step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
    double n = 2.0 * (c->within / step) + 1.0;
    return n * n * NS_BIOME_AT;
}

const char *dimName(int dim)
{
    return dim == DIM_NETHER ? "nether" : dim == DIM_END ? "end" : "overworld";
}

uint32_t queryGenFlags(const Query *q)
{
    return q->largeBiomes ? LARGE_BIOMES : 0;
}

// ------------------------------------------------------------------- parsing

static int str2biome_(int mc, const char *s)
{
    for (int id = 0; id < 256; id++) {
        const char *n = biome2str(mc, id);
        if (n && !strcmp(n, s)) return id;
    }
    return -1;
}

// Biome FAMILIES, because a player asking for "badlands" means the badlands --
// all three of them -- and making them write out badlands, wooded_badlands and
// eroded_badlands as separate conditions is both tedious and a trap: three OR-ed
// biomes is not something the condition language expresses, so people write one
// and silently search a third of what they meant.
//
// Members are listed by NAME and resolved per version, so a family works on any
// version and simply contains fewer biomes on older ones.
static const struct { const char *name; const char *members[14]; } BIOME_CAT[] = {
  {"badlands",   {"badlands","wooded_badlands","eroded_badlands",
                  "badlands_plateau","modified_badlands_plateau",
                  "wooded_badlands_plateau",NULL}},
  {"mesa",       {"badlands","wooded_badlands","eroded_badlands",
                  "badlands_plateau","modified_badlands_plateau",
                  "wooded_badlands_plateau",NULL}},
  {"jungle",     {"jungle","sparse_jungle","bamboo_jungle","jungle_edge",
                  "modified_jungle",NULL}},
  {"taiga",      {"taiga","snowy_taiga","old_growth_pine_taiga",
                  "old_growth_spruce_taiga","taiga_hills","giant_tree_taiga",NULL}},
  {"forest",     {"forest","birch_forest","dark_forest","flower_forest",
                  "old_growth_birch_forest","birch_forest_hills",NULL}},
  {"savanna",    {"savanna","savanna_plateau","windswept_savanna",
                  "shattered_savanna",NULL}},
  {"swamp",      {"swamp","mangrove_swamp",NULL}},
  {"ocean",      {"ocean","deep_ocean","cold_ocean","deep_cold_ocean",
                  "lukewarm_ocean","deep_lukewarm_ocean","warm_ocean",
                  "deep_warm_ocean","frozen_ocean","deep_frozen_ocean",NULL}},
  {"snowy",      {"snowy_plains","snowy_taiga","snowy_beach","snowy_slopes",
                  "ice_spikes","frozen_peaks","frozen_river","snowy_tundra",NULL}},
  {"mountain",   {"jagged_peaks","frozen_peaks","stony_peaks","snowy_slopes",
                  "meadow","grove","windswept_hills","windswept_gravelly_hills",
                  "windswept_forest",NULL}},
  {"mushroom",   {"mushroom_fields","mushroom_field_shore",NULL}},
};

// Fills c->biomeSet from a family name. Returns the number of members that
// exist in THIS version, or -1 if the name is not a family at all.
static int biomeCategory_(int mc, const char *s, unsigned char *set, int *rep)
{
    for (size_t i = 0; i < sizeof BIOME_CAT / sizeof *BIOME_CAT; i++) {
        if (strcmp(BIOME_CAT[i].name, s)) continue;
        int n = 0;
        for (int j = 0; BIOME_CAT[i].members[j]; j++) {
            int id = str2biome_(mc, BIOME_CAT[i].members[j]);
            if (id < 0) continue;      // absent in this version; not an error
            set[id] = 1;
            if (!n) *rep = id;
            n++;
        }
        return n;                      // 0 means the family is empty HERE
    }
    return -1;
}

// The structure vocabulary. Exposed via queryStructureName/queryStructureAt so
// the NL front end can read the authoritative list rather than hardcoding one.
static const struct { const char *n; int t; } STRUCT_TBL[] = {
    // overworld
    {"mansion", Mansion}, {"village", Village}, {"monument", Monument},
    {"desert_pyramid", Desert_Pyramid}, {"jungle_temple", Jungle_Pyramid},
    {"swamp_hut", Swamp_Hut}, {"igloo", Igloo}, {"shipwreck", Shipwreck},
    {"outpost", Outpost}, {"ancient_city", Ancient_City},
    {"ruined_portal", Ruined_Portal}, {"trail_ruins", Trail_Ruins},
    {"trial_chambers", Trial_Chambers}, {"treasure", Treasure},
    {"buried_treasure", Treasure},
    {"ocean_ruin", Ocean_Ruin},
    {"mineshaft", Mineshaft}, {"geode", Geode}, {"amethyst_geode", Geode},
    // nether -- fortress and bastion share salt+geometry and are mutually
    // exclusive at a given site (nextInt(5) < 2 picks which)
    {"fortress", Fortress}, {"bastion", Bastion},
    {"ruined_portal_nether", Ruined_Portal_N},
    // end
    {"end_city", End_City},
};
#define NSTRUCT (int)(sizeof(STRUCT_TBL)/sizeof(STRUCT_TBL[0]))

// Structures the engine cannot actually verify. cubiomes' isViableFeatureBiome
// returns unconditionally for these (`return mc >= MC_1_16_1;` for overworld
// ruined portals; `return 1;` for the nether variant), so EVERY generation
// attempt is reported as a hit and false positives cannot be filtered.
// Measured: ruined_portal passes 100.0% of attempts, versus 0.9-31.7% for
// every biome-checked structure (tools/confidence.c).
int queryStructureVerified(int i)
{
    const char *n = queryStructureName(i);
    if (!n) return 1;
    return !(!strcmp(n, "ruined_portal") || !strcmp(n, "ruined_portal_nether"));
}

int queryStructureCount(void) { return NSTRUCT; }
const char *queryStructureName(int i) { return (i >= 0 && i < NSTRUCT) ? STRUCT_TBL[i].n : NULL; }
int queryStructureType(int i) { return (i >= 0 && i < NSTRUCT) ? STRUCT_TBL[i].t : -1; }

static int str2struct_(const char *s)
{
    for (int i = 0; i < NSTRUCT; i++)
        if (!strcmp(STRUCT_TBL[i].n, s)) return STRUCT_TBL[i].t;
    return -1;
}

// "stronghold" is deliberately NOT in STRUCT_TBL. Every other consumer of that
// table -- the cluster search, the loot scan, the UI's structure list -- goes
// through getStructurePos, and a stronghold has no region position to return:
// it is placed by walking rings with the biome generator. Listing it there
// would offer the user searches that silently cannot work.
//
// Overlap is the one place it does make sense, so it is accepted only here.
static int str2overlapStruct_(const char *s)
{
    if (!strcmp(s, "stronghold")) return Stronghold;
    return str2struct_(s);
}

// Structures cubiomes places from the chunk POPULATION seed rather than the
// region-based structure algorithm. The distinction matters here: a region
// structure's position depends only on the lower 48 bits (which is what makes
// pass 1 possible at all), while a population-seeded one is derived from all 64
// bits on 1.18+ (Xoroshiro). Filtering those on the structure seed alone
// reports positions that are not in the world -- so they are excluded from
// pass 1 and located in pass 2, where the full seed exists.
static int isPopulationFeature(int stype)
{
    return stype == Geode || stype == Desert_Well ||
           stype == End_Gateway || stype == End_Island;
}

// ------------------------------------------------------- structure footprints
//
// NOMINAL half-extents in blocks, measured out from the position the engine
// reports for an instance. These are the sizes the overlap test uses, and they
// are approximations on purpose: cubiomes models a structure's *placement*
// exactly but not its full assembled extent (getVariant sizes cover only the
// starting piece for jigsaw structures like villages, and nothing at all for
// several others). A box here is "roughly what this structure occupies",
// generous enough that a real collision is not missed and tight enough that
// two structures a hundred blocks apart are not called overlapping.
//
// Consequence, stated plainly: an overlap hit is a STRONG CANDIDATE for two
// structures generating into each other, not a proof. Sprawling structures
// (village, mineshaft, fortress) reach well past their nominal box.
static int structHalfExtent(int stype)
{
    switch (stype) {
    case Treasure:        return 1;
    case Igloo:           return 4;
    case Swamp_Hut:       return 5;
    case Jungle_Pyramid:  return 8;
    case Ocean_Ruin:      return 8;
    case Geode:           return 8;
    case Ruined_Portal:   return 8;
    case Ruined_Portal_N: return 8;
    case Desert_Pyramid:  return 11;
    case Shipwreck:       return 14;
    case Outpost:         return 16;
    case Trail_Ruins:     return 16;
    case End_City:        return 24;
    case Bastion:         return 24;
    case Mansion:         return 24;
    case Monument:        return 29;
    case Village:         return 32;
    case Mineshaft:       return 40;
    case Trial_Chambers:  return 48;
    case Fortress:        return 48;
    case Ancient_City:    return 64;
    // extendStronghold refuses to place a piece more than 112 blocks from the
    // start, so this is the true worst case. It is only used to decide WHICH
    // partners are worth testing -- the overlap itself is tested against the
    // real piece boxes, because a 112-block box would call almost anything an
    // overlap.
    case Stronghold:      return 112;
    default:              return 16;
    }
}

// The keys a condition object may carry -- every one the parser reads, plus a
// few a model adds as annotations. Anything else is a typo or an invention
// ("radius", "distance", "withn"), and it used to be skipped without a word:
// "radius": 100 searched the default 1000 and looked like a working query.
static const char *const COND_KEYS[] = {
    "abandoned", "basement", "biome", "biome_area", "cactus", "cave_below",
    "count", "cracked", "deepslate", "exact", "exposed", "eyes", "family",
    "floating", "gap", "giant", "height", "id", "island", "land", "loot",
    "maxDepth", "max_depth", "metric", "minRoofY", "min_roof_y", "of", "ore",
    "overlap", "pad", "pct", "precision", "relief", "ship", "sides", "size",
    "slime", "spread", "stronghold", "structure", "surface", "vein", "within",
    "y_max", "y_min",
    "note", "notes", "label", "comment", "description",
    NULL
};

static void unknownKeys(const cJSON *e, const char *id, char *warn, size_t n)
{
    for (const cJSON *it = e->child; it; it = it->next) {
        if (!it->string) continue;
        int known = 0;
        for (int i = 0; COND_KEYS[i] && !known; i++)
            known = !strcmp(it->string, COND_KEYS[i]);
        if (known) continue;
        size_t len = strlen(warn);
        if (len + 96 >= n) return;
        snprintf(warn + len, n - len, "%scondition \"%s\": unknown key \"%s\" was ignored",
                 len ? "; " : "", id, it->string);
    }
}

// The hard limits on a stronghold's height from 1.18, checked up front so an
// impossible request is refused instead of searched overnight.
//
// moveBelowSeaLevel puts the roof at k = span + minY + 1, then draws
// k += nextInt(53 - k) only while k < 53. Before 1.18 minY is 0, so a vertical
// span of 52+ pushes the roof above 52 -- exactly what the shallow strongholds
// are. From 1.18 minY is -64, so k starts 64 lower and would need a 116-block
// span to clear 52, which no stronghold has. So from 1.18:
//   - no roof is above y=52, and
//   - on DRY land (top block at y>=62, SEA_LEVEL_Y) the ground is at least
//     10 blocks over the roof. "shallow" (<=4) and "exposed" (<=0) are both
//     impossible there -- the old message even recommended "shallow" as the
//     fix. On the sea floor they are possible: an ocean floor below y=52 sits
//     under the roof, so with "land": false an exposed stronghold is real.
#define SH_ROOF_MAX_118 52
static int strongholdBound18(int mc, const Cond *c, char *err, size_t errlen)
{
    if (mc < MC_1_18) return 0;
    if (c->hasRoofMin && c->minRoofY > SH_ROOF_MAX_118) {
        snprintf(err, errlen,
            "impossible: condition \"%s\" asks for a stronghold roof at y>=%d, "
            "but from 1.18 a stronghold's roof can never be above y=%d "
            "(moveBelowSeaLevel starts it 64 blocks lower than it used to). "
            "Surface-level strongholds are a pre-1.18 phenomenon -- try 1.17.",
            c->id, c->minRoofY, SH_ROOF_MAX_118);
        return 1;
    }
    int minLand = SEA_LEVEL_Y - SH_ROOF_MAX_118;     // 10 blocks
    if (c->hasMaxDepth && c->landOnly && c->maxDepth < minLand) {
        snprintf(err, errlen,
            "impossible: condition \"%s\" asks for a stronghold under at most "
            "%d block(s) of DRY ground, but from 1.18 the roof tops out at y=%d "
            "while dry land starts at y=%d, so the least ground over one is %d "
            "blocks. Ask for max_depth %d or more, set \"land\": false for one "
            "open to the sea floor, or use 1.17, where surface strongholds exist.",
            c->id, c->maxDepth, SH_ROOF_MAX_118, SEA_LEVEL_Y, minLand, minLand);
        return 1;
    }
    return 0;
}

int queryParse(Query *q, const char *json, char *err, size_t errlen)
{
    memset(q, 0, sizeof(*q));
    cJSON *root = cJSON_Parse(json);
    if (!root) { snprintf(err, errlen, "invalid JSON"); return 1; }

    int rc = 1;
    cJSON *jv = cJSON_GetObjectItem(root, "version");
    if (!jv || !cJSON_IsString(jv)) { snprintf(err, errlen, "missing \"version\""); goto done; }
    q->mc = str2mc(jv->valuestring);
    if (q->mc < 0) { snprintf(err, errlen, "unknown version \"%s\"", jv->valuestring); goto done; }

    // World preset. Large Biomes is the same generator at a different biome
    // scale, so it changes every biome-dependent answer -- and nothing else in
    // the query needs to know, as long as every Generator is built with it.
    cJSON *jlb = cJSON_GetObjectItem(root, "large_biomes");
    q->largeBiomes = (jlb && cJSON_IsTrue(jlb)) ? 1 : 0;

    cJSON *jc = cJSON_GetObjectItem(root, "conditions");
    if (!jc || !cJSON_IsArray(jc)) { snprintf(err, errlen, "missing \"conditions\" array"); goto done; }

    int n = cJSON_GetArraySize(jc);
    if (n > MAX_COND) { snprintf(err, errlen, "too many conditions (max %d)", MAX_COND); goto done; }

    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(jc, i);
        Cond *c = &q->cond[i];

        cJSON *x = cJSON_GetObjectItem(e, "id");
        if (!x || !cJSON_IsString(x)) { snprintf(err, errlen, "condition %d: missing \"id\"", i); goto done; }
        snprintf(c->id, ID_LEN, "%s", x->valuestring);

        x = cJSON_GetObjectItem(e, "of");
        // Default to the ORIGIN, not the world spawn. Both are supported, but
        // spawn is 64-bit and biome-derived, so pass 1 cannot filter on it and
        // has to widen by SPAWN_MARGIN -- turning a 2%-survival query into a
        // 96%-survival one. Make the cheap, deterministic reference the
        // default and let "spawn" be an explicit, informed choice.
        snprintf(c->ofId, ID_LEN, "%s", (x && cJSON_IsString(x)) ? x->valuestring : "origin");

        x = cJSON_GetObjectItem(e, "within");
        c->within = (x && cJSON_IsNumber(x)) ? x->valueint : 1000;

        // How `within` measures. Default stays EUCLIDEAN so no existing query
        // changes meaning; "chebyshev" is opt-in and is what a seed finder means
        // by "within N on any axis" or "within N chunks". An unrecognised value
        // is an ERROR rather than a silent fallback to the default -- a typo
        // that quietly searched a disc when a square was asked for would look
        // exactly like a rare seed.
        x = cJSON_GetObjectItem(e, "metric");
        c->metric = MET_EUCLID;
        if (x && cJSON_IsString(x)) {
            const char *mv = x->valuestring;
            if (!strcmp(mv, "chebyshev") || !strcmp(mv, "square") || !strcmp(mv, "axis"))
                c->metric = MET_CHEBYSHEV;
            else if (!strcmp(mv, "euclidean") || !strcmp(mv, "euclid") || !strcmp(mv, "disc"))
                c->metric = MET_EUCLID;
            else {
                snprintf(err, errlen,
                    "condition \"%s\": metric \"%s\" is not one of "
                    "chebyshev (square, any-axis) or euclidean (disc)",
                    c->id, mv);
                goto done;
            }
        }

        cJSON *je = cJSON_GetObjectItem(e, "eyes");
        cJSON *jl = cJSON_GetObjectItem(e, "loot");
        cJSON *js = cJSON_GetObjectItem(e, "structure");
        cJSON *jb = cJSON_GetObjectItem(e, "biome");
        cJSON *jore = cJSON_GetObjectItem(e, "ore");
        cJSON *jslime = cJSON_GetObjectItem(e, "slime");
        cJSON *jcact = cJSON_GetObjectItem(e, "cactus");
        cJSON *jarea = cJSON_GetObjectItem(e, "biome_area");
        cJSON *jheight = cJSON_GetObjectItem(e, "height");
        cJSON *jrelief = cJSON_GetObjectItem(e, "relief");
        cJSON *jisland = cJSON_GetObjectItem(e, "island");
        cJSON *jovl = cJSON_GetObjectItem(e, "overlap");
        cJSON *jsh = cJSON_GetObjectItem(e, "stronghold");
        if (jsh && !cJSON_IsNull(jsh)) {
            // A stronghold on its own: "an exposed stronghold", "a stronghold
            // right under the grass". Not a CT_STRUCTURE -- it has no region
            // position and cannot go through getStructurePos -- and not an
            // overlap either, because there is no second structure involved.
            c->type = CT_STRONGHOLD;
            c->dim  = DIM_OVERWORLD;
            if (cJSON_IsString(jsh) && jsh->valuestring) {
                if (!strcmp(jsh->valuestring, "exposed")) {
                    c->hasMaxDepth = 1; c->maxDepth = 0;
                } else if (!strcmp(jsh->valuestring, "shallow")) {
                    c->hasMaxDepth = 1; c->maxDepth = 4;
                } else if (strcmp(jsh->valuestring, "any")) {
                    snprintf(err, errlen,
                        "condition \"%s\": stronghold must be true, \"any\", "
                        "\"shallow\" or \"exposed\" -- got \"%s\"",
                        c->id, jsh->valuestring);
                    goto done;
                }
            } else if (cJSON_IsNumber(jsh)) {
                c->hasMaxDepth = 1; c->maxDepth = jsh->valueint;
            }
            cJSON *jd = cJSON_GetObjectItem(e, "max_depth");
            if (!jd) jd = cJSON_GetObjectItem(e, "maxDepth");
            if (jd && cJSON_IsNumber(jd)) { c->hasMaxDepth = 1; c->maxDepth = jd->valueint; }
            cJSON *jr = cJSON_GetObjectItem(e, "min_roof_y");
            if (!jr) jr = cJSON_GetObjectItem(e, "minRoofY");
            if (jr && cJSON_IsNumber(jr)) { c->hasRoofMin = 1; c->minRoofY = jr->valueint; }
            cJSON *jland = cJSON_GetObjectItem(e, "land");
            c->landOnly = (jland && cJSON_IsFalse(jland)) ? 0 : 1;

            // The same hard bounds the overlap form checks (strongholdBound18),
            // which need `land` -- so it is read first.
            if (strongholdBound18(q->mc, c, err, errlen)) goto done;
            cJSON *sw = cJSON_GetObjectItem(e, "within");
            c->within = (sw && cJSON_IsNumber(sw)) ? sw->valueint : 3000;
            if (c->within > 30000) c->within = 30000;
            if (c->within < 64)    c->within = 64;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if (jovl && cJSON_IsArray(jovl) && cJSON_GetArraySize(jovl) == 2) {
            // Two structures generating into the same ground: a ruined portal
            // inside a village, a mineshaft under a monument. Distance alone
            // cannot express this -- "within 30 blocks" is satisfied by two
            // structures that merely sit near each other, while a real
            // collision is about their FOOTPRINTS meeting.
            c->type = CT_OVERLAP;
            const char *an = cJSON_GetArrayItem(jovl, 0)->valuestring;
            const char *bn = cJSON_GetArrayItem(jovl, 1)->valuestring;
            if (!an || !bn) {
                snprintf(err, errlen, "condition \"%s\": overlap needs two structure names", c->id);
                goto done;
            }
            c->structType  = str2overlapStruct_(an);
            c->structType2 = str2overlapStruct_(bn);
            if (c->structType < 0 || c->structType2 < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown structure in overlap", c->id);
                goto done;
            }
            if (c->structType == Stronghold && c->structType2 == Stronghold) {
                snprintf(err, errlen,
                    "condition \"%s\": two strongholds cannot overlap -- they are "
                    "generated in rings and never share ground", c->id);
                goto done;
            }
            // How high the stronghold's highest piece must reach. This is the
            // version-independent way to ask for a SHALLOW stronghold: block
            // terrain only exists from 1.18 here, but a roof at y=62 is at the
            // surface in every version.
            cJSON *jroof = cJSON_GetObjectItem(e, "min_roof_y");
            if (!jroof) jroof = cJSON_GetObjectItem(e, "minRoofY");
            if (jroof) {
                if (c->structType != Stronghold && c->structType2 != Stronghold) {
                    snprintf(err, errlen,
                        "condition \"%s\": min_roof_y only means something for a "
                        "stronghold overlap -- no other structure here carries a Y",
                        c->id);
                    goto done;
                }
                c->minRoofY = jroof->valueint;
                c->hasRoofMin = 1;
            }
            // max_depth: ground over the piece that meets the partner, in
            // blocks. 0 = it breaks the surface, i.e. genuinely EXPOSED. This
            // is the terrain-aware question; min_roof_y above is only a proxy
            // for it, and the two disagree in exactly the case people care
            // about -- a roof at y=60 under a hill is 5 blocks down, not out.
            cJSON *jdep = cJSON_GetObjectItem(e, "max_depth");
            if (!jdep) jdep = cJSON_GetObjectItem(e, "maxDepth");
            if (jdep && cJSON_IsNumber(jdep)) {
                if (c->structType != Stronghold && c->structType2 != Stronghold) {
                    snprintf(err, errlen,
                        "condition \"%s\": max_depth only means something for a "
                        "stronghold overlap -- no other structure here carries a Y",
                        c->id);
                    goto done;
                }
                c->maxDepth = jdep->valueint;
                c->hasMaxDepth = 1;
                // Default to dry land, exactly as the standalone condition
                // does. Without it the first results were strongholds breaking
                // out through a river bed at y<=52 -- true by the arithmetic,
                // useless to walk to, and a village on the bank does nothing to
                // make them the find people mean.
                cJSON *jl = cJSON_GetObjectItem(e, "land");
                c->landOnly = (jl && cJSON_IsFalse(jl)) ? 0 : 1;
            }
            if ((c->structType == Stronghold || c->structType2 == Stronghold) &&
                strongholdBound18(q->mc, c, err, errlen))
                goto done;
            // A stronghold has no StructureConfig; it is always overworld.
            StructureConfig sa, sb;
            if (c->structType == Stronghold) {
                if (!getStructureConfig(c->structType2, q->mc, &sb)) {
                    snprintf(err, errlen, "condition \"%s\": %s does not exist in %s",
                             c->id, bn, jv->valuestring);
                    goto done;
                }
                sa = sb; sa.dim = DIM_OVERWORLD;
                if (sb.dim != DIM_OVERWORLD) {
                    snprintf(err, errlen,
                        "condition \"%s\": a stronghold is an overworld structure and "
                        "%s is not -- they can never overlap", c->id, bn);
                    goto done;
                }
            } else if (c->structType2 == Stronghold) {
                if (!getStructureConfig(c->structType, q->mc, &sa)) {
                    snprintf(err, errlen, "condition \"%s\": %s does not exist in %s",
                             c->id, an, jv->valuestring);
                    goto done;
                }
                sb = sa; sb.dim = DIM_OVERWORLD;
                if (sa.dim != DIM_OVERWORLD) {
                    snprintf(err, errlen,
                        "condition \"%s\": a stronghold is an overworld structure and "
                        "%s is not -- they can never overlap", c->id, an);
                    goto done;
                }
            } else if (!getStructureConfig(c->structType, q->mc, &sa) ||
                       !getStructureConfig(c->structType2, q->mc, &sb)) {
                snprintf(err, errlen, "condition \"%s\": a structure in overlap does not exist in %s",
                         c->id, jv->valuestring);
                goto done;
            }
            if (sa.dim != sb.dim) {
                snprintf(err, errlen,
                    "condition \"%s\": %s and %s are in different dimensions -- they can never overlap",
                    c->id, an, bn);
                goto done;
            }
            c->dim = sa.dim;
            cJSON *jpad = cJSON_GetObjectItem(e, "pad");
            c->overlapPad = (jpad && cJSON_IsNumber(jpad)) ? jpad->valueint : 0;
            if (c->overlapPad < 0)   c->overlapPad = 0;
            if (c->overlapPad > 128) c->overlapPad = 128;
            // `within` is how far from the reference the PAIR may be, not how
            // close the two structures are (that is the footprint test).
            cJSON *ow = cJSON_GetObjectItem(e, "within");
            c->within = (ow && cJSON_IsNumber(ow)) ? ow->valueint : 3000;
            if (c->within > 30000) c->within = 30000;
            if (c->within < 64)    c->within = 64;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if (jisland && cJSON_IsTrue(jisland)) {
            // Island / survival-island spawn: the reference point is LAND, and
            // ocean covers at least `pct`% of the surrounding disc -- a small
            // landmass in open water. Defaults to spawn (the useful case).
            c->type = CT_ISLAND;
            c->dim = DIM_OVERWORLD;
            cJSON *ip = cJSON_GetObjectItem(e, "pct");
            c->areaPct = (ip && cJSON_IsNumber(ip)) ? ip->valueint : 75;
            if (c->areaPct < 1)   c->areaPct = 1;
            if (c->areaPct > 100) c->areaPct = 100;
            cJSON *iw = cJSON_GetObjectItem(e, "within");
            c->within = (iw && cJSON_IsNumber(iw)) ? iw->valueint : 256;
            if (c->within < 48)   c->within = 48;
            if (c->within > 2048) c->within = 2048;
            c->scanStep = SCAN_FINE;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "spawn");
        } else if (jcact && cJSON_IsNumber(jcact)) {
            c->type = CT_CACTUS;
            c->dim = DIM_OVERWORLD;
            c->cactusMin = jcact->valueint;
            if (c->cactusMin < 1) c->cactusMin = 1;
            if (q->mc < MC_1_18) {
                snprintf(err, errlen, "condition \"%s\": cactus height needs "
                         "block-level terrain, which is 1.18+", c->id);
                goto done;
            }
            cJSON *cw = cJSON_GetObjectItem(e, "within");
            c->within = (cw && cJSON_IsNumber(cw)) ? cw->valueint : 128;
            // Every chunk in range must roll its patch and most of the cost is
            // real terrain, so a huge radius is a different query, not a slower
            // one. 512 blocks is already ~1000 chunks per seed.
            if (c->within > 512) c->within = 512;
            if (c->within < 16)  c->within = 16;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if (jslime && cJSON_IsNumber(jslime)) {
            c->type = CT_SLIME;
            c->dim = DIM_OVERWORLD;              // slime chunks are overworld
            c->slimeMin = jslime->valueint;
            if (c->slimeMin < 1) c->slimeMin = 1;
            // A cluster query: count slime chunks in a small disc. Default small
            // (a dense cluster near the point is the useful, rare thing).
            cJSON *sw = cJSON_GetObjectItem(e, "within");
            c->within = (sw && cJSON_IsNumber(sw)) ? sw->valueint : 128;
            if (c->within > 2048) c->within = 2048;
            if (c->within < 16)   c->within = 16;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if ((jheight && cJSON_IsNumber(jheight)) ||
                   (jrelief && cJSON_IsNumber(jrelief))) {
            // APPROXIMATE terrain height: the disc must contain a surface point
            // at least "height" high AND/OR a peak-to-valley spread ("relief") of
            // at least that many blocks (mapApproxHeight -- an estimate, not exact
            // game height). Best on 1.18+; overworld only.
            //   height : find tall terrain (a peak/mountain)
            //   relief : find STEEP terrain -- a big local drop. Pair with
            //            of:<structure> + a small radius for a structure sitting
            //            on a cliff edge / halfway up a mountainside.
            c->type = CT_HEIGHT;
            c->dim = DIM_OVERWORLD;
            // -64 = world floor = "no minimum" when only relief is asked for.
            c->heightMin = (jheight && cJSON_IsNumber(jheight)) ? jheight->valueint : -64;
            c->reliefMin = (jrelief && cJSON_IsNumber(jrelief)) ? jrelief->valueint : 0;
            if (c->reliefMin < 0) c->reliefMin = 0;
            // A small disc so "tall terrain at an outpost" (of: outpost) means
            // at the outpost. Default modest; cap so the scan can't wedge.
            cJSON *hw = cJSON_GetObjectItem(e, "within");
            c->within = (hw && cJSON_IsNumber(hw)) ? hw->valueint : 64;
            if (c->within > 2048) c->within = 2048;
            if (c->within < 4)    c->within = 4;
            c->scanStep = SCAN_FINE;
            cJSON *hp = cJSON_GetObjectItem(e, "precision");
            if (hp && cJSON_IsString(hp)) {
                if      (!strcmp(hp->valuestring, "fast"))  c->scanStep = SCAN_FAST;
                else if (!strcmp(hp->valuestring, "fine"))  c->scanStep = SCAN_FINE;
                else if (!strcmp(hp->valuestring, "exact")) c->scanStep = SCAN_EXACT;
            }
            // "exact": real block-level terrain (generateColumn) instead of the
            // 1:4 smoothed approximation. The only way to see a sharp drop right
            // under a structure's footprint. 1.18+ only.
            cJSON *jex = cJSON_GetObjectItem(e, "exact");
            if (jex && cJSON_IsBool(jex) && cJSON_IsTrue(jex)) {
                if (q->mc < MC_1_18) {
                    snprintf(err, errlen, "condition \"%s\": \"exact\" terrain needs MC 1.18+",
                             c->id);
                    goto done;
                }
                c->exactTerrain = 1;
                // Exact terrain is per-block; a big radius is ruinously slow. This
                // is a footprint check, so cap it hard.
                if (c->within > 48) c->within = 48;
            }
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if (jore && cJSON_IsString(jore)) {
            c->type = CT_ORE;
            const OreMaterial *mat = oreMaterialByName(jore->valuestring);
            if (!mat) {
                snprintf(err, errlen, "condition \"%s\": unknown ore \"%s\"",
                         c->id, jore->valuestring);
                goto done;
            }
            // Find the material's index (stored so query.h needs no ore types).
            int nm; const OreMaterial *tab = oreMaterials(&nm);
            c->oreMat = (int)(mat - tab);
            c->dim = mat->dim;
            cJSON *oc = cJSON_GetObjectItem(e, "count");
            c->oreMin = (oc && cJSON_IsNumber(oc)) ? oc->valueint : 1;
            if (c->oreMin < 1) c->oreMin = 1;
            // "vein": the biggest CONNECTED blob, not the scattered total. This
            // is the record-hunting measure -- 40 diamonds spread over a disc is
            // ordinary, 12 in one vein is not.
            cJSON *ov = cJSON_GetObjectItem(e, "vein");
            c->veinMin = (ov && cJSON_IsNumber(ov)) ? ov->valueint : 0;
            if (c->veinMin < 0) c->veinMin = 0;
            // "exposed": only count ore with an air neighbour in real terrain --
            // ore you can see in a cave wall. Needs block-level terrain.
            cJSON *oe = cJSON_GetObjectItem(e, "exposed");
            if (oe && cJSON_IsTrue(oe)) {
                if (q->mc < MC_1_18) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"exposed\" ore needs MC 1.18+ (block-level terrain)",
                        c->id);
                    goto done;
                }
                if (mat->dim != DIM_OVERWORLD) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"exposed\" ore is overworld-only", c->id);
                    goto done;
                }
                c->oreExposed = 1;
            }
            // Y band. "deepslate emerald ore" is not a different ore -- it is
            // the SAME ore, wearing whatever stone it replaced, so the block
            // form is decided entirely by the Y it generated at. Below y=0 on
            // 1.18+ is deepslate; above it is stone. Expressing it as a band
            // means every ore gets the distinction for free, and the engine
            // never has to carry two block ids for one material.
            c->oreYMin = -ORE_Y_ANY; c->oreYMax = ORE_Y_ANY;
            cJSON *jdeep = cJSON_GetObjectItem(e, "deepslate");
            if (jdeep && cJSON_IsTrue(jdeep)) {
                if (q->mc < MC_1_18) {
                    snprintf(err, errlen,
                        "condition \"%s\": deepslate only exists from 1.18 -- "
                        "before that the whole column is stone", c->id);
                    goto done;
                }
                c->oreYMax = -1;
            }
            cJSON *jymin = cJSON_GetObjectItem(e, "y_min");
            if (jymin && cJSON_IsNumber(jymin)) c->oreYMin = jymin->valueint;
            cJSON *jymax = cJSON_GetObjectItem(e, "y_max");
            if (jymax && cJSON_IsNumber(jymax)) c->oreYMax = jymax->valueint;
            if (c->oreYMin > c->oreYMax) {
                snprintf(err, errlen,
                    "condition \"%s\": y_min %d is above y_max %d, so no block "
                    "can satisfy it", c->id, c->oreYMin, c->oreYMax);
                goto done;
            }
            // Counting is per-chunk over the whole column, so cost scales with
            // area. Default modest; cap so it cannot wedge the search.
            cJSON *ow = cJSON_GetObjectItem(e, "within");
            c->within = (ow && cJSON_IsNumber(ow)) ? ow->valueint : 64;
            if (c->within > 256) c->within = 256;
            if (c->within < 16)  c->within = 16;
            // Exposure generates real terrain per candidate chunk, so a wide
            // disc is ruinous. Keep it to a cave's worth of ground.
            if (c->oreExposed && c->within > 64) c->within = 64;
            // Ore is measured from a point; default origin. Spawn only makes
            // sense in the overworld (guarded later, like every condition).
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
        } else if (jl && cJSON_IsObject(jl)) {
            c->type = CT_LOOT;
            cJSON *ls = cJSON_GetObjectItem(jl, "structure");
            cJSON *li = cJSON_GetObjectItem(jl, "item");
            cJSON *lc = cJSON_GetObjectItem(jl, "count");
            if (!ls || !cJSON_IsString(ls) || !li || !cJSON_IsString(li)) {
                snprintf(err, errlen,
                    "condition \"%s\": loot needs \"structure\" and \"item\"", c->id);
                goto done;
            }
            c->structType = str2struct_(ls->valuestring);
            if (c->structType < 0 || !lootStructureSupported(c->structType)) {
                snprintf(err, errlen,
                    "condition \"%s\": loot search not supported for \"%s\" "
                    "(try desert_pyramid, jungle_temple, igloo, outpost, shipwreck, "
                    "ruined_portal, fortress, bastion)",
                    c->id, ls->valuestring);
                goto done;
            }
            // Accept "diamond" or "minecraft:diamond".
            if (strchr(li->valuestring, ':'))
                snprintf(c->lootItem, sizeof c->lootItem, "%s", li->valuestring);
            else
                snprintf(c->lootItem, sizeof c->lootItem, "minecraft:%s", li->valuestring);
            c->lootMin = (lc && cJSON_IsNumber(lc)) ? lc->valueint : 1;
            if (c->lootMin < 1) c->lootMin = 1;
            // Loot rolls per instance, so the radius bounds cost directly.
            // Default small; cap so a stray big radius can't wedge the search.
            cJSON *lw = cJSON_GetObjectItem(e, "within");
            c->within = (lw && cJSON_IsNumber(lw)) ? lw->valueint : 3000;
            if (c->within > 10000) c->within = 10000;
            if (c->within < 100)   c->within = 100;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
            StructureConfig sc;
            if (!getStructureConfig(c->structType, q->mc, &sc)) {
                snprintf(err, errlen, "condition \"%s\": %s not in %s",
                         c->id, ls->valuestring, jv->valuestring);
                goto done;
            }
            c->dim = sc.dim;
        } else if (je && cJSON_IsNumber(je)) {
            c->type = CT_EYES;
            c->eyesMin = je->valueint;
            c->dim = DIM_OVERWORLD;
            c->parent = PARENT_ORIGIN;   // a whole-world property, not a place
            if (c->eyesMin < 0 || c->eyesMin > EYE_FRAMES) {
                snprintf(err, errlen, "condition \"%s\": eyes must be 0..%d",
                         c->id, EYE_FRAMES);
                goto done;
            }
        } else if (js && cJSON_IsString(js)) {
            c->type = CT_STRUCTURE;
            c->structType = str2struct_(js->valuestring);
            if (c->structType < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown structure \"%s\"", c->id, js->valuestring);
                goto done;
            }
            StructureConfig sc;
            if (!getStructureConfig(c->structType, q->mc, &sc)) {
                snprintf(err, errlen, "condition \"%s\": %s does not exist in %s",
                         c->id, js->valuestring, jv->valuestring);
                goto done;
            }
            c->dim = sc.dim;   // authoritative: the engine's own config
            // "surface": reject the buried variant. Only ruined portals have a
            // buried variant the engine can identify exactly (getVariant's
            // underground flag), so refuse the flag elsewhere rather than
            // silently ignore it.
            cJSON *jsurf = cJSON_GetObjectItem(e, "surface");
            if (jsurf && cJSON_IsTrue(jsurf)) {
                if (c->structType != Ruined_Portal && c->structType != Ruined_Portal_N) {
                    snprintf(err, errlen,
                             "condition \"%s\": \"surface\" only applies to ruined_portal",
                             c->id);
                    goto done;
                }
                c->surfaceOnly = 1;
            }
            // Variant requirements. Each is exact (getVariant reads the same RNG
            // the game does) and each is gated to the structure that has it, so
            // a misplaced flag is an error rather than a silent no-op.
            cJSON *jab = cJSON_GetObjectItem(e, "abandoned");
            if (jab && cJSON_IsTrue(jab)) {
                if (c->structType != Village) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"abandoned\" (zombie village) only applies to village", c->id);
                    goto done;
                }
                c->reqAbandoned = 1;
            }
            cJSON *jbase = cJSON_GetObjectItem(e, "basement");
            if (jbase && cJSON_IsTrue(jbase)) {
                if (c->structType != Igloo) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"basement\" only applies to igloo", c->id);
                    goto done;
                }
                c->reqBasement = 1;
            }
            cJSON *jgi = cJSON_GetObjectItem(e, "giant");
            if (jgi && cJSON_IsTrue(jgi)) {
                if (c->structType != Ruined_Portal && c->structType != Ruined_Portal_N) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"giant\" only applies to ruined_portal", c->id);
                    goto done;
                }
                c->reqGiant = 1;
            }
            // "exposed": buried treasure whose chest sits on dry land at/above
            // sea level (visible/reachable) rather than submerged underwater.
            // Checked against real terrain height at the chest.
            cJSON *jexp = cJSON_GetObjectItem(e, "exposed");
            if (jexp && cJSON_IsTrue(jexp)) {
                if (c->structType != Treasure) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"exposed\" only applies to buried_treasure", c->id);
                    goto done;
                }
                c->reqExposed = 1;
            }
            // "size" / "cracked": amethyst geode shape. getVariant reads the
            // same draws the game makes, so both are exact. Size is the geode's
            // generated radius parameter -- bigger means more amethyst, and the
            // top of the range is rare enough to be worth ranking on.
            cJSON *jsz = cJSON_GetObjectItem(e, "size");
            if (jsz && cJSON_IsNumber(jsz)) {
                if (c->structType != Geode) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"size\" only applies to geode", c->id);
                    goto done;
                }
                c->geodeSize = jsz->valueint;
                if (c->geodeSize < 0) c->geodeSize = 0;
            }
            // "cracked": true wants a geode broken open (95% of them, so barely
            // a filter); FALSE wants the rare SEALED one -- 1 in 20, and the
            // only kind that is still full of amethyst when you find it.
            cJSON *jcr = cJSON_GetObjectItem(e, "cracked");
            if (jcr && cJSON_IsBool(jcr)) {
                if (c->structType != Geode) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"cracked\" only applies to geode", c->id);
                    goto done;
                }
                c->reqCracked = cJSON_IsTrue(jcr) ? 1 : -1;
            }
            cJSON *jfloat = cJSON_GetObjectItem(e, "floating");
            if (jfloat && cJSON_IsNumber(jfloat)) {
                if (q->mc < MC_1_18) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"floating\" needs MC 1.18+ "
                        "(block-level terrain)", c->id);
                    goto done;
                }
                if (sc.dim != DIM_OVERWORLD) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"floating\" is overworld-only", c->id);
                    goto done;
                }
                c->floatVoid = jfloat->valueint;
                if (c->floatVoid < 1) c->floatVoid = 1;
                cJSON *jr = cJSON_GetObjectItem(e, "gap");
                c->floatRing = (jr && cJSON_IsNumber(jr)) ? jr->valueint : 24;
                if (c->floatRing < 4) c->floatRing = 4;
                cJSON *jn = cJSON_GetObjectItem(e, "sides");
                c->floatNeed = (jn && cJSON_IsNumber(jn)) ? jn->valueint : 6;
                if (c->floatNeed < 1) c->floatNeed = 1;
                if (c->floatNeed > 8) c->floatNeed = 8;
            }
            // "cave_below": N -- the structure stands over an open cavern at
            // least N blocks tall. Real block terrain, so this sees 1.18's
            // noise caves, not just carved tunnels. A village on a thin crust
            // above a cathedral-sized void is the thing this finds.
            cJSON *jcave = cJSON_GetObjectItem(e, "cave_below");
            if (jcave && cJSON_IsNumber(jcave)) {
                if (q->mc < MC_1_18) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"cave_below\" needs MC 1.18+ "
                        "(block-level terrain)", c->id);
                    goto done;
                }
                if (sc.dim != DIM_OVERWORLD) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"cave_below\" is overworld-only", c->id);
                    goto done;
                }
                c->caveBelow = jcave->valueint;
                if (c->caveBelow < 1) c->caveBelow = 1;
            }
            // "ship": end city that contains an end ship (guaranteed elytra).
            cJSON *jship = cJSON_GetObjectItem(e, "ship");
            if (jship && cJSON_IsTrue(jship)) {
                if (c->structType != End_City) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"ship\" only applies to end_city", c->id);
                    goto done;
                }
                c->reqShip = 1;
            }
            // "count": N asks for a CLUSTER -- at least N viable instances of
            // this structure within the radius (triple village, huts near
            // spawn). Default 1 = an ordinary single-instance match.
            cJSON *jcnt = cJSON_GetObjectItem(e, "count");
            c->structMin = (jcnt && cJSON_IsNumber(jcnt)) ? jcnt->valueint : 1;
            if (c->structMin < 1) c->structMin = 1;
            if (c->structMin > 1 && isPopulationFeature(c->structType)) {
                // The cluster paths count instances from the 48-bit structure
                // seed, which does not locate a population-seeded feature.
                snprintf(err, errlen,
                    "condition \"%s\": \"count\" clusters are not supported for %s "
                    "(it is placed from the chunk population seed, not the region grid)",
                    c->id, js->valuestring);
                goto done;
            }
            if (c->structMin > 1 && (c->surfaceOnly || c->reqAbandoned ||
                                     c->reqBasement || c->reqGiant)) {
                snprintf(err, errlen,
                    "condition \"%s\": a variant filter can't be combined with a "
                    "cluster count", c->id);
                goto done;
            }
            // "spread": T -> a TIGHT cluster. The N instances must fit within T
            // blocks of a common member, located anywhere within `within` of the
            // reference (this is the "quad huts anywhere" search). `within` here
            // is the search reach, not the cluster size.
            cJSON *jspr = cJSON_GetObjectItem(e, "spread");
            c->spread = (jspr && cJSON_IsNumber(jspr)) ? jspr->valueint : 0;
            if (c->spread > 0) {
                if (c->structMin < 2) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"spread\" needs \"count\" >= 2", c->id);
                    goto done;
                }
                if (c->spread < 16)   c->spread = 16;
                if (c->spread > 2048) c->spread = 2048;
                // Search reach: default modest, cap so the region window stays
                // bounded (tightCluster clamps to 64 regions per side anyway).
                cJSON *tw = cJSON_GetObjectItem(e, "within");
                c->within = (tw && cJSON_IsNumber(tw)) ? tw->valueint : 2000;
                if (c->within > 12000) c->within = 12000;
                if (c->within < 256)   c->within = 256;
            }
        } else if (jb && cJSON_IsString(jb)) {
            c->type = CT_BIOME;
            snprintf(c->biomeLabel, sizeof c->biomeLabel, "%s", jb->valuestring);
            // "family":true means the name is a FAMILY -- badlands covers
            // wooded and eroded too. It is opt-in rather than inferred because
            // most family names are ALSO biome names ("badlands", "jungle",
            // "taiga"), and inferring would make the same word mean the narrow
            // thing or the wide thing depending on version, silently.
            cJSON *jfam = cJSON_GetObjectItem(e, "family");
            if (jfam && cJSON_IsTrue(jfam)) {
                int rep = -1;
                int nmem = biomeCategory_(q->mc, jb->valuestring, c->biomeSet, &rep);
                if (nmem > 0) { c->biomeIsSet = 1; c->biomeId = rep; }
                else {
                    snprintf(err, errlen,
                        "condition \"%s\": \"%s\" is not a biome family "
                        "(try badlands, mesa, jungle, taiga, forest, savanna, "
                        "swamp, ocean, snowy, mountain, mushroom)",
                        c->id, jb->valuestring);
                    goto done;
                }
            } else c->biomeId = str2biome_(q->mc, jb->valuestring);
            if (c->biomeId < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown biome \"%s\"", c->id, jb->valuestring);
                goto done;
            }
            c->dim = getDimension(c->biomeId);   // inferred, no JSON field needed
            // Scan precision: recall/cost tradeoff, documented in query.h.
            // Defaults to "fine" -- silently discarding ~1 in 10 matching
            // seeds is a worse failure than being slower on a path that
            // usually runs only for pass-1 survivors.
            c->scanStep = SCAN_FINE;
            cJSON *jp = cJSON_GetObjectItem(e, "precision");
            if (jp && cJSON_IsString(jp)) {
                if      (!strcmp(jp->valuestring, "fast"))  c->scanStep = SCAN_FAST;
                else if (!strcmp(jp->valuestring, "fine"))  c->scanStep = SCAN_FINE;
                else if (!strcmp(jp->valuestring, "exact")) c->scanStep = SCAN_EXACT;
                else {
                    snprintf(err, errlen,
                             "condition \"%s\": precision must be fast|fine|exact, got \"%s\"",
                             c->id, jp->valuestring);
                    goto done;
                }
            }
        } else if (jarea && cJSON_IsString(jarea)) {
            c->type = CT_BIOME_AREA;
            snprintf(c->biomeLabel, sizeof c->biomeLabel, "%s", jarea->valuestring);
            // "family":true means the name is a FAMILY -- badlands covers
            // wooded and eroded too. It is opt-in rather than inferred because
            // most family names are ALSO biome names ("badlands", "jungle",
            // "taiga"), and inferring would make the same word mean the narrow
            // thing or the wide thing depending on version, silently.
            cJSON *jfam = cJSON_GetObjectItem(e, "family");
            if (jfam && cJSON_IsTrue(jfam)) {
                int rep = -1;
                int nmem = biomeCategory_(q->mc, jarea->valuestring, c->biomeSet, &rep);
                if (nmem > 0) { c->biomeIsSet = 1; c->biomeId = rep; }
                else {
                    snprintf(err, errlen,
                        "condition \"%s\": \"%s\" is not a biome family "
                        "(try badlands, mesa, jungle, taiga, forest, savanna, "
                        "swamp, ocean, snowy, mountain, mushroom)",
                        c->id, jarea->valuestring);
                    goto done;
                }
            } else c->biomeId = str2biome_(q->mc, jarea->valuestring);
            if (c->biomeId < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown biome \"%s\"", c->id, jarea->valuestring);
                goto done;
            }
            c->dim = getDimension(c->biomeId);
            // Fraction of the disc, as a percentage, that must be this biome.
            // A size filter: a big radius + high pct finds a genuinely large
            // biome (huge mushroom island, sprawling mesa) around the point.
            cJSON *jpct = cJSON_GetObjectItem(e, "pct");
            c->areaPct = (jpct && cJSON_IsNumber(jpct)) ? jpct->valueint : 50;
            if (c->areaPct < 1)   c->areaPct = 1;
            if (c->areaPct > 100) c->areaPct = 100;
            // Radius bounds the sampled region; default a modest disc, cap so a
            // stray radius can't wedge the search (area scans every cell).
            cJSON *aw = cJSON_GetObjectItem(e, "within");
            c->within = (aw && cJSON_IsNumber(aw)) ? aw->valueint : 800;
            if (c->within > 4000) c->within = 4000;
            if (c->within < 64)   c->within = 64;
            cJSON *of = cJSON_GetObjectItem(e, "of");
            snprintf(c->ofId, ID_LEN, "%s", (of && cJSON_IsString(of)) ? of->valuestring : "origin");
            // Same recall/cost tradeoff as a plain biome scan (see query.h).
            c->scanStep = SCAN_FINE;
            cJSON *jp = cJSON_GetObjectItem(e, "precision");
            if (jp && cJSON_IsString(jp)) {
                if      (!strcmp(jp->valuestring, "fast"))  c->scanStep = SCAN_FAST;
                else if (!strcmp(jp->valuestring, "fine"))  c->scanStep = SCAN_FINE;
                else if (!strcmp(jp->valuestring, "exact")) c->scanStep = SCAN_EXACT;
                else {
                    snprintf(err, errlen,
                             "condition \"%s\": precision must be fast|fine|exact, got \"%s\"",
                             c->id, jp->valuestring);
                    goto done;
                }
            }
        } else {
            // "exposed" on its own is a near miss, not nonsense: it is a
            // buried-treasure modifier, and a planner reaching for an exposed
            // STRONGHOLD writes it by analogy. Saying "need structure or biome"
            // to that is technically true and useless -- name the key it wanted.
            if (cJSON_GetObjectItem(e, "exposed"))
                snprintf(err, errlen,
                    "condition \"%s\": \"exposed\" is a modifier, not a condition. "
                    "For a stronghold breaking the surface write "
                    "{\"id\":\"%s\",\"stronghold\":\"exposed\",\"within\":3000,"
                    "\"of\":\"origin\"} -- and it needs a pre-1.18 version. For an "
                    "exposed buried treasure chest, add \"structure\":"
                    "\"buried_treasure\" alongside it.", c->id, c->id);
            else
                snprintf(err, errlen, "condition \"%s\": need \"structure\" or \"biome\"", c->id);
            goto done;
        }

        // Every other condition type clamps its own radius; structures and
        // biomes took any number. A stray value -- a typo, or a model writing
        // 50000 -- is not harmless: a biome scan is quadratic in it (30000 at
        // "fine" is ~11M samples per seed, a search that never returns) and
        // within + SPAWN_MARGIN can overflow. Refused rather than clamped, so
        // the query never silently means something other than what was asked;
        // a far-away search is "of":"x,z" with a modest radius.
        if (c->type == CT_STRUCTURE || c->type == CT_BIOME) {
            int cap = (c->type == CT_BIOME) ? 10000 : 100000;
            if (c->within < 0 || c->within > cap) {
                snprintf(err, errlen,
                    "condition \"%s\": within=%d is out of range for a %s (0..%d). "
                    "To search far from the origin, use \"of\": \"<x>,<z>\" with "
                    "a smaller radius.", c->id, c->within,
                    c->type == CT_BIOME ? "biome" : "structure", cap);
                goto done;
            }
        }
        unknownKeys(e, c->id, q->warn, sizeof q->warn);
    }
    q->n = n;
    q->rankIdx = -1;

    // Optional leaderboard: {"rank": {"of": <cond id>, "by": <metric>, "top": K}}
    cJSON *jrank = cJSON_GetObjectItem(root, "rank");
    if (jrank && cJSON_IsObject(jrank)) {
        cJSON *ro = cJSON_GetObjectItem(jrank, "of");
        if (!ro || !cJSON_IsString(ro)) {
            snprintf(err, errlen, "\"rank\" needs \"of\": the id of the condition to rank by");
            goto done;
        }
        snprintf(q->rankOf, ID_LEN, "%s", ro->valuestring);
        cJSON *rt = cJSON_GetObjectItem(jrank, "top");
        q->rankTop = (rt && cJSON_IsNumber(rt)) ? rt->valueint : 10;
        if (q->rankTop < 1)  q->rankTop = 1;
        if (q->rankTop > 50) q->rankTop = 50;
        q->rankBy = RANK_AUTO;
        cJSON *rb = cJSON_GetObjectItem(jrank, "by");
        if (rb && cJSON_IsString(rb)) {
            const char *b = rb->valuestring;
            if      (!strcmp(b, "height"))  q->rankBy = RANK_HEIGHT;
            else if (!strcmp(b, "relief"))  q->rankBy = RANK_RELIEF;
            else if (!strcmp(b, "vein"))    q->rankBy = RANK_VEIN;
            else if (!strcmp(b, "count"))   q->rankBy = RANK_COUNT;
            else if (!strcmp(b, "pct"))     q->rankBy = RANK_PCT;
            else if (!strcmp(b, "tall"))    q->rankBy = RANK_TALL;
            else if (!strcmp(b, "size"))    q->rankBy = RANK_SIZE;
            else if (!strcmp(b, "area"))    q->rankBy = RANK_AREA;
            else if (strcmp(b, "auto")) {
                snprintf(err, errlen,
                    "\"rank\".\"by\" must be auto|height|relief|vein|count|pct|size|area, got \"%s\"", b);
                goto done;
            }
        }
    }
    rc = 0;
done:
    cJSON_Delete(root);
    return rc;
}

// ------------------------------------------------------------------ planning

static int findId(const Query *q, const char *id)
{
    for (int i = 0; i < q->n; i++)
        if (!strcmp(q->cond[i].id, id)) return i;
    return -1;
}

// ------------------------------------------------------------- feasibility
//
// Some queries cannot be satisfied by ANY seed, and searching for them is not
// merely slow -- it never ends. The two the engine can prove are the ones that
// come from Minecraft's placement rules rather than from rarity:
//
//   * a structure inside another structure's exclusion zone (an outpost within
//     10 chunks of a village -- see placementAllowed above)
//   * two structures of the same type closer together than their grid can put
//     them: offsets within a region span `chunkRange`, so two instances in
//     adjacent regions are at least (spacing - chunkRange + 1) chunks apart
//
// Rarity is deliberately NOT judged here. "12-eye portal" is astronomically
// unlikely but possible, and `--explain` already estimates that honestly; only
// genuinely impossible things are refused, or the tool would be lying about
// what it cannot do.
static int minSameTypeGapBlocks(const Query *q, int stype)
{
    StructureConfig sc;
    if (!getStructureConfig(stype, q->mc, &sc)) return 0;
    if (sc.regionSize <= 0 || sc.chunkRange <= 0) return 0;
    int gapChunks = sc.regionSize - sc.chunkRange + 1;
    return gapChunks > 0 ? gapChunks * 16 : 0;
}

static int pairsOutpostAndVillage(const Cond *a, const Cond *b)
{
    return (a->structType == Outpost && b->structType == Village) ||
           (a->structType == Village && b->structType == Outpost);
}

int queryFeasible(const Query *q, char *err, size_t errlen)
{
    const int OUTPOST_VILLAGE_MIN = (OUTPOST_VILLAGE_CHUNKS + 1) * 16;   // 176

    for (int i = 0; i < q->n; i++) {
        const Cond *c = &q->cond[i];

        // An overlap condition naming both is impossible outright: their
        // footprints can never meet if they are 11 chunks apart at best.
        if (c->type == CT_OVERLAP && q->mc >= MC_1_14 &&
            ((c->structType == Outpost && c->structType2 == Village) ||
             (c->structType == Village && c->structType2 == Outpost))) {
            snprintf(err, errlen,
                "impossible: a pillager outpost never generates within %d chunks of a "
                "village, so the two can never overlap. Minecraft's own placement data "
                "excludes it (pillager_outposts.json: exclusion_zone, other_set=villages), "
                "which also rules out an outpost inside a village",
                OUTPOST_VILLAGE_CHUNKS);
            return 1;
        }

        // A tight cluster closer than the grid allows.
        if (c->type == CT_STRUCTURE && c->structMin > 1 && c->spread > 0) {
            int gap = minSameTypeGapBlocks(q, c->structType);
            if (gap > 0 && c->spread < gap) {
                snprintf(err, errlen,
                    "impossible: two %s can never be closer than %d blocks (the "
                    "placement grid puts them in different regions), but this asks for "
                    "%d of them within %d",
                    struct2str(c->structType), gap, c->structMin, c->spread);
                return 1;
            }
        }

        // A structure measured from another structure, closer than allowed.
        int p = c->parent;
        if (c->type != CT_STRUCTURE || p < 0 || q->cond[p].type != CT_STRUCTURE)
            continue;
        if (q->mc >= MC_1_14 && pairsOutpostAndVillage(c, &q->cond[p]) &&
            c->within < OUTPOST_VILLAGE_MIN) {
            snprintf(err, errlen,
                "impossible: a pillager outpost never generates within %d chunks (%d "
                "blocks) of a village -- Minecraft's placement data excludes it. Ask "
                "for %d blocks or more",
                OUTPOST_VILLAGE_CHUNKS, OUTPOST_VILLAGE_MIN, OUTPOST_VILLAGE_MIN);
            return 1;
        }
        if (c->structType == q->cond[p].structType) {
            int gap = minSameTypeGapBlocks(q, c->structType);
            if (gap > 0 && c->within < gap) {
                snprintf(err, errlen,
                    "impossible: two %s are never closer than %d blocks, but this asks "
                    "for one within %d of another",
                    struct2str(c->structType), gap, c->within);
                return 1;
            }
        }
    }
    return 0;
}

// Is condition p's matched position produced, or moved, in pass 2? Pass 1 fixes
// a plain structure's position from 48-bit geometry and pass 2 only confirms
// it. Everything else is decided in pass 2: biomes, ore, height and the other
// scans exist only there; a spawn-relative structure is re-found around the
// true spawn; a cluster is re-counted with viability; loot settles on the
// first chest that qualifies; an overlap re-picks its pair. A structure
// measured from any of those moves with it.
//
// Anything measured FROM such a condition must run after it in pass 2 and read
// the pass-2 position. Reading the pass-1 one measured a biome around the
// village nearest the origin while the output named the village nearest spawn.
static int posFromPass2(const Query *q, int p)
{
    for (int depth = 0; depth <= MAX_COND && p >= 0; depth++) {
        const Cond *c = &q->cond[p];
        if (c->type != CT_STRUCTURE) return 1;
        if (isPopulationFeature(c->structType)) return 1;
        if (c->structMin > 1) return 1;
        if (c->parent == PARENT_SPAWN) return 1;
        p = c->parent;      // a plain structure moves exactly when its parent does
    }
    return 0;
}

int queryPlan(Query *q, char *err, size_t errlen)
{
    // resolve parents
    for (int i = 0; i < q->n; i++) {
        Cond *c = &q->cond[i];
        if (!strcmp(c->ofId, "origin")) { c->parent = PARENT_ORIGIN; continue; }
        if (!strcmp(c->ofId, "spawn"))  { c->parent = PARENT_SPAWN;  continue; }
        // "of":"<x>,<z>" -- an explicit place to search around. Everything else
        // measures from origin or spawn, both of which sit near 0,0, so without
        // this the engine could only ever look at the middle of the world.
        {
            long long ax, az; char tail;
            if (sscanf(c->ofId, "%lld,%lld%c", &ax, &az, &tail) == 2) {
                // 30 million is the world border. Beyond it the generator still
                // answers, but nothing there is reachable in a real game, so a
                // hit would be a lie by omission rather than a find.
                if (ax < -30000000LL || ax > 30000000LL ||
                    az < -30000000LL || az > 30000000LL) {
                    snprintf(err, errlen,
                        "condition \"%s\": \"of\" position %lld,%lld is outside "
                        "the world border (+/-30000000)", c->id, ax, az);
                    return 1;
                }
                c->parent = PARENT_AT;
                c->atX = (int)ax; c->atZ = (int)az;
                continue;
            }
        }
        c->parent = findId(q, c->ofId);
        if (c->parent < 0) {
            snprintf(err, errlen, "condition \"%s\": unknown parent \"%s\"", c->id, c->ofId);
            return 1;
        }
        if (c->parent == i) { snprintf(err, errlen, "condition \"%s\" references itself", c->id); return 1; }
    }
    // Mark conditions that are measured from -- a biome parent must record the
    // representative match point (nearest its own centre), not just any hit.
    for (int i = 0; i < q->n; i++) q->cond[i].isParent = 0;
    for (int i = 0; i < q->n; i++)
        if (q->cond[i].parent >= 0) q->cond[q->cond[i].parent].isParent = 1;

    // Pass 1 order: structure geometry, parents before children (a child's
    // radius is measured from its parent's matched position, so we cannot
    // reorder across that edge no matter what the cost model wants).
    int done[MAX_COND] = {0};
    q->ngeom = 0;
    // Population-seeded features (geode & friends) have no 48-bit position, so
    // they never enter pass 1. Marked done up front: the cycle check below must
    // not flag them, and nothing may be measured FROM one, because pass 1 would
    // then be filtering a radius around a position it cannot know.
    for (int i = 0; i < q->n; i++) {
        if (q->cond[i].type != CT_STRUCTURE) continue;
        if (!isPopulationFeature(q->cond[i].structType)) continue;
        if (q->cond[i].isParent) {
            snprintf(err, errlen,
                "condition \"%s\": a %s is located in pass 2, so nothing can be "
                "measured from it -- put the radius on the other condition instead",
                q->cond[i].id, struct2str(q->cond[i].structType));
            return 1;
        }
        done[i] = 1;
    }
    for (int iter = 0; iter < q->n + 1 && q->ngeom < q->n; iter++) {
        // among currently-schedulable geometry tasks, take the cheapest
        int best = -1; double bestc = 1e30;
        for (int i = 0; i < q->n; i++) {
            if (done[i] || (q->cond[i].type != CT_STRUCTURE
                          && q->cond[i].type != CT_LOOT
                          && q->cond[i].type != CT_OVERLAP)) continue;
            int p = q->cond[i].parent;
            if (p >= 0 && !done[p]) continue;          // parent not placed yet
            double c = geomCost(q, i);
            if (c < bestc) { bestc = c; best = i; }
        }
        if (best < 0) break;
        done[best] = 1;
        q->geom[q->ngeom++] = best;
    }

    // A structure, loot or overlap condition is placed in pass 1, so it cannot
    // be measured from something pass 1 never places. That used to surface as
    // "dependency cycle", which sent people hunting for a cycle that was not
    // there. Say what is actually wrong, and how to express it instead.
    for (int i = 0; i < q->n; i++) {
        CondType t = q->cond[i].type;
        int p = q->cond[i].parent;
        if (done[i] || p < 0) continue;
        if (t != CT_STRUCTURE && t != CT_LOOT && t != CT_OVERLAP) continue;
        CondType pt = q->cond[p].type;
        if (pt != CT_STRUCTURE && pt != CT_LOOT && pt != CT_OVERLAP) {
            snprintf(err, errlen,
                "condition \"%s\" is a structure measured from \"%s\", which is "
                "only located after the structure pass -- measure \"%s\" from "
                "\"%s\" instead (swap the \"of\")",
                q->cond[i].id, q->cond[p].id, q->cond[p].id, q->cond[i].id);
            return 1;
        }
    }
    // any structure condition left unscheduled means a dependency cycle
    for (int i = 0; i < q->n; i++)
        if ((q->cond[i].type == CT_STRUCTURE || q->cond[i].type == CT_LOOT
             || q->cond[i].type == CT_OVERLAP) && !done[i]) {
            snprintf(err, errlen, "dependency cycle involving \"%s\"", q->cond[i].id);
            return 1;
        }
    // An eye count has no position, so nothing can be measured from it.
    for (int i = 0; i < q->n; i++) {
        int p = q->cond[i].parent;
        if (p >= 0 && q->cond[p].type == CT_EYES) {
            snprintf(err, errlen,
                "condition \"%s\" is measured from \"%s\", an eye count, which "
                "has no position -- measure it from a stronghold instead",
                q->cond[i].id, q->cond[p].id);
            return 1;
        }
    }
    // A biome may be measured from a structure (placed in pass 1) or from
    // another biome (placed in pass 2 -- this is how "biome A adjacent to
    // biome B" is expressed: B is measured within a short radius of A). It may
    // not orbit an ore/slime/area count, which has no single meaningful point.
    for (int i = 0; i < q->n; i++) {
        if (q->cond[i].type != CT_BIOME) continue;
        int p = q->cond[i].parent;
        if (p >= 0 && q->cond[p].type != CT_STRUCTURE && q->cond[p].type != CT_BIOME) {
            snprintf(err, errlen,
                     "condition \"%s\": parent \"%s\" must be a structure or a biome",
                     q->cond[i].id, q->cond[p].id);
            return 1;
        }
    }

    // A parent's matched position is a coordinate in ITS dimension. Measuring a
    // radius from it in a different dimension is meaningless (and the nether is
    // 8:1 compressed), so refuse rather than silently produce nonsense.
    for (int i = 0; i < q->n; i++) {
        int p = q->cond[i].parent;
        if (p >= 0 && q->cond[p].dim != q->cond[i].dim) {
            snprintf(err, errlen,
                     "condition \"%s\" (%s) is measured from \"%s\" (%s) -- "
                     "cross-dimension distances are not meaningful",
                     q->cond[i].id, dimName(q->cond[i].dim),
                     q->cond[p].id, dimName(q->cond[p].dim));
            return 1;
        }
    }

    // The world spawn is an overworld position. Measuring a nether or end
    // structure from it is meaningless, so say so instead of quietly using it.
    for (int i = 0; i < q->n; i++) {
        if (q->cond[i].parent == PARENT_SPAWN && q->cond[i].dim != DIM_OVERWORLD) {
            snprintf(err, errlen,
                     "condition \"%s\" is in the %s, which has no world spawn -- "
                     "measure it from \"origin\" instead",
                     q->cond[i].id, dimName(q->cond[i].dim));
            return 1;
        }
    }

    // Distinct dimensions pass 2 must visit. applySeed is the dominant per-seed
    // cost, so this is exactly how many times we pay it.
    q->ndims = 0;
    for (int i = 0; i < q->n; i++) {
        int d = q->cond[i].dim, seen = 0;
        for (int k = 0; k < q->ndims; k++) if (q->dims[k] == d) seen = 1;
        if (!seen && q->ndims < 3) q->dims[q->ndims++] = d;
    }

    // Pass 2 order: everything biome-dependent, cheapest first. A plain
    // structure's position is fixed by pass 1, so a condition measured from one
    // has no ordering constraint. A parent whose position pass 2 produces or
    // moves (posFromPass2: biomes, scans, spawn-relative structures, clusters,
    // loot, overlaps) must run first, or its child is measured from a position
    // that is about to change. Schedule like pass 1 -- cheapest
    // currently-schedulable condition each round -- honouring those edges.
    int doneV[MAX_COND] = {0};
    q->nviab = 0;
    for (int iter = 0; iter < q->n + 1 && q->nviab < q->n; iter++) {
        int best = -1; double bestc = 1e30;
        for (int i = 0; i < q->n; i++) {
            if (doneV[i]) continue;
            int p = q->cond[i].parent;
            if (p >= 0 && posFromPass2(q, p) && !doneV[p]) continue;
            double c = viabCost(q, i);
            if (c < bestc) { bestc = c; best = i; }
        }
        if (best < 0) break;
        doneV[best] = 1;
        q->viab[q->nviab++] = best;
    }
    // Unscheduled leftovers mean a pass-2 parent cycle (A of B, B of A).
    if (q->nviab < q->n) {
        for (int i = 0; i < q->n; i++)
            if (!doneV[i]) {
                snprintf(err, errlen, "dependency cycle involving \"%s\"", q->cond[i].id);
                return 1;
            }
    }

    if (queryFeasible(q, err, errlen)) return 1;

    // Leaderboard target. Ranking by a condition the query does not have is a
    // typo, not a default -- say so rather than silently searching unranked.
    q->rankIdx = -1;
    if (q->rankTop > 0) {
        q->rankIdx = findId(q, q->rankOf);
        if (q->rankIdx < 0) {
            snprintf(err, errlen, "\"rank\": no condition with id \"%s\"", q->rankOf);
            return 1;
        }
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%s", queryScoreLabel(q));
        if (!lbl[0]) {
            snprintf(err, errlen,
                     "\"rank\": condition \"%s\" has nothing to rank by", q->rankOf);
            return 1;
        }
    }

    // Pass 1.5. Cheap enough to sit between the two passes, and it only ever
    // rejects, so it needs no place in the cost model or the dependency order.
    climatePlan(q, &q->clim);

    q->est_cost_ns = 0;
    for (int i = 0; i < q->ngeom; i++) q->est_cost_ns += geomCost(q, q->geom[i]);
    return 0;
}

// ------------------------------------------------------------ leaderboard
//
// The metric a ranked search sorts on. RANK_AUTO takes the condition's own
// natural measurement, which is what "find the biggest one" means for every
// condition type that has a magnitude at all.
static int rankMetric(const Query *q)
{
    if (q->rankIdx < 0) return RANK_AUTO;
    if (q->rankBy != RANK_AUTO) return q->rankBy;
    switch (q->cond[q->rankIdx].type) {
    case CT_HEIGHT:     return q->cond[q->rankIdx].reliefMin > 0 ? RANK_RELIEF : RANK_HEIGHT;
    case CT_CACTUS:     return RANK_TALL;
    case CT_ORE:        return q->cond[q->rankIdx].veinMin > 0 ? RANK_VEIN : RANK_COUNT;
    case CT_SLIME:      return RANK_COUNT;
    case CT_STRUCTURE:  return q->cond[q->rankIdx].structType == Geode ? RANK_SIZE : RANK_COUNT;
    case CT_LOOT:       return RANK_COUNT;
    case CT_EYES:       return RANK_COUNT;
    case CT_BIOME_AREA: return RANK_PCT;
    case CT_ISLAND:     return RANK_PCT;
    case CT_OVERLAP:    return RANK_AREA;
    default:            return RANK_AUTO;
    }
}

const char *queryScoreLabel(const Query *q)
{
    if (q->rankIdx < 0) return "";
    const Cond *c = &q->cond[q->rankIdx];
    switch (rankMetric(q)) {
    case RANK_HEIGHT: return "peak Y";
    case RANK_RELIEF: return "relief";
    case RANK_VEIN:   return "vein";
    case RANK_SIZE:   return "geode size";
    case RANK_PCT:    return "percent";
    case RANK_AREA:   return "overlap";
    case RANK_TALL:   return "cactus height";
    case RANK_COUNT:
        switch (c->type) {
        case CT_ORE:       return "ore blocks";
        case CT_SLIME:     return "slime chunks";
        case CT_STRUCTURE: return "structures";
        case CT_LOOT:      return "items";
        case CT_EYES:      return "eyes";
        default:           return "count";
        }
    default: return "";
    }
}

int queryScore(const Query *q, const Match *m)
{
    if (q->rankIdx < 0) return INT_MIN;
    int k = q->rankIdx;
    const Cond *c = &q->cond[k];
    switch (rankMetric(q)) {
    case RANK_HEIGHT: return m->peakHeight[k];
    case RANK_RELIEF: return m->peakDrop[k];
    case RANK_VEIN:   return m->veinMax[k];
    case RANK_SIZE:   return m->geodeSize[k];
    case RANK_AREA:   return m->overlapArea[k];
    case RANK_TALL:   return m->cactusTall[k];
    case RANK_PCT: {
        int tot = m->areaTotal[k];
        return tot > 0 ? (int)((int64_t)m->areaCells[k] * 100 / tot) : 0;
    }
    case RANK_COUNT:
        switch (c->type) {
        case CT_ORE:       return m->oreCount[k];
        case CT_SLIME:     return m->slimeCount[k];
        case CT_STRUCTURE: return m->structCount[k] > 0 ? m->structCount[k] : 1;
        case CT_LOOT:      return m->lootCount[k];
        case CT_EYES:      return m->eyes;
        default:           return 0;
        }
    default: return INT_MIN;
    }
}

const char *condDesc(const Query *q, int i, char *buf, size_t n)
{
    const Cond *c = &q->cond[i];
    if (c->type == CT_EYES) {
        snprintf(buf, n, "end portal with >= %d of %d eyes", c->eyesMin, EYE_FRAMES);
        return buf;
    }
    if (c->type == CT_LOOT) {
        const char *it = c->lootItem;
        if (!strncmp(it, "minecraft:", 10)) it += 10;
        snprintf(buf, n, "%s with >= %d %s in its chests",
                 struct2str(c->structType), c->lootMin, it);
        return buf;
    }
    if (c->type == CT_CACTUS) {
        snprintf(buf, n, "a cactus >= %d blocks tall within %d of %s",
                 c->cactusMin, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_ORE) {
        int nm; const OreMaterial *tab = oreMaterials(&nm);
        const char *exp = c->oreExposed ? " exposed" : "";
        if (c->veinMin > 0)
            snprintf(buf, n, ">= %d %s%s ore in ONE vein within %d of %s",
                     c->veinMin, tab[c->oreMat].name, exp, c->within, c->ofId);
        else
            snprintf(buf, n, ">= %d %s%s ore within %d of %s",
                     c->oreMin, tab[c->oreMat].name, exp, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_STRONGHOLD) {
        if (c->hasMaxDepth && c->hasRoofMin)
            snprintf(buf, n, "a stronghold with its roof <= %d below the surface "
                             "and at or above y=%d, within %d of %s",
                     c->maxDepth, c->minRoofY, c->within, c->ofId);
        else if (c->hasMaxDepth)
            snprintf(buf, n, "a stronghold with its roof %s (<= %d below the "
                             "surface), %s, within %d of %s",
                     c->maxDepth <= 0 ? "BREAKING THE SURFACE" : "near the surface",
                     c->maxDepth, c->landOnly ? "on dry land" : "sea floor allowed",
                     c->within, c->ofId);
        else if (c->hasRoofMin)
            snprintf(buf, n, "a stronghold whose roof reaches y=%d, within %d of %s",
                     c->minRoofY, c->within, c->ofId);
        else
            snprintf(buf, n, "a stronghold within %d of %s", c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_OVERLAP) {
        // A stronghold pair is tested against real piece boxes, so calling it
        // "nominal" would understate it -- and the roof threshold has to appear
        // here or a query that silently filters on Y reads as one that does not.
        int sh = (c->structType == Stronghold || c->structType2 == Stronghold);
        if (sh && c->hasRoofMin)
            snprintf(buf, n, "%s and %s meet%s (within %d of %s, real stronghold "
                             "pieces, roof at or above y=%d)",
                     struct2str(c->structType), struct2str(c->structType2),
                     c->overlapPad > 0 ? "+pad" : "", c->within, c->ofId,
                     c->minRoofY);
        else if (sh)
            snprintf(buf, n, "%s and %s meet%s (within %d of %s, real stronghold pieces)",
                     struct2str(c->structType), struct2str(c->structType2),
                     c->overlapPad > 0 ? "+pad" : "", c->within, c->ofId);
        else
            snprintf(buf, n, "%s and %s footprints meet%s (within %d of %s, nominal boxes)",
                     struct2str(c->structType), struct2str(c->structType2),
                     c->overlapPad > 0 ? "+pad" : "", c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_SLIME) {
        snprintf(buf, n, ">= %d slime chunks within %d of %s",
                 c->slimeMin, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_BIOME_AREA) {
        snprintf(buf, n, "%s covers >= %d%% within %d of %s",
                 biome2str(q->mc, c->biomeId), c->areaPct, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_ISLAND) {
        snprintf(buf, n, "island: land at %s, ocean >= %d%% within %d",
                 c->ofId, c->areaPct, c->within);
        return buf;
    }
    if (c->type == CT_HEIGHT) {
        const char *tag = c->exactTerrain ? "exact" : "approx";
        if (c->reliefMin > 0 && c->heightMin > -64)
            snprintf(buf, n, "%sterrain peak >= %d & relief >= %d within %d of %s (%s)",
                     c->exactTerrain ? "" : "~", c->heightMin, c->reliefMin, c->within, c->ofId, tag);
        else if (c->reliefMin > 0)
            snprintf(buf, n, "%sterrain relief >= %d within %d of %s (%s)",
                     c->exactTerrain ? "" : "~", c->reliefMin, c->within, c->ofId, tag);
        else
            snprintf(buf, n, "%sterrain height >= %d within %d of %s (%s)",
                     c->exactTerrain ? "" : "~", c->heightMin, c->within, c->ofId, tag);
        return buf;
    }
    const char *what = (c->type == CT_STRUCTURE)
        ? struct2str(c->structType) : biome2str(q->mc, c->biomeId);
    const char *prec = c->type != CT_BIOME ? ""
                     : c->scanStep == SCAN_FAST  ? " [fast]"
                     : c->scanStep == SCAN_EXACT ? " [exact]" : " [fine]";
    if (c->type == CT_STRUCTURE && c->structType == Geode &&
        (c->geodeSize > 0 || c->reqCracked != 0)) {
        char sz[24] = "";
        if (c->geodeSize > 0) snprintf(sz, sizeof sz, " size >= %d", c->geodeSize);
        snprintf(buf, n, "%sgeode%s within %d of %s",
                 c->reqCracked > 0 ? "cracked " : c->reqCracked < 0 ? "sealed " : "",
                 sz, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_STRUCTURE && c->structMin > 1) {
        if (c->spread > 0)
            snprintf(buf, n, ">= %d %s within %d of each other (searching %d of %s)",
                     c->structMin, what ? what : "?", c->spread, c->within, c->ofId);
        else
            snprintf(buf, n, ">= %d %s within %d of %s",
                     c->structMin, what ? what : "?", c->within, c->ofId);
        return buf;
    }
    snprintf(buf, n, "%s within %d of %s%s", what ? what : "?", c->within, c->ofId, prec);
    return buf;
}

void queryPrintPlan(const Query *q, FILE *f)
{
    char buf[96];
    fprintf(f, "plan (reordered by cost, NOT by the order you wrote them)\n");
    fprintf(f, "  pass 1  48-bit geometry -- no biome generator exists yet\n");
    for (int i = 0; i < q->ngeom; i++) {
        int k = q->geom[i];
        fprintf(f, "    %d. %-42s ~%.0f ns\n", i+1, condDesc(q, k, buf, sizeof buf), geomCost(q, k));
    }
    fprintf(f, "  pass 2  64-bit, biome-dependent -- only for pass-1 survivors\n");
    for (int i = 0; i < q->nviab; i++) {
        int k = q->viab[i];
        fprintf(f, "    %d. %-42s ~%.0f ns\n", i+1, condDesc(q, k, buf, sizeof buf), viabCost(q, k));
    }
    // Not a pass of its own: a cheap test fused into the biome scans above,
    // sparing them the full six-noise lookup wherever temperature already rules
    // the biome out. Listed so the windows are auditable.
    if (q->clim.n > 0) {
        fprintf(f, "  temperature gate, inside the pass-2 biome scans\n");
        for (int k = 0; k < q->n; k++)
            if (q->clim.use[k])
                fprintf(f, "       %-42s t in [%lld, %lld]\n",
                        condDesc(q, k, buf, sizeof buf),
                        (long long)q->clim.lo[k], (long long)q->clim.hi[k]);
    }
    fprintf(f, "  est. pass-1 cost: %.0f ns/seed\n\n", q->est_cost_ns);

    // World spawn is an ESTIMATE, and the estimator says so itself: cubiomes'
    // getSpawn is documented "may be inaccurate because the world spawn depends
    // on grass blocks". Grass is decoration, which nothing simulates at scan
    // speed. A spawn-relative result is the one kind a player checks
    // IMMEDIATELY -- they load the world and look -- so shipping it without the
    // caveat is the over-claim most likely to be caught.
    for (int i = 0; i < q->n; i++) {
        if (q->cond[i].parent != PARENT_SPAWN) continue;
        fprintf(f,
            "  NOTE: distances measured from WORLD SPAWN are approximate. The\n"
            "        spawn point depends on grass placement, which is decoration\n"
            "        and is not simulated here, so the real in-game spawn can sit\n"
            "        some way off. Structure positions are exact; their distance\n"
            "        FROM SPAWN is not.\n\n");
        break;
    }
}

// ---------------------------------------------------------------- evaluation

static inline int64_t d2(Pos a, Pos b) {
    int64_t dx = (int64_t)a.x - b.x, dz = (int64_t)a.z - b.z;
    return dx*dx + dz*dz;
}

// Distance under a condition's chosen metric, SQUARED -- so every caller keeps
// comparing against the same `lim = reach*reach` and nothing else changes.
// Chebyshev squared is just max(|dx|,|dz|)^2, which is monotone in the same way.
static inline int64_t dmet(Pos a, Pos b, int metric) {
    if (metric != MET_CHEBYSHEV) return d2(a, b);
    int64_t dx = (int64_t)a.x - b.x, dz = (int64_t)a.z - b.z;
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    int64_t m = dx > dz ? dx : dz;
    return m * m;
}

// Count structure instances of one condition's type whose position is within
// `reach` of `centre`. If `g` is non-NULL each candidate is biome-viability
// checked (pass 2); if NULL only geometry is counted (pass 1, an over-admit).
// The centroid of the counted instances is written to `*centroid` when found.
static int placementOk(int mc, uint32_t gflags, uint64_t worldSeed, int stype, Pos p);
static int hasPlacementRule(int mc, int stype);
static int generatesAt(const Query *q, int stype, uint64_t s48, uint64_t worldSeed,
                       Generator *g, Pos p);

static int countInstances(const Query *q, int k, uint64_t s48, uint64_t worldSeed,
                          Pos centre, int reach, Generator *g, Pos *centroid)
{
    const Cond *c = &q->cond[k];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
    double span = sc.regionSize * 16.0;
    int r0x = (int)floor((centre.x - reach) / span);
    int r1x = (int)floor((centre.x + reach) / span);
    int r0z = (int)floor((centre.z - reach) / span);
    int r1z = (int)floor((centre.z + reach) / span);
    int64_t lim = (int64_t)reach * reach;
    int cnt = 0; int64_t sx = 0, sz = 0;
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos p;
        if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
        if (dmet(p, centre, c->metric) > lim) continue;
        if (!generatesAt(q, c->structType, s48, worldSeed, g, p)) continue;
        cnt++; sx += p.x; sz += p.z;
    }
    if (cnt > 0 && centroid) { centroid->x = (int)(sx / cnt); centroid->z = (int)(sz / cnt); }
    return cnt;
}

// Find the largest TIGHT cluster of a structure: instances that all fall within
// `spread` of a common member, with that member within `reach` of `centre`.
// Geometry-only when g==NULL (pass 1); viability-checked otherwise (pass 2).
// Returns the best member count found and writes that anchor member's position
// to *anchor -- reporting a real instance (not a centroid) makes the result
// exactly re-checkable: count viable instances within `spread` of it.
#define TIGHT_MAX_DIM 64
static int tightCluster(const Query *q, int k, uint64_t s48, uint64_t worldSeed,
                        Pos centre, int reach, Generator *g, Pos *anchor)
{
    const Cond *c = &q->cond[k];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
    double span = sc.regionSize * 16.0;
    int wr = (int)ceil(c->spread / span);          // neighbour reach, in regions
    if (wr < 1) wr = 1;
    // Region window: the core (anchors within reach of centre) plus a wr-region
    // skirt so edge anchors can see every neighbour. Clamp to a fixed size so
    // the per-thread grid is bounded regardless of `within`.
    int g0x = (int)floor((centre.x - reach) / span) - wr;
    int g0z = (int)floor((centre.z - reach) / span) - wr;
    int W = (int)floor((centre.x + reach) / span) + wr - g0x + 1;
    int H = (int)floor((centre.z + reach) / span) + wr - g0z + 1;
    if (W > TIGHT_MAX_DIM) W = TIGHT_MAX_DIM;
    if (H > TIGHT_MAX_DIM) H = TIGHT_MAX_DIM;

    static _Thread_local Pos  grid[TIGHT_MAX_DIM * TIGHT_MAX_DIM];
    static _Thread_local unsigned char has[TIGHT_MAX_DIM * TIGHT_MAX_DIM];
    for (int j = 0; j < H; j++)
    for (int i = 0; i < W; i++) {
        int idx = j * W + i; has[idx] = 0;
        Pos p;
        if (!getStructurePos(c->structType, q->mc, s48, g0x + i, g0z + j, &p)) continue;
        if (!generatesAt(q, c->structType, s48, worldSeed, g, p)) continue;
        grid[idx] = p; has[idx] = 1;
    }

    int64_t spr2 = (int64_t)c->spread * c->spread;
    int64_t reach2 = (int64_t)reach * reach;
    int best = 0;
    for (int j = wr; j < H - wr; j++)          // core anchors only
    for (int i = wr; i < W - wr; i++) {
        int a = j * W + i;
        if (!has[a] || dmet(grid[a], centre, c->metric) > reach2) continue;
        int cnt = 0;
        for (int dj = -wr; dj <= wr; dj++)
        for (int di = -wr; di <= wr; di++) {
            int b = (j + dj) * W + (i + di);
            if (has[b] && d2(grid[a], grid[b]) <= spr2) cnt++;
        }
        if (cnt > best) { best = cnt; if (anchor) *anchor = grid[a]; }
    }
    return best;
}

// Find a pair of instances -- one of structType, one of structType2 -- whose
// nominal footprints (see structHalfExtent), widened by `pad`, intersect. The
// first structure must be within `reach` of `centre`; the second is wherever it
// falls. Geometry-only when g == NULL (pass 1), viability-checked otherwise.
// Writes the best (largest-intersection) pair found and returns its area.
// A stronghold's footprint is its PIECES, and nothing else here works that way.
//
// Every other structure in this file is a point plus a nominal half-extent, so
// "do these two overlap" is a box test on two positions. A stronghold is up to
// 112 blocks of corridor in every direction from its start, most of it empty
// ground: testing it as a 224-block box would report an overlap for virtually
// any structure in the neighbourhood, which is exactly the kind of
// confident-but-wrong answer this project keeps having to undo.
//
// So the pieces are assembled and the partner is tested against each piece box.
// That also makes minRoofY meaningful: the piece meeting the partner carries
// its own Y, so "the stronghold breaks into the mansion" becomes a real
// question about a real block range instead of an inference from x/z proximity.
#define SH_MAXPIECE 1024
static _Thread_local Piece g_shPieces[SH_MAXPIECE];

// Ground level over one stronghold piece, for the overlap's depth test.
//
// min_roof_y alone cannot answer "is it EXPOSED", and the difference is not
// academic: a search for roof >= 55 inside a village returned strongholds at
// depth 7-13, and the one that was actually walked to in game was under four
// blocks of dirt. A bare Y is meaningless without the ground over it, which is
// the same reason the standalone stronghold condition compares against terrain
// rather than a threshold. This mirrors that code exactly.
//
// The SurfaceNoise is thread-local and re-seeded only when the world seed
// changes, so a seed that never reaches an overlapping piece never pays for it.
static _Thread_local SurfaceNoise g_shSN;
static _Thread_local uint64_t g_shSNSeed = 0;
static _Thread_local int g_shSNValid = 0;

static int shSurfaceY(const Query *q, uint64_t worldSeed, Generator *g, int x, int z)
{
    int ok = 0;
    int y = terrainSurfaceY(q->mc, worldSeed, queryGenFlags(q), x, z, &ok);
    if (ok) return y;
    // Below 1.18 there is no block-exact terrain -- and below 1.18 is precisely
    // where exposed strongholds live, so this fallback carries the feature.
    // mapApproxHeight is a SMOOTHED 1:4 estimate: it cannot see a sharp dip, so
    // it reads a few blocks high exactly where a stronghold breaks out.
    if (!g_shSNValid || g_shSNSeed != worldSeed) {
        initSurfaceNoise(&g_shSN, DIM_OVERWORLD, worldSeed);
        g_shSNSeed = worldSeed; g_shSNValid = 1;
    }
    float fy = 0;
    mapApproxHeight(&fy, NULL, g, &g_shSN, x >> 2, z >> 2, 1, 1);
    return (int)fy;
}

// May the ring walk stop? Strongholds come in rings around the ORIGIN, walked
// outward, and sh.dist is the next one's distance from the origin (chunks).
// Once that is past every point within `reach` of `centre` -- plus the margin
// a ring's spread needs -- no later stronghold can be in range. The cutoff used
// to ignore the centre, so a search at "of":"20000,0" stopped at the inner
// rings and never reached the ones that were actually near it.
static int shPastLast(const StrongholdIter *sh, Pos centre, int reach)
{
    double cdist = sqrt((double)centre.x * centre.x + (double)centre.z * centre.z);
    return sh->dist * 16.0 > cdist + (double)reach * 2.0 + 2048.0;
}

static int strongholdOverlapFind(const Query *q, int k, uint64_t s48,
                                 uint64_t worldSeed, Pos centre, int reach,
                                 Generator *g, Pos *aOut, Pos *bOut)
{
    const Cond *c = &q->cond[k];
    int shFirst = (c->structType == Stronghold);
    int other = shFirst ? c->structType2 : c->structType;
    StructureConfig so;
    if (!getStructureConfig(other, q->mc, &so)) return 0;
    int hb = structHalfExtent(other) + c->overlapPad;
    int64_t lim = (int64_t)reach * reach;
    double spanB = so.regionSize * 16.0;

    // PASS 1 has no biome generator and nextStronghold cannot run without one.
    // Filter on the partner alone: a mansion is far rarer than a stronghold, so
    // "is there a mansion in range at all" already discards nearly every seed,
    // and pass 2 does the real work only on what survives. The nominal 1 is
    // safe because pass 2 recomputes the area from scratch.
    if (!g) {
        int r0x = (int)floor((centre.x - reach) / spanB);
        int r1x = (int)floor((centre.x + reach) / spanB);
        int r0z = (int)floor((centre.z - reach) / spanB);
        int r1z = (int)floor((centre.z + reach) / spanB);
        for (int rx = r0x; rx <= r1x; rx++)
        for (int rz = r0z; rz <= r1z; rz++) {
            Pos b;
            if (!getStructurePos(other, q->mc, s48, rx, rz, &b)) continue;
            if (dmet(b, centre, c->metric) > lim) continue;
            if (!placementAllowed(q->mc, s48, other, b)) continue;
            if (aOut) *aOut = b;
            if (bOut) *bOut = b;
            return 1;
        }
        return 0;
    }

    StrongholdIter sh;
    initFirstStronghold(&sh, q->mc, worldSeed);
    int best = 0;
    while (nextStronghold(&sh, g) > 0) {
        // sh.pos is the CURRENT stronghold; sh.dist is where the NEXT one will
        // be (cubiomes' own header says so). Breaking on sh.dist before testing
        // sh.pos therefore threw away a stronghold that was in range, whenever
        // the next ring happened to clear the cutoff -- so the test is done
        // first and the break moved after it. The break is only an
        // optimisation; there are 128 strongholds in a world, so walking a few
        // extra costs nothing and missing one is a wrong answer.
        int pastLast = shPastLast(&sh, centre, reach);
        if (dmet(sh.pos, centre, c->metric) > lim) { if (pastLast) break; continue; }

        int span = 112 + hb;
        int b0x = (int)floor((sh.pos.x - span) / spanB);
        int b1x = (int)floor((sh.pos.x + span) / spanB);
        int b0z = (int)floor((sh.pos.z - span) / spanB);
        int b1z = (int)floor((sh.pos.z + span) / spanB);
        int cnt = -1;
        for (int qx = b0x; qx <= b1x; qx++)
        for (int qz = b0z; qz <= b1z; qz++) {
            Pos b;
            if (!getStructurePos(other, q->mc, s48, qx, qz, &b)) continue;
            if (abs(b.x - sh.pos.x) > span || abs(b.z - sh.pos.z) > span) continue;
            if (!generatesAt(q, other, s48, worldSeed, g, b)) continue;

            // Only now is assembling the stronghold worth it, and the pieces are
            // the same for every partner, so this runs at most once per site.
            if (cnt < 0)
                cnt = getStrongholdPiecesOpt(g_shPieces, SH_MAXPIECE, q->mc,
                                             worldSeed, sh.pos.x >> 4,
                                             sh.pos.z >> 4, 1);
            if (cnt <= 0) continue;

            for (int i = 0; i < cnt; i++) {
                const Piece *pc = &g_shPieces[i];
                int lox = pc->bb0.x > b.x - hb ? pc->bb0.x : b.x - hb;
                int hix = pc->bb1.x < b.x + hb ? pc->bb1.x : b.x + hb;
                int loz = pc->bb0.z > b.z - hb ? pc->bb0.z : b.z - hb;
                int hiz = pc->bb1.z < b.z + hb ? pc->bb1.z : b.z + hb;
                int ox = hix - lox + 1, oz = hiz - loz + 1;
                if (ox <= 0 || oz <= 0) continue;
                // The roof test applies to the piece that actually meets the
                // partner -- "the part of the stronghold inside the mansion
                // reaches this high" -- not to the stronghold's overall top,
                // which may be a corridor a hundred blocks away.
                if (c->hasRoofMin && pc->bb1.y < c->minRoofY) continue;
                // max_depth: how much ground is over THAT piece. Tested on the
                // same piece as the roof, and last because it is the only part
                // of this loop that touches terrain.
                if (c->hasMaxDepth) {
                    int px = (pc->bb0.x + pc->bb1.x) >> 1;
                    int pz = (pc->bb0.z + pc->bb1.z) >> 1;
                    int psurf = shSurfaceY(q, worldSeed, g, px, pz);
                    if (c->landOnly && psurf < SEA_LEVEL_Y) continue;
                    if (psurf - pc->bb1.y > c->maxDepth) continue;
                }
                int area = ox * oz;
                if (area <= best) continue;
                best = area;
                if (aOut) *aOut = shFirst ? sh.pos : b;
                if (bOut) *bOut = shFirst ? b : sh.pos;
            }
        }
    }
    return best;
}

static int overlapFind(const Query *q, int k, uint64_t s48, uint64_t worldSeed,
                       Pos centre, int reach,
                       Generator *g, Pos *aOut, Pos *bOut)
{
    const Cond *c = &q->cond[k];
    if (c->structType == Stronghold || c->structType2 == Stronghold)
        return strongholdOverlapFind(q, k, s48, worldSeed, centre, reach, g,
                                     aOut, bOut);
    StructureConfig sa, sb;
    if (!getStructureConfig(c->structType,  q->mc, &sa)) return 0;
    if (!getStructureConfig(c->structType2, q->mc, &sb)) return 0;
    int ha = structHalfExtent(c->structType);
    int hb = structHalfExtent(c->structType2);
    int span = ha + hb + c->overlapPad;      // max centre separation that touches

    double spanA = sa.regionSize * 16.0, spanB = sb.regionSize * 16.0;
    int r0x = (int)floor((centre.x - reach) / spanA);
    int r1x = (int)floor((centre.x + reach) / spanA);
    int r0z = (int)floor((centre.z - reach) / spanA);
    int r1z = (int)floor((centre.z + reach) / spanA);
    int64_t lim = (int64_t)reach * reach;
    int best = 0;

    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos a;
        if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &a)) continue;
        if (dmet(a, centre, c->metric) > lim) continue;
        if (!generatesAt(q, c->structType, s48, worldSeed, g, a)) continue;

        // Only the regions that could hold a second structure close enough.
        int b0x = (int)floor((a.x - span) / spanB), b1x = (int)floor((a.x + span) / spanB);
        int b0z = (int)floor((a.z - span) / spanB), b1z = (int)floor((a.z + span) / spanB);
        for (int qx = b0x; qx <= b1x; qx++)
        for (int qz = b0z; qz <= b1z; qz++) {
            Pos b;
            if (!getStructurePos(c->structType2, q->mc, s48, qx, qz, &b)) continue;
            // Same type on both sides ("two villages in each other") must still
            // be two different instances.
            if (c->structType == c->structType2 && b.x == a.x && b.z == a.z) continue;
            int ox = span - abs(b.x - a.x);
            int oz = span - abs(b.z - a.z);
            if (ox <= 0 || oz <= 0) continue;
            int area = ox * oz;
            if (area <= best) continue;
            if (!generatesAt(q, c->structType2, s48, worldSeed, g, b)) continue;
            best = area;
            if (aOut) *aOut = a;
            if (bOut) *bOut = b;
        }
    }
    return best;
}

// --------------------------------------------- 1.18+ surface placement checks
//
// cubiomes' isViableStructurePos models the biome rules but not the terrain
// ones, and from 1.18 three structures also refuse to generate on ground that
// is too low. Its own README says so; the effect is reported structures that do
// not exist in game. These are the real rules, read out of the shipped 1.21
// classes rather than inferred:
//
//   SinglePieceStructure.findGenerationPoint (desert pyramid 21x21,
//   jungle temple 12x15):  getLowestY(ctx, w, d) < seaLevel -> no structure.
//   getLowestY takes the WORLD_SURFACE_WG height at four corners of the chunk's
//   min block plus (0|w, 0|d) and returns the minimum.
//
//   WoodlandMansionStructure.findGenerationPoint:
//   getLowestYIn5by5BoxOffset7Blocks(ctx, rot).getY() < 60 -> no mansion.
//   The box is 5x5 anchored at chunk min + 7, its sign flipped per rotation,
//   and the rotation is the first draw of the chunk's structure RNG.
//
// WATER, and why this errs one way. Minecraft's WORLD_SURFACE_WG counts water,
// so a submerged corner reads as the water surface; we use solid ground. For
// the two sea-level checks that makes no difference to the verdict: a submerged
// corner has water at 62, which fails ">= 63" exactly as the seabed below it
// does. For the mansion's lower bar of 60 it can differ, and we take the
// conservative side -- we may reject a mansion vanilla would allow beside
// water, never accept one it would refuse.
static int lowestCorner(int mc, uint32_t gflags, uint64_t ws, int x0, int z0, int w, int d, int *out)
{
    int lo = INT_MAX;
    for (int i = 0; i < 4; i++) {
        int ok = 0;
        int y = terrainSurfaceY(mc, ws, gflags, x0 + ((i & 1) ? w : 0),
                                z0 + ((i & 2) ? d : 0), &ok);
        if (!ok) return 0;
        if (y < lo) lo = y;
    }
    *out = lo;
    return 1;
}

static int placementOk(int mc, uint32_t gflags, uint64_t worldSeed, int stype, Pos p)
{
    if (mc < MC_1_18) return 1;      // the rule arrived with the new terrain
    int x0 = p.x & ~15, z0 = p.z & ~15, lo;
    switch (stype) {
    case Desert_Pyramid:
        if (!lowestCorner(mc, gflags, worldSeed, x0, z0, 21, 21, &lo)) return 1;
        return lo >= SEA_LEVEL;
    case Jungle_Pyramid:
        if (!lowestCorner(mc, gflags, worldSeed, x0, z0, 12, 15, &lo)) return 1;
        return lo >= SEA_LEVEL;
    case Mansion: {
        // Rotation first: nextInt(4) off the chunk's structure RNG, the same
        // draw the game makes before it measures the ground.
        uint64_t rnd = chunkGenerateRnd(worldSeed, p.x >> 4, p.z >> 4);
        int rot = nextInt(&rnd, 4);          // 0 none, 1 cw90, 2 cw180, 3 ccw90
        int w = (rot == 1 || rot == 2) ? -5 : 5;
        int d = (rot == 2 || rot == 3) ? -5 : 5;
        if (!lowestCorner(mc, gflags, worldSeed, x0 + 7, z0 + 7, w, d, &lo)) return 1;
        return lo >= 60;
    }
    default:
        return 1;
    }
}

// Does this structure type pay for a terrain check on this version?
static int hasPlacementRule(int mc, int stype)
{
    return mc >= MC_1_18 && (stype == Desert_Pyramid || stype == Jungle_Pyramid ||
                             stype == Mansion);
}

// ------------------------------------------------- placement exclusion zones
//
// Some structures refuse to generate near another KIND of structure, and
// cubiomes does not model it. From Minecraft's own placement data
// (data/minecraft/worldgen/structure_set/pillager_outposts.json):
//
//     "exclusion_zone": { "chunk_count": 10, "other_set": "minecraft:villages" }
//
// and ExclusionZone.isPlacementForbidden -> hasStructureChunkInRange scans the
// square of chunks within +-10 and asks whether any of them is a village
// PLACEMENT chunk. Placement, not viability: a village that would fail its own
// biome check still blocks the outpost. So this is pure 48-bit geometry and
// belongs in pass 1, where it costs a couple of getStructurePos calls and kills
// the candidate before any biome work.
//
// Consequence for the user: an outpost and a village can never be closer than
// 11 chunks, which is what makes "an outpost inside a village" impossible
// rather than merely rare.
static int outpostBlockedByVillage(int mc, uint64_t s48, Pos p)
{
    StructureConfig vc;
    if (!getStructureConfig(Village, mc, &vc)) return 0;
    int cx = p.x >> 4, cz = p.z >> 4;
    double span = vc.regionSize * 16.0;
    // Village regions that could hold a placement chunk inside the square.
    int r0x = (int)floor((double)((cx - OUTPOST_VILLAGE_CHUNKS) * 16) / span);
    int r1x = (int)floor((double)((cx + OUTPOST_VILLAGE_CHUNKS) * 16) / span);
    int r0z = (int)floor((double)((cz - OUTPOST_VILLAGE_CHUNKS) * 16) / span);
    int r1z = (int)floor((double)((cz + OUTPOST_VILLAGE_CHUNKS) * 16) / span);
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos v;
        if (!getStructurePos(Village, mc, s48, rx, rz, &v)) continue;
        int vcx = v.x >> 4, vcz = v.z >> 4;
        if (abs(vcx - cx) <= OUTPOST_VILLAGE_CHUNKS &&
            abs(vcz - cz) <= OUTPOST_VILLAGE_CHUNKS)
            return 1;                       // the game would refuse this outpost
    }
    return 0;
}

// Is this candidate position allowed by the placement rules cubiomes omits?
// 48-bit only, so it is usable from either pass.
static int placementAllowed(int mc, uint64_t s48, int stype, Pos p)
{
    if (stype == Outpost && mc >= MC_1_14)
        return !outpostBlockedByVillage(mc, s48, p);
    return 1;
}

// Does this candidate instance really generate? Every loop over candidate
// instances goes through here, so the rules cubiomes leaves out (the outpost
// exclusion zone, the 1.18 terrain floor) cannot be applied in one copy of the
// loop and forgotten in another. They were: the loot and overlap loops skipped
// both, so a loot hit could sit in a desert pyramid the game never builds, and
// an outpost the game refuses still rolled a chest. With g == NULL (pass 1)
// only the 48-bit part applies; pass 2 adds biome viability, then terrain, in
// cost order.
static int generatesAt(const Query *q, int stype, uint64_t s48, uint64_t worldSeed,
                       Generator *g, Pos p)
{
    if (!placementAllowed(q->mc, s48, stype, p)) return 0;
    if (!g) return 1;
    if (!isViableStructurePos(stype, g, p.x, p.z, 0)) return 0;
    return placementOk(q->mc, queryGenFlags(q), worldSeed, stype, p);
}

int queryStage1(const Query *q, uint64_t s48, Match *m)
{
    // Zero it: callers declare Match on the stack, and haveSpawn/haveEyes must
    // start false or pass 2 will short-circuit on garbage and report junk.
    memset(m, 0, sizeof(*m));
    // Where each condition's pass-2 position can land: within slack[k] of
    // anc[k]. Only read for conditions whose position pass 2 moves.
    Pos anc[MAX_COND]; int slack[MAX_COND];
    for (int i = 0; i < q->ngeom; i++) {
        int k = q->geom[i];
        const Cond *c = &q->cond[k];
        // Pass 1 centres. Origin and spawn both collapse to (0,0) here -- spawn
        // deliberately, since it sits near origin and SPAWN_MARGIN widens the
        // reach so pass 1 over-admits and pass 2 refines. PARENT_AT must NOT
        // collapse: its centre can be a million blocks away, and folding it to
        // origin made an explicit position silently search the middle of the
        // world instead.
        Pos centre = (c->parent >= 0)          ? m->pos[c->parent]
                   : (c->parent == PARENT_AT)  ? (Pos){c->atX, c->atZ}
                   : (Pos){0,0};
        // Pass 1 cannot know the world spawn (it is 64-bit and biome-derived),
        // so a spawn-relative condition is filtered against origin widened by
        // SPAWN_MARGIN. Over-admits; pass 2 then checks it exactly.
        int reach = c->within + (c->parent == PARENT_SPAWN ? SPAWN_MARGIN : 0);
        // Same reasoning one level down. If the parent's position moves in
        // pass 2, the child will be measured from wherever it lands -- which
        // is anywhere within the parent's own slack. Filter against that whole
        // area, not the pass-1 guess, or seeds whose true parent has the child
        // nearby are thrown away here and never reach pass 2.
        if (c->parent >= 0 && posFromPass2(q, c->parent)) {
            centre = anc[c->parent];
            reach += slack[c->parent];
        }
        anc[k] = centre; slack[k] = reach;

        // Footprint collision: a rare pairing, so filtering it here on pure
        // geometry kills the overwhelming majority of seeds before any biome
        // work happens. Pass 2 re-runs it with viability checks.
        if (c->type == CT_OVERLAP) {
            Pos a, b;
            int area = overlapFind(q, k, s48, 0, centre, reach, NULL, &a, &b);
            if (area <= 0) return 0;
            m->pos[k] = a; m->partner[k] = b; m->overlapArea[k] = area;
            continue;
        }

        // Cluster: at least structMin instances within reach. Pass 1 counts
        // geometry only (no biome generator yet); pass 2 re-counts viability.
        if (c->structMin > 1) {
            Pos cen;
            int got = (c->spread > 0)
                ? tightCluster(q, k, s48, 0, centre, reach, NULL, &cen)
                : countInstances(q, k, s48, 0, centre, reach, NULL, &cen);
            if (got < c->structMin) return 0;
            m->pos[k] = cen;
            continue;
        }

        StructureConfig sc;
        if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
        double span = sc.regionSize * 16.0;
        int r0x = (int)floor((centre.x - reach) / span);
        int r1x = (int)floor((centre.x + reach) / span);
        int r0z = (int)floor((centre.z - reach) / span);
        int r1z = (int)floor((centre.z + reach) / span);

        int64_t lim = (int64_t)reach * reach;
        int64_t best = INT64_MAX; Pos bp = {0,0}; int found = 0;
        for (int rx = r0x; rx <= r1x; rx++)
        for (int rz = r0z; rz <= r1z; rz++) {
            Pos p;
            if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
            int64_t d = dmet(p, centre, c->metric);
            if (d > lim) continue;
            // Distinctness: two conditions of the same structure type must
            // match two DIFFERENT instances. Without this, "swamp_hut within
            // 140 of swamp_hut" is satisfied by the same hut at distance 0,
            // and multi-instance constellations (quad huts) never filter.
            if (!placementAllowed(q->mc, s48, c->structType, p)) continue;
            int taken = 0;
            for (int j = 0; j < i && !taken; j++) {
                int o = q->geom[j];
                if (q->cond[o].structType == c->structType &&
                    m->pos[o].x == p.x && m->pos[o].z == p.z) taken = 1;
            }
            if (taken) continue;
            if (d < best) { best = d; bp = p; found = 1; }
        }
        if (!found) return 0;      // reject: cheap, and kills 65536 world seeds
        m->pos[k] = bp;
    }
    return 1;
}

static int stage2Dim(const Query *q, Generator *g, int dim, uint64_t worldSeed,
                     const Match *m, Match *fixed, int *haveSpawn, Pos *spawn, LootCache *lc);

int queryStage2(const Query *q, Generator *g, uint64_t worldSeed, Match *m, LootCache *lc)
{
    // Outer loop over dimensions: applySeed (~25 us) is the dominant per-seed
    // cost, so pay it once per dimension rather than once per condition. A
    // single-dimension query -- the common case -- pays exactly one.
    Match fixed = *m;
    int haveSpawn = 0; Pos spawn = {0,0};
    for (int d = 0; d < q->ndims; d++) {
        int dim = q->dims[d];
        applySeed(g, dim, worldSeed);
        if (!stage2Dim(q, g, dim, worldSeed, m, &fixed, &haveSpawn, &spawn, lc)) return 0;
    }
    fixed.spawn = spawn; fixed.haveSpawn = haveSpawn;
    *m = fixed;   // report the positions actually verified
    return 1;
}

// Re-find a structure match around an arbitrary centre. Used in pass 2 to
// redo spawn-relative conditions against the true world spawn.
static int refind(const Query *q, int k, uint64_t s48, Pos centre,
                  const Match *taken, Pos *out)
{
    const Cond *c = &q->cond[k];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
    double span = sc.regionSize * 16.0;
    int r0x = (int)floor((centre.x - c->within) / span);
    int r1x = (int)floor((centre.x + c->within) / span);
    int r0z = (int)floor((centre.z - c->within) / span);
    int r1z = (int)floor((centre.z + c->within) / span);
    int64_t lim = (int64_t)c->within * c->within, best = INT64_MAX;
    int found = 0;
    for (int rx = r0x; rx <= r1x; rx++)
    for (int rz = r0z; rz <= r1z; rz++) {
        Pos p;
        if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
        int64_t d = dmet(p, centre, c->metric);
        if (d > lim || d >= best) continue;
        if (!placementAllowed(q->mc, s48, c->structType, p)) continue;
        // Same distinctness rule pass 1 applies: two conditions of the same
        // structure type must resolve to two different instances.
        int clash = 0;
        for (int j = 0; j < q->n && !clash; j++) {
            if (j == k || q->cond[j].structType != c->structType) continue;
            if (q->cond[j].type != CT_STRUCTURE) continue;
            if (taken->pos[j].x == p.x && taken->pos[j].z == p.z) clash = 1;
        }
        if (clash) continue;
        best = d; *out = p; found = 1;
    }
    return found;
}

// Locate the first stronghold and count its end portal's filled frames.
// ~10 ms: nextStronghold's biome checks dominate. Cached per seed in `fixed`
// so several eye conditions (or a later report) pay for it once.
static int resolveEyes(const Query *q, Generator *g, uint64_t worldSeed, Match *fixed)
{
    if (fixed->haveEyes) return 1;
    StrongholdIter sh;
    initFirstStronghold(&sh, q->mc, worldSeed);
    if (!nextStronghold(&sh, g)) return 0;

    StructureSaltConfig ssconf;
    if (!getStructureSaltConfig(Stronghold, q->mc, -1, &ssconf)) return 0;

    Piece pieces[512];
    int n = getStrongholdLoot(pieces, 512, ssconf, q->mc, worldSeed,
                              sh.pos.x >> 4, sh.pos.z >> 4);
    for (int i = 0; i < n; i++) {
        if (pieces[i].type != SH_PORTAL_ROOM) continue;
        int bits = pieces[i].additionalData, eyes = 0;
        for (int b = 0; b < EYE_FRAMES; b++) if (bits & (1 << b)) eyes++;
        fixed->stronghold = sh.pos;
        fixed->eyes = eyes;
        fixed->haveEyes = 1;
        return 1;
    }
    return 0;   // no portal room among the generated pieces
}

// The point a condition's radius is measured from. Origin is (0,0); spawn is
// resolved lazily and cached. A parent's position is read from `fixed`, which
// starts as a copy of pass 1's match and is overwritten whenever pass 2
// produces or moves a position -- so it is always the parent as finally
// verified. The planner runs every such parent first (posFromPass2).
static Pos condCentre(const Query *q, int k, const Match *m, Match *fixed,
                      int *haveSpawn, Pos *spawn, Generator *g)
{
    (void)m;
    const Cond *c = &q->cond[k];
    if (c->parent == PARENT_SPAWN) {
        if (!*haveSpawn) { *spawn = getSpawn(g); *haveSpawn = 1; }
        return *spawn;
    }
    if (c->parent == PARENT_ORIGIN) return (Pos){0, 0};
    if (c->parent == PARENT_AT)     return (Pos){c->atX, c->atZ};
    return fixed->pos[c->parent];
}

static int stage2Dim(const Query *q, Generator *g, int dim, uint64_t worldSeed,
                     const Match *m, Match *fixed, int *haveSpawn, Pos *spawn, LootCache *lc)
{
    uint64_t s48 = worldSeed & ((1ULL << 48) - 1);
    SurfaceNoise sn; int haveSN = 0;   // ore counting needs it; init once per dim
    for (int i = 0; i < q->nviab; i++) {
        int k = q->viab[i];
        const Cond *c = &q->cond[k];
        if (c->dim != dim) continue;   // handled in another dimension's pass

        if (c->type == CT_EYES) {
            if (!resolveEyes(q, g, worldSeed, fixed)) return 0;
            if (fixed->eyes < c->eyesMin) return 0;
            continue;
        }

        if (c->type == CT_STRONGHOLD) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // How far below the surface the stronghold's roof sits.
            //
            // A bare Y threshold cannot answer this: a roof at y=60 is a metre
            // under the grass on a plain and seventy metres inside a mountain,
            // and only one of those is the find people mean. So the roof is
            // compared against the terrain height AT the stronghold.
            //
            // Block-exact terrain exists only from 1.18, and 1.18 is precisely
            // where strongholds stopped coming near the surface -- so the
            // versions this question is about are the ones without it.
            // mapApproxHeight covers them; checked against real server blocks it
            // landed within one block at both sites (63 vs 63, 65 vs ~66).
            if (q->mc < MC_1_18 && !haveSN) {
                initSurfaceNoise(&sn, dim, worldSeed); haveSN = 1;
            }
            int64_t lim = (int64_t)c->within * c->within;
            StrongholdIter sh;
            initFirstStronghold(&sh, q->mc, worldSeed);
            int hit = 0;
            while (nextStronghold(&sh, g) > 0) {
                // Same off-by-one-ring as the overlap above: sh.dist describes
                // the NEXT stronghold, sh.pos the current one. A stronghold 81
                // blocks from spawn was reported as no match at within=112 and
                // as a match at within=300, purely because the cutoff moved.
                int pastLast = shPastLast(&sh, centre, c->within);
                if (dmet(sh.pos, centre, c->metric) > lim) { if (pastLast) break; continue; }

                int cnt = getStrongholdPiecesOpt(g_shPieces, SH_MAXPIECE, q->mc,
                                                 worldSeed, sh.pos.x >> 4,
                                                 sh.pos.z >> 4, 1);
                if (cnt <= 0) continue;

                // Depth is measured PER PIECE, against the ground over that
                // piece.
                //
                // Comparing the stronghold's highest piece against the surface
                // at its start chunk is apples to oranges: a stronghold sprawls
                // 112 blocks, so the two can be a hundred blocks and forty
                // metres of hillside apart. A seed checked this way predicted a
                // roof of 66 where the blocks near the start topped out at 63 --
                // the tall piece was simply somewhere else. The question "does
                // any part of it break the surface" is per piece, and the answer
                // has to name WHICH part, or the coordinate sends you to a spot
                // with nothing at it.
                int bestDepth = 0x7fffffff, bestRoof = 0, bestSurf = 0;
                Pos bestAt = sh.pos;
                for (int i = 0; i < cnt; i++) {
                    const Piece *pc = &g_shPieces[i];
                    // The roof bound is a property of the piece that is the
                    // find. Testing it only on whichever piece came out
                    // shallowest rejected strongholds where a different piece
                    // met both bounds. Cheap, so before the terrain lookup.
                    if (c->hasRoofMin && pc->bb1.y < c->minRoofY) continue;
                    int px = (pc->bb0.x + pc->bb1.x) >> 1;
                    int pz = (pc->bb0.z + pc->bb1.z) >> 1;
                    int ok = 0;
                    int psurf = terrainSurfaceY(q->mc, worldSeed, queryGenFlags(q),
                                                px, pz, &ok);
                    if (!ok) {
                        float fy = 0;
                        mapApproxHeight(&fy, NULL, g, &sn, px >> 2, pz >> 2, 1, 1);
                        psurf = (int)fy;
                    }
                    if (c->landOnly && psurf < SEA_LEVEL_Y) continue;
                    int d = psurf - pc->bb1.y;
                    if (d < bestDepth) {
                        bestDepth = d; bestRoof = pc->bb1.y;
                        bestSurf = psurf; bestAt = (Pos){px, pz};
                    }
                }
                if (bestDepth == 0x7fffffff) continue;   // no piece qualified
                if (c->hasMaxDepth && bestDepth > c->maxDepth) continue;
                (void)bestSurf;

                fixed->pos[k] = sh.pos;
                // The shallowest piece -- where you actually go. Stored in the
                // pair slot, which a stronghold condition never otherwise uses.
                fixed->partner[k] = bestAt;
                fixed->shRoof[k] = bestRoof;
                fixed->shDepth[k] = bestDepth;
                hit = 1;
                break;
            }
            if (!hit) return 0;
            continue;
        }

        if (c->type == CT_OVERLAP) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // Re-run the pair search with the biome generator: pass 1 only knew
            // the two structures would be ATTEMPTED there, not that either
            // actually generates. A pair where one half fails viability is not
            // an overlap at all.
            Pos a, b;
            int area = overlapFind(q, k, s48, worldSeed, centre, c->within, g, &a, &b);
            if (area <= 0) return 0;
            fixed->pos[k] = a;
            fixed->partner[k] = b;
            fixed->overlapArea[k] = area;
            continue;
        }

        if (c->type == CT_CACTUS) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            Cactus best;
            int tall = cactusTallest(g, q->mc, queryGenFlags(q), worldSeed,
                                     centre.x, centre.z, c->within, &best);
            if (tall < c->cactusMin) return 0;
            fixed->pos[k] = (Pos){best.x, best.z};
            fixed->cactusTall[k] = tall;
            fixed->cactusBase[k] = best.baseY;
            continue;
        }

        if (c->type == CT_ORE) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            if (!haveSN) { initSurfaceNoise(&sn, dim, worldSeed); haveSN = 1; }
            int nm; const OreMaterial *tab = oreMaterials(&nm);
            int vein = 0;
            // Vein connectivity costs a sort plus a flood fill, so only compute
            // it when something asks: a threshold, or a leaderboard on it.
            int wantVein = c->veinMin > 0 ||
                           (q->rankIdx == k && rankMetric(q) == RANK_VEIN);
            int cnt = oreScan(g, &sn, q->mc, queryGenFlags(q), &tab[c->oreMat],
                              centre.x, centre.z, c->within,
                              c->oreExposed, wantVein ? &vein : NULL,
                              c->oreYMin, c->oreYMax);
            if (cnt < c->oreMin) return 0;
            if (c->veinMin > 0 && vein < c->veinMin) return 0;
            fixed->pos[k] = centre;
            fixed->oreCount[k] = cnt;
            fixed->veinMax[k] = vein;
            continue;
        }

        if (c->type == CT_SLIME) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // Count slime chunks in the disc. isSlimeChunk uses the full world
            // seed (a per-chunk Java-RNG check), so this is a cheap pass-2 test.
            int64_t lim = (int64_t)c->within * c->within;
            int c0x = (centre.x - c->within) >> 4, c1x = (centre.x + c->within) >> 4;
            int c0z = (centre.z - c->within) >> 4, c1z = (centre.z + c->within) >> 4;
            int cnt = 0;
            for (int chx = c0x; chx <= c1x; chx++)
            for (int chz = c0z; chz <= c1z; chz++) {
                int bx = (chx << 4) + 8 - centre.x, bz = (chz << 4) + 8 - centre.z;
                if ((int64_t)bx*bx + (int64_t)bz*bz > lim) continue;
                if (isSlimeChunk(worldSeed, chx, chz)) cnt++;
            }
            if (cnt < c->slimeMin) return 0;
            fixed->pos[k] = centre;
            fixed->slimeCount[k] = cnt;
            continue;
        }

        if (c->type == CT_ISLAND) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // The reference must be LAND, and ocean must cover >= pct% of the
            // disc: a small island in open water. Same surface probe as biomes.
            int cid = chunkBiome(g, centre.x, centre.z);
            if (isOceanic(cid)) return 0;                 // spawn itself is water
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            int total = 0, ocean = 0;
            for (int dx = -c->within; dx <= c->within; dx += step)
            for (int dz = -c->within; dz <= c->within; dz += step) {
                if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
                int bx = centre.x + dx, bz = centre.z + dz;
                total++;
                if (isOceanic(chunkBiome(g, bx, bz))) ocean++;
            }
            if (total <= 0 || (int64_t)ocean * 100 < (int64_t)c->areaPct * total)
                return 0;
            fixed->pos[k] = centre;
            fixed->areaCells[k] = ocean;
            fixed->areaTotal[k] = total;
            continue;
        }

        if (c->type == CT_BIOME_AREA) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // Sample the disc on a scanStep lattice and take the fraction of
            // sample points that are the target biome. Same surface probe as a
            // plain biome check (y=319>>2 -- a y=63 probe lands in cave biomes).
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            int total = 0, match = 0;
            for (int dx = -c->within; dx <= c->within; dx += step)
            for (int dz = -c->within; dz <= c->within; dz += step) {
                if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
                int bx = centre.x + dx, bz = centre.z + dz;
                total++;   // counted before the gate: it is the denominator
                int qx = (bx>>4)*4 + 2, qz = (bz>>4)*4 + 2;
                if (!climateMayHold(q, worldSeed, k, qx, qz)) continue;
                int id = surfaceBiomeQ(g, qx, qz);
                if (c->biomeIsSet ? c->biomeSet[id & 0xff] : id == c->biomeId) match++;
            }
            // Integer test without floating point: match/total >= pct/100.
            if (total <= 0 || (int64_t)match * 100 < (int64_t)c->areaPct * total)
                return 0;
            fixed->pos[k] = centre;
            fixed->areaCells[k] = match;
            fixed->areaTotal[k] = total;
            continue;
        }

        if (c->type == CT_HEIGHT) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            if (c->exactTerrain) {
                // Real block-level terrain under the footprint -- the true cliff
                // signal. Only reachable for survivors of every cheaper filter
                // (the planner sorts this dead last), so the ~ms cost is paid rarely.
                int peak, relief;
                if (!exactFootprint(q->mc, queryGenFlags(q), worldSeed, centre, c->within, &peak, &relief))
                    return 0;
                if (peak < c->heightMin) return 0;
                if (c->reliefMin > 0 && relief < c->reliefMin) return 0;
                fixed->pos[k] = centre;
                fixed->peakHeight[k] = peak;
                fixed->peakDrop[k] = relief;
                continue;
            }
            // mapApproxHeight needs SurfaceNoise only between Beta 1.8 and 1.18;
            // 1.18+ derives height from the biome noise directly. Reuse the ore
            // pass's sn (inited once per dim).
            SurfaceNoise *snp = NULL;
            if (q->mc < MC_1_18) {
                if (!haveSN) { initSurfaceNoise(&sn, dim, worldSeed); haveSN = 1; }
                snp = &sn;
            }
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            int peak = -64; Pos peakPos = centre; int found = 0;
            int valley = 4096;   // track the lowest surface too, for relief
            for (int dx = -c->within; dx <= c->within; dx += step)
            for (int dz = -c->within; dz <= c->within; dz += step) {
                if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
                int bx = centre.x + dx, bz = centre.z + dz;
                float y = 0;
                mapApproxHeight(&y, NULL, g, snp, bx >> 2, bz >> 2, 1, 1);
                int h = (int)y;
                found = 1;
                if (h > peak) { peak = h; peakPos = (Pos){bx, bz}; }
                if (h < valley) valley = h;
            }
            if (!found || peak < c->heightMin) return 0;
            if (c->reliefMin > 0 && (peak - valley) < c->reliefMin) return 0;
            fixed->pos[k] = peakPos;
            fixed->peakHeight[k] = peak;
            fixed->peakDrop[k] = found ? (peak - valley) : 0;
            continue;
        }

        if (c->type == CT_LOOT) {
            // Scan every instance in range that really generates and roll its
            // loot; the FIRST one meeting the count wins. Checking only the
            // nearest would reject seeds where a farther instance holds the
            // item. A leaderboard ranked on this condition is the exception: it
            // wants the BEST chest in range, so it rolls them all.
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            int wantMax = (q->rankIdx == k);
            StructureConfig sc;
            if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
            double span = sc.regionSize * 16.0;
            int r0x = (int)floor((centre.x - c->within) / span);
            int r1x = (int)floor((centre.x + c->within) / span);
            int r0z = (int)floor((centre.z - c->within) / span);
            int r1z = (int)floor((centre.z + c->within) / span);
            int64_t lim = (int64_t)c->within * c->within;
            int hit = 0, bestCnt = -1;
            for (int rx = r0x; rx <= r1x && !(hit && !wantMax); rx++)
            for (int rz = r0z; rz <= r1z && !(hit && !wantMax); rz++) {
                Pos p;
                if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
                if (dmet(p, centre, c->metric) > lim) continue;
                if (!generatesAt(q, c->structType, s48, worldSeed, g, p)) continue;
                StructureVariant sv;
                int biome = chunkBiome(g, p.x, p.z);
                getVariant(&sv, c->structType, q->mc, worldSeed, p.x, p.z, biome);
                // The FULL world seed: 1.18+ chest seeds depend on all 64 bits.
                int cnt = lootCountItem(lc, q->mc, worldSeed, c->structType,
                                        p.x, p.z, &sv, c->lootItem);
                if (cnt >= c->lootMin && cnt > bestCnt) {
                    bestCnt = cnt;
                    fixed->pos[k] = p;
                    fixed->lootCount[k] = cnt;
                    hit = 1;
                }
            }
            if (!hit) return 0;
            continue;
        }

        // Cluster: re-count viable instances within the radius of the true
        // reference (spawn resolved here) and require at least structMin. Pass 1
        // only counted geometry, so this is where the biome check happens.
        if (c->type == CT_STRUCTURE && c->structMin > 1) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            Pos cen; int cnt = (c->spread > 0)
                ? tightCluster(q, k, s48, worldSeed, centre, c->within, g, &cen)
                : countInstances(q, k, s48, worldSeed, centre, c->within, g, &cen);
            if (cnt < c->structMin) return 0;
            fixed->pos[k] = cen;
            fixed->structCount[k] = cnt;
            continue;
        }

        // Spawn-relative, or measured from a parent pass 2 moved: pass 1 only
        // guaranteed this is near where the parent USED to be. Re-find it
        // around the true reference now that the biome generator exists.
        if (c->type == CT_STRUCTURE && !isPopulationFeature(c->structType) &&
            (c->parent == PARENT_SPAWN ||
             (c->parent >= 0 && posFromPass2(q, c->parent)))) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            Pos p;
            if (!refind(q, k, s48, centre, fixed, &p)) return 0;
            fixed->pos[k] = p;
        }

        // Population-seeded feature: pass 1 could not place it, so find it here
        // with the full world seed. Nearest instance to the reference wins.
        if (c->type == CT_STRUCTURE && isPopulationFeature(c->structType)) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            StructureConfig sc;
            if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
            double span = sc.regionSize * 16.0;
            int r0x = (int)floor((centre.x - c->within) / span);
            int r1x = (int)floor((centre.x + c->within) / span);
            int r0z = (int)floor((centre.z - c->within) / span);
            int r1z = (int)floor((centre.z + c->within) / span);
            int64_t lim = (int64_t)c->within * c->within, best = INT64_MAX;
            Pos bp = {0,0}; int found = 0; int bestSize = 0;
            for (int rx = r0x; rx <= r1x; rx++)
            for (int rz = r0z; rz <= r1z; rz++) {
                Pos p;
                if (!getStructurePos(c->structType, q->mc, worldSeed, rx, rz, &p)) continue;
                int64_t d = dmet(p, centre, c->metric);
                if (d > lim || d >= best) continue;
                if (!isViableStructurePos(c->structType, g, p.x, p.z, 0)) continue;
                if (c->structType == Geode &&
                    (c->geodeSize > 0 || c->reqCracked != 0 ||
                     (q->rankIdx == k && rankMetric(q) == RANK_SIZE))) {
                    StructureVariant sv;
                    int biome = chunkBiome(g, p.x, p.z);
                    if (!getVariant(&sv, Geode, q->mc, worldSeed, p.x, p.z, biome)) continue;
                    if (c->reqCracked > 0 && !sv.cracked) continue;
                    if (c->reqCracked < 0 &&  sv.cracked) continue;
                    if (sv.size < c->geodeSize) continue;
                    bestSize = sv.size;
                }
                best = d; bp = p; found = 1;
            }
            if (!found) return 0;
            fixed->pos[k] = bp;
            fixed->geodeSize[k] = bestSize;
            continue;
        }

        if (c->type == CT_STRUCTURE) {
            Pos p = fixed->pos[k];     // pass 1's, or re-found just above
            if (!isViableStructurePos(c->structType, g, p.x, p.z, 0))
                return 0;
            // Terrain rules the engine does not model (1.18+). Runs after the
            // biome check because it is orders of magnitude more expensive.
            if (!placementOk(q->mc, queryGenFlags(q), worldSeed, c->structType, p))
                return 0;
            if (c->surfaceOnly || c->reqAbandoned || c->reqBasement || c->reqGiant) {
                // Variant checks. Each reads the exact RNG decision the game
                // makes (getVariant), so these are exact filters, not guesses:
                // a buried portal is a 50/50 coin flip; a zombie village, an
                // igloo basement, and a giant portal are likewise fixed draws.
                StructureVariant sv;
                int biome = chunkBiome(g, p.x, p.z);
                getVariant(&sv, c->structType, q->mc, worldSeed, p.x, p.z, biome);
                if (c->surfaceOnly  && sv.underground) return 0;
                if (c->reqAbandoned && !sv.abandoned)  return 0;
                if (c->reqBasement  && !sv.basement)   return 0;
                if (c->reqGiant     && !sv.giant)      return 0;
            }
            if (c->reqExposed) {
                // The buried-treasure chest sits at chunk-local (9,9) on the solid
                // surface. If that surface is at/above sea level it's a dry-land
                // (exposed/reachable) treasure; below = submerged. Exact terrain
                // on 1.18+, the smoothed estimate on older versions.
                int cx = (p.x & ~15) + 9, cz = (p.z & ~15) + 9, surf;
                if (q->mc >= MC_1_18) {
                    int rel;
                    if (!exactFootprint(q->mc, queryGenFlags(q), worldSeed, (Pos){cx, cz}, 0, &surf, &rel))
                        return 0;
                } else {
                    if (!haveSN) { initSurfaceNoise(&sn, dim, worldSeed); haveSN = 1; }
                    float y = 0;
                    mapApproxHeight(&y, NULL, g, &sn, cx >> 2, cz >> 2, 1, 1);
                    surf = (int)y;
                }
                // Dry means the top block is at or above the waterline: water
                // fills y<=62, so a top block AT 62 has air over it. Comparing
                // against 63 rejected exactly the beach chests at the waterline.
                if (surf < SEA_LEVEL_Y) return 0;
            }
            if (c->caveBelow > 0) {
                // Sample a few columns across the footprint, not just the
                // anchor: a village is 60+ blocks wide and the cavern that
                // makes it interesting rarely sits exactly under one corner.
                static const int OFF[5][2] =
                    {{0,0},{16,16},{-16,-16},{16,-16},{-16,16}};
                int best = 0, top = 0;
                for (int i = 0; i < 5; i++) {
                    int t = 0;
                    int v = terrainVoidBelow(q->mc, worldSeed, queryGenFlags(q),
                                             p.x + OFF[i][0], p.z + OFF[i][1],
                                             CAVE_SCAN_DEPTH, &t);
                    if (v > best) { best = v; top = t; }
                }
                if (best < c->caveBelow) return 0;
                fixed->caveHeight[k] = best;
                (void)top;
            }
            if (c->floatVoid > 0) {
                // Same five-point sample as cave_below, for the same reason: a
                // structure is wide and the interesting geometry rarely sits
                // exactly under its anchor. The BEST of the five decides.
                static const int FOFF[5][2] =
                    {{0,0},{16,16},{-16,-16},{16,-16},{-16,16}};
                int bestv = 0, bestcap = 0; Pos bestat = p;
                for (int i = 0; i < 5; i++) {
                    int cap = 0;
                    int v = terrainFloating(q->mc, worldSeed, queryGenFlags(q),
                                            p.x + FOFF[i][0], p.z + FOFF[i][1],
                                            c->floatVoid, c->floatRing,
                                            c->floatNeed, &cap);
                    if (v > bestv) {
                        bestv = v; bestcap = cap;
                        // WHICH offset matched matters: the island can sit 22
                        // blocks from the anchor, and reporting only the anchor
                        // sends you to a column that is not floating at all.
                        bestat.x = p.x + FOFF[i][0];
                        bestat.z = p.z + FOFF[i][1];
                    }
                }
                if (bestv < c->floatVoid) return 0;
                fixed->caveHeight[k] = bestv;
                fixed->floatCap[k] = bestcap;
                fixed->floatAt[k] = bestat;
            }
            if (c->reqShip) {
                // Enumerate the end city's jigsaw and require an END_SHIP piece
                // (the guaranteed-elytra ship). Exact -- reads the real assembly.
                Piece pieces[END_CITY_PIECES_MAX];
                int np = getEndCityPieces(pieces, worldSeed, p.x >> 4, p.z >> 4);
                int hasShip = 0;
                for (int pi = 0; pi < np; pi++)
                    if (pieces[pi].type == END_SHIP) { hasShip = 1; break; }
                if (!hasShip) return 0;
            }
            fixed->pos[k] = p;
        } else {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            // Sample points, at 1:4 ("quart") biome resolution. "fast" and
            // "fine" take chunk centres -- every 4th chunk, or every chunk --
            // which is where structure viability samples too; "exact" takes
            // every quart. The DISTANCE is tested on the point actually
            // sampled. It used to be tested on a lattice point and the biome
            // read at that point's chunk centre, up to ~11 blocks further out,
            // so "within 16" could match at 24; and "exact"'s 4-block lattice
            // landed 16 times on the same chunk centre, paying 16x for
            // nothing.
            int qstep = step >= 16 ? (step / 16) * 4 : 1;   // in quarts
            int qoff  = step >= 16 ? 2 : 0;                 // chunk-centre quart
            int q0x = (int)floor((centre.x - c->within) / (step >= 16 ? 16.0 : 4.0));
            int q0z = (int)floor((centre.z - c->within) / (step >= 16 ? 16.0 : 4.0));
            int q1x = (int)floor((centre.x + c->within) / (step >= 16 ? 16.0 : 4.0));
            int q1z = (int)floor((centre.z + c->within) / (step >= 16 ? 16.0 : 4.0));
            if (step >= 16) { q0x *= 4; q0z *= 4; q1x *= 4; q1z *= 4; }
            // A leaf biome only needs presence, so stop at the first hit. A
            // biome that is a PARENT records the match nearest its own centre --
            // the representative point a child ("biome B within D of A") is then
            // measured from, so a large biome's far corner doesn't anchor it.
            int hit = 0; Pos best = centre; int64_t bestd = lim + 1;
            for (int qxb = q0x; qxb <= q1x; qxb += qstep) {
                for (int qzb = q0z; qzb <= q1z; qzb += qstep) {
                    int qx = qxb + qoff, qz = qzb + qoff;
                    // The block at the centre of the sampled quart.
                    int bx = qx * 4 + 2, bz = qz * 4 + 2;
                    int64_t dx = bx - centre.x, dz = bz - centre.z;
                    int64_t d = dx*dx + dz*dz;
                    if (d > lim) continue;
                    // surface sampling: biomes are 3D since 1.18; a y=63 probe
                    // lands in cave biomes. 319>>2 is what viability checks use.
                    // Temperature first: a third of the cost, and where it says
                    // no the full six-noise lookup cannot say yes.
                    if (!climateMayHold(q, worldSeed, k, qx, qz)) continue;
                    int id = surfaceBiomeQ(g, qx, qz);
                    if (!(c->biomeIsSet ? c->biomeSet[id & 0xff] : id == c->biomeId)) continue;
                    hit = 1;
                    if (d < bestd) { bestd = d; best = (Pos){bx, bz}; }
                    if (!c->isParent) break;   // presence-only: first hit is enough
                }
                if (hit && !c->isParent) break;
            }
            if (!hit) return 0;
            // The sampled point itself, so checkbiome at this position reads
            // the same quart and re-verifies it exactly.
            fixed->pos[k] = best;
        }
    }
    return 1;
}
