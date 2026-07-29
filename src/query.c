#include "query.h"
#include "util.h"
#include "cJSON.h"
#include "features/stronghold.h"
#include "terrain.h"
#include "loot.h"
#include "ore.h"
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
#define NS_LOOT       47000.0   // structure viability + a ~5us loot roll
#define NS_ORE_CHUNK   4000.0   // per chunk: one biome probe + config scan + gen
#define NS_SLIME_CHUNK    6.0   // per chunk: one Java-RNG isSlimeChunk call
#define NS_TERRAIN_CELL 200000.0 // exact terrain: one heavy noise column per 4x4
#define SEA_LEVEL          63    // overworld sea level: at/above = dry land

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
    if (!getStructureConfig(c->structType, q->mc, &sc)) return NS_STRUCT_POS;
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
    if (c->type == CT_OVERLAP)   // both halves of the pair must be viable
        return 2.0 * NS_VIABLE + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
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
    default:              return 16;
    }
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

        cJSON *je = cJSON_GetObjectItem(e, "eyes");
        cJSON *jl = cJSON_GetObjectItem(e, "loot");
        cJSON *js = cJSON_GetObjectItem(e, "structure");
        cJSON *jb = cJSON_GetObjectItem(e, "biome");
        cJSON *jore = cJSON_GetObjectItem(e, "ore");
        cJSON *jslime = cJSON_GetObjectItem(e, "slime");
        cJSON *jarea = cJSON_GetObjectItem(e, "biome_area");
        cJSON *jheight = cJSON_GetObjectItem(e, "height");
        cJSON *jrelief = cJSON_GetObjectItem(e, "relief");
        cJSON *jisland = cJSON_GetObjectItem(e, "island");
        cJSON *jovl = cJSON_GetObjectItem(e, "overlap");
        if (jovl && cJSON_IsArray(jovl) && cJSON_GetArraySize(jovl) == 2) {
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
            c->structType  = str2struct_(an);
            c->structType2 = str2struct_(bn);
            if (c->structType < 0 || c->structType2 < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown structure in overlap", c->id);
                goto done;
            }
            StructureConfig sa, sb;
            if (!getStructureConfig(c->structType, q->mc, &sa) ||
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
                    "(try desert_pyramid, jungle_temple, igloo, outpost, shipwreck, ruined_portal)",
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
            c->biomeId = str2biome_(q->mc, jb->valuestring);
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
            c->biomeId = str2biome_(q->mc, jarea->valuestring);
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
            snprintf(err, errlen, "condition \"%s\": need \"structure\" or \"biome\"", c->id);
            goto done;
        }
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

int queryPlan(Query *q, char *err, size_t errlen)
{
    // resolve parents
    for (int i = 0; i < q->n; i++) {
        Cond *c = &q->cond[i];
        if (!strcmp(c->ofId, "origin")) { c->parent = PARENT_ORIGIN; continue; }
        if (!strcmp(c->ofId, "spawn"))  { c->parent = PARENT_SPAWN;  continue; }
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

    // any structure condition left unscheduled means a dependency cycle
    for (int i = 0; i < q->n; i++)
        if ((q->cond[i].type == CT_STRUCTURE || q->cond[i].type == CT_LOOT
             || q->cond[i].type == CT_OVERLAP) && !done[i]) {
            snprintf(err, errlen, "dependency cycle involving \"%s\"", q->cond[i].id);
            return 1;
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

    // Pass 2 order: everything biome-dependent, cheapest first. Structure
    // positions are fixed by pass 1, so most conditions have no ordering
    // constraint. The exception is a condition measured from a BIOME parent:
    // that parent's position is produced here in pass 2, so it must run first.
    // Schedule like pass 1 -- cheapest currently-schedulable condition each
    // round -- honouring only the biome-parent edge.
    int doneV[MAX_COND] = {0};
    q->nviab = 0;
    for (int iter = 0; iter < q->n + 1 && q->nviab < q->n; iter++) {
        int best = -1; double bestc = 1e30;
        for (int i = 0; i < q->n; i++) {
            if (doneV[i]) continue;
            int p = q->cond[i].parent;
            // Only a biome parent is produced in pass 2; wait for it.
            if (p >= 0 && q->cond[p].type == CT_BIOME && !doneV[p]) continue;
            double c = viabCost(q, i);
            if (c < bestc) { bestc = c; best = i; }
        }
        if (best < 0) break;
        doneV[best] = 1;
        q->viab[q->nviab++] = best;
    }
    // Unscheduled leftovers mean a biome-parent cycle (A of B, B of A).
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
    if (c->type == CT_OVERLAP) {
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
    fprintf(f, "  est. pass-1 cost: %.0f ns/seed\n\n", q->est_cost_ns);
}

// ---------------------------------------------------------------- evaluation

static inline int64_t d2(Pos a, Pos b) {
    int64_t dx = (int64_t)a.x - b.x, dz = (int64_t)a.z - b.z;
    return dx*dx + dz*dz;
}

// Count structure instances of one condition's type whose position is within
// `reach` of `centre`. If `g` is non-NULL each candidate is biome-viability
// checked (pass 2); if NULL only geometry is counted (pass 1, an over-admit).
// The centroid of the counted instances is written to `*centroid` when found.
static int placementOk(int mc, uint32_t gflags, uint64_t worldSeed, int stype, Pos p);
static int hasPlacementRule(int mc, int stype);

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
        if (d2(p, centre) > lim) continue;
        if (!placementAllowed(q->mc, s48, c->structType, p)) continue;
        if (g && !isViableStructurePos(c->structType, g, p.x, p.z, 0)) continue;
        if (g && !placementOk(q->mc, queryGenFlags(q), worldSeed, c->structType, p)) continue;
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
        if (!placementAllowed(q->mc, s48, c->structType, p)) continue;
        if (g && !isViableStructurePos(c->structType, g, p.x, p.z, 0)) continue;
        if (g && !placementOk(q->mc, queryGenFlags(q), worldSeed, c->structType, p)) continue;
        grid[idx] = p; has[idx] = 1;
    }

    int64_t spr2 = (int64_t)c->spread * c->spread;
    int64_t reach2 = (int64_t)reach * reach;
    int best = 0;
    for (int j = wr; j < H - wr; j++)          // core anchors only
    for (int i = wr; i < W - wr; i++) {
        int a = j * W + i;
        if (!has[a] || d2(grid[a], centre) > reach2) continue;
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
static int overlapFind(const Query *q, int k, uint64_t s48, Pos centre, int reach,
                       Generator *g, Pos *aOut, Pos *bOut)
{
    const Cond *c = &q->cond[k];
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
        if (d2(a, centre) > lim) continue;
        if (!placementAllowed(q->mc, s48, c->structType, a)) continue;
        if (g && !isViableStructurePos(c->structType, g, a.x, a.z, 0)) continue;

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
            if (!placementAllowed(q->mc, s48, c->structType2, b)) continue;
            if (g && !isViableStructurePos(c->structType2, g, b.x, b.z, 0)) continue;
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

int queryStage1(const Query *q, uint64_t s48, Match *m)
{
    // Zero it: callers declare Match on the stack, and haveSpawn/haveEyes must
    // start false or pass 2 will short-circuit on garbage and report junk.
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < q->ngeom; i++) {
        int k = q->geom[i];
        const Cond *c = &q->cond[k];
        Pos centre = (c->parent >= 0) ? m->pos[c->parent] : (Pos){0,0};
        // Pass 1 cannot know the world spawn (it is 64-bit and biome-derived),
        // so a spawn-relative condition is filtered against origin widened by
        // SPAWN_MARGIN. Over-admits; pass 2 then checks it exactly.
        int reach = c->within + (c->parent == PARENT_SPAWN ? SPAWN_MARGIN : 0);

        // Footprint collision: a rare pairing, so filtering it here on pure
        // geometry kills the overwhelming majority of seeds before any biome
        // work happens. Pass 2 re-runs it with viability checks.
        if (c->type == CT_OVERLAP) {
            Pos a, b;
            int area = overlapFind(q, k, s48, centre, reach, NULL, &a, &b);
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
            int64_t d = d2(p, centre);
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
        int64_t d = d2(p, centre);
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

// The point a condition's radius is measured from. A structure parent's
// position comes from pass 1 (m->pos, or fixed->pos when it was spawn-relative
// and refined in pass 2); a BIOME parent's position is produced in pass 2
// (fixed->pos) -- this is what makes "biome B within D of biome A" work.
// Origin is (0,0); spawn is resolved lazily and cached.
static Pos condCentre(const Query *q, int k, const Match *m, Match *fixed,
                      int *haveSpawn, Pos *spawn, Generator *g)
{
    const Cond *c = &q->cond[k];
    if (c->parent == PARENT_SPAWN) {
        if (!*haveSpawn) { *spawn = getSpawn(g); *haveSpawn = 1; }
        return *spawn;
    }
    if (c->parent == PARENT_ORIGIN) return (Pos){0, 0};
    const Cond *p = &q->cond[c->parent];
    if (p->type == CT_BIOME) return fixed->pos[c->parent];
    return (p->parent == PARENT_SPAWN) ? fixed->pos[c->parent] : m->pos[c->parent];
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

        if (c->type == CT_OVERLAP) {
            Pos centre = condCentre(q, k, m, fixed, haveSpawn, spawn, g);
            // Re-run the pair search with the biome generator: pass 1 only knew
            // the two structures would be ATTEMPTED there, not that either
            // actually generates. A pair where one half fails viability is not
            // an overlap at all.
            Pos a, b;
            int area = overlapFind(q, k, s48, centre, c->within, g, &a, &b);
            if (area <= 0) return 0;
            fixed->pos[k] = a;
            fixed->partner[k] = b;
            fixed->overlapArea[k] = area;
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
                              c->oreExposed, wantVein ? &vein : NULL);
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
            int cid = getBiomeAt(g, 0, (centre.x>>4)*4 + 2, 319 >> 2, (centre.z>>4)*4 + 2);
            if (isOceanic(cid)) return 0;                 // spawn itself is water
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            int total = 0, ocean = 0;
            for (int dx = -c->within; dx <= c->within; dx += step)
            for (int dz = -c->within; dz <= c->within; dz += step) {
                if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
                int bx = centre.x + dx, bz = centre.z + dz;
                total++;
                if (isOceanic(getBiomeAt(g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2))) ocean++;
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
                total++;
                int id = getBiomeAt(g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2);
                if (id == c->biomeId) match++;
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
            // Scan every viable instance in range and roll its loot; the FIRST
            // one meeting the count wins. Checking only the nearest would
            // reject seeds where a farther instance holds the item.
            Pos centre = (c->parent >= 0) ? fixed->pos[c->parent] : (Pos){0,0};
            if (c->parent == PARENT_SPAWN) {
                if (!*haveSpawn) { *spawn = getSpawn(g); *haveSpawn = 1; }
                centre = *spawn;
            }
            StructureConfig sc;
            if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
            double span = sc.regionSize * 16.0;
            int r0x = (int)floor((centre.x - c->within) / span);
            int r1x = (int)floor((centre.x + c->within) / span);
            int r0z = (int)floor((centre.z - c->within) / span);
            int r1z = (int)floor((centre.z + c->within) / span);
            int64_t lim = (int64_t)c->within * c->within;
            int hit = 0;
            for (int rx = r0x; rx <= r1x && !hit; rx++)
            for (int rz = r0z; rz <= r1z && !hit; rz++) {
                Pos p;
                if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
                if (d2(p, centre) > lim) continue;
                if (!isViableStructurePos(c->structType, g, p.x, p.z, 0)) continue;
                StructureVariant sv;
                int biome = getBiomeAt(g, 0, (p.x>>4)*4+2, 319>>2, (p.z>>4)*4+2);
                getVariant(&sv, c->structType, q->mc, s48, p.x, p.z, biome);
                int cnt = lootCountItem(lc, q->mc, s48, c->structType,
                                        p.x, p.z, &sv, c->lootItem);
                if (cnt >= c->lootMin) {
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

        // Spawn-relative: pass 1 only guaranteed this is near the ORIGIN.
        // Resolve it properly now that the biome generator exists.
        if (c->parent == PARENT_SPAWN && c->type == CT_STRUCTURE) {
            if (!*haveSpawn) { *spawn = getSpawn(g); *haveSpawn = 1; }
            Pos p;
            if (!refind(q, k, s48, *spawn, fixed, &p)) return 0;
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
                int64_t d = d2(p, centre);
                if (d > lim || d >= best) continue;
                if (!isViableStructurePos(c->structType, g, p.x, p.z, 0)) continue;
                if (c->structType == Geode &&
                    (c->geodeSize > 0 || c->reqCracked != 0 ||
                     (q->rankIdx == k && rankMetric(q) == RANK_SIZE))) {
                    StructureVariant sv;
                    int biome = getBiomeAt(g, 0, (p.x>>4)*4+2, 319>>2, (p.z>>4)*4+2);
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
            Pos p = (c->parent == PARENT_SPAWN) ? fixed->pos[k] : m->pos[k];
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
                int biome = getBiomeAt(g, 0, (p.x>>4)*4+2, 319>>2, (p.z>>4)*4+2);
                getVariant(&sv, c->structType, q->mc, s48, p.x, p.z, biome);
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
                if (surf < SEA_LEVEL) return 0;
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
            // A leaf biome only needs presence, so stop at the first hit. A
            // biome that is a PARENT records the match nearest its own centre --
            // the representative point a child ("biome B within D of A") is then
            // measured from, so a large biome's far corner doesn't anchor it.
            int hit = 0; Pos best = centre; int64_t bestd = lim + 1;
            for (int dx = -c->within; dx <= c->within; dx += step) {
                for (int dz = -c->within; dz <= c->within; dz += step) {
                    int64_t d = (int64_t)dx*dx + (int64_t)dz*dz;
                    if (d > lim) continue;
                    int bx = centre.x + dx, bz = centre.z + dz;
                    // surface sampling: biomes are 3D since 1.18; a y=63 probe
                    // lands in cave biomes. 319>>2 is what viability checks use.
                    int id = getBiomeAt(g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2);
                    if (id != c->biomeId) continue;
                    hit = 1;
                    if (d < bestd) { bestd = d; best = (Pos){bx, bz}; }
                    if (!c->isParent) break;   // presence-only: first hit is enough
                }
                if (hit && !c->isParent) break;
            }
            if (!hit) return 0;
            fixed->pos[k] = best;   // where the biome was matched (for output/children)
        }
    }
    return 1;
}
