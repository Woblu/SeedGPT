#include "query.h"
#include "util.h"
#include "cJSON.h"
#include "features/stronghold.h"
#include "loot.h"
#include "ore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- cost model
//
// Numbers are measured on this machine (i7-10700K, clang -O3), not guessed:
//   getStructurePos                  ~9 ns
//   isViableStructurePos            ~42 us
//   getBiomeAt (seed already applied) ~2.3 us
// applySeed (~25 us) is paid once per world seed, not per condition, so it is
// not attributed to any single condition here.
#define NS_STRUCT_POS    9.0
#define NS_VIABLE     42000.0
#define NS_BIOME_AT    2300.0
#define NS_SPAWN    2300000.0   // getSpawn: ~437/s
#define NS_EYES    10250000.0   // locate stronghold + pieces + loot: ~98/s
#define NS_LOOT       47000.0   // structure viability + a ~5us loot roll
#define NS_ORE_CHUNK   4000.0   // per chunk: one biome probe + config scan + gen
#define NS_SLIME_CHUNK    6.0   // per chunk: one Java-RNG isSlimeChunk call

static double geomCost(const Query *q, int i)
{
    const Cond *c = &q->cond[i];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return NS_STRUCT_POS;
    // regions we must scan to cover a disc of radius `within`
    double span = sc.regionSize * 16.0;
    double reach = c->within + (c->parent == PARENT_SPAWN ? SPAWN_MARGIN : 0);
    double n = 2.0 * (reach / span) + 1.0;
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
        return n * n * NS_ORE_CHUNK + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_SLIME) {
        double n = 2.0 * (c->within / 16.0) + 1.0;   // one RNG call per chunk
        return n * n * NS_SLIME_CHUNK + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    }
    if (c->type == CT_STRUCTURE)
        return NS_VIABLE + (c->parent == PARENT_SPAWN ? NS_SPAWN : 0.0);
    // biome scan: samples on a `scanStep` lattice across the disc
    double step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
    double n = 2.0 * (c->within / step) + 1.0;
    return n * n * NS_BIOME_AT;
}

const char *dimName(int dim)
{
    return dim == DIM_NETHER ? "nether" : dim == DIM_END ? "end" : "overworld";
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
    {"ocean_ruin", Ocean_Ruin},
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
        if (jslime && cJSON_IsNumber(jslime)) {
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
            // Counting is per-chunk over the whole column, so cost scales with
            // area. Default modest; cap so it cannot wedge the search.
            cJSON *ow = cJSON_GetObjectItem(e, "within");
            c->within = (ow && cJSON_IsNumber(ow)) ? ow->valueint : 64;
            if (c->within > 256) c->within = 256;
            if (c->within < 16)  c->within = 16;
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
        } else {
            snprintf(err, errlen, "condition \"%s\": need \"structure\" or \"biome\"", c->id);
            goto done;
        }
    }
    q->n = n;
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

    // Pass 1 order: structure geometry, parents before children (a child's
    // radius is measured from its parent's matched position, so we cannot
    // reorder across that edge no matter what the cost model wants).
    int done[MAX_COND] = {0};
    q->ngeom = 0;
    for (int iter = 0; iter < q->n + 1 && q->ngeom < q->n; iter++) {
        // among currently-schedulable geometry tasks, take the cheapest
        int best = -1; double bestc = 1e30;
        for (int i = 0; i < q->n; i++) {
            if (done[i] || (q->cond[i].type != CT_STRUCTURE
                          && q->cond[i].type != CT_LOOT)) continue;
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
        if ((q->cond[i].type == CT_STRUCTURE || q->cond[i].type == CT_LOOT) && !done[i]) {
            snprintf(err, errlen, "dependency cycle involving \"%s\"", q->cond[i].id);
            return 1;
        }
    // a biome condition may not parent a structure it is measured from unless
    // that structure was placed in pass 1
    for (int i = 0; i < q->n; i++) {
        if (q->cond[i].type != CT_BIOME) continue;
        int p = q->cond[i].parent;
        if (p >= 0 && q->cond[p].type != CT_STRUCTURE) {
            snprintf(err, errlen, "condition \"%s\": parent \"%s\" is not a structure",
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

    // Pass 2 order: everything biome-dependent, cheapest first. No dependency
    // constraints here -- positions are already fixed by pass 1.
    q->nviab = 0;
    for (int i = 0; i < q->n; i++) q->viab[q->nviab++] = i;
    for (int a = 0; a < q->nviab; a++)
        for (int b = a + 1; b < q->nviab; b++)
            if (viabCost(q, q->viab[b]) < viabCost(q, q->viab[a])) {
                int t = q->viab[a]; q->viab[a] = q->viab[b]; q->viab[b] = t;
            }

    q->est_cost_ns = 0;
    for (int i = 0; i < q->ngeom; i++) q->est_cost_ns += geomCost(q, q->geom[i]);
    return 0;
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
        snprintf(buf, n, ">= %d %s ore within %d of %s",
                 c->oreMin, tab[c->oreMat].name, c->within, c->ofId);
        return buf;
    }
    if (c->type == CT_SLIME) {
        snprintf(buf, n, ">= %d slime chunks within %d of %s",
                 c->slimeMin, c->within, c->ofId);
        return buf;
    }
    const char *what = (c->type == CT_STRUCTURE)
        ? struct2str(c->structType) : biome2str(q->mc, c->biomeId);
    const char *prec = c->type != CT_BIOME ? ""
                     : c->scanStep == SCAN_FAST  ? " [fast]"
                     : c->scanStep == SCAN_EXACT ? " [exact]" : " [fine]";
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

        if (c->type == CT_ORE) {
            Pos centre = c->parent >= 0
                ? (q->cond[c->parent].parent == PARENT_SPAWN
                     ? fixed->pos[c->parent] : m->pos[c->parent])
                : (c->parent == PARENT_SPAWN
                     ? (*haveSpawn ? *spawn : (*spawn = getSpawn(g), *haveSpawn = 1, *spawn))
                     : (Pos){0,0});
            if (!haveSN) { initSurfaceNoise(&sn, dim, worldSeed); haveSN = 1; }
            int nm; const OreMaterial *tab = oreMaterials(&nm);
            int cnt = oreCountMaterial(g, &sn, q->mc, &tab[c->oreMat],
                                       centre.x, centre.z, c->within);
            if (cnt < c->oreMin) return 0;
            fixed->pos[k] = centre;
            fixed->oreCount[k] = cnt;
            continue;
        }

        if (c->type == CT_SLIME) {
            Pos centre = c->parent >= 0
                ? (q->cond[c->parent].parent == PARENT_SPAWN
                     ? fixed->pos[c->parent] : m->pos[c->parent])
                : (c->parent == PARENT_SPAWN
                     ? (*haveSpawn ? *spawn : (*spawn = getSpawn(g), *haveSpawn = 1, *spawn))
                     : (Pos){0,0});
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

        // Spawn-relative: pass 1 only guaranteed this is near the ORIGIN.
        // Resolve it properly now that the biome generator exists.
        if (c->parent == PARENT_SPAWN && c->type == CT_STRUCTURE) {
            if (!*haveSpawn) { *spawn = getSpawn(g); *haveSpawn = 1; }
            Pos p;
            if (!refind(q, k, s48, *spawn, fixed, &p)) return 0;
            fixed->pos[k] = p;
        }

        if (c->type == CT_STRUCTURE) {
            Pos p = (c->parent == PARENT_SPAWN) ? fixed->pos[k] : m->pos[k];
            if (!isViableStructurePos(c->structType, g, p.x, p.z, 0))
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
            fixed->pos[k] = p;
        } else {
            Pos centre = c->parent >= 0 ? (q->cond[c->parent].parent == PARENT_SPAWN
                                            ? fixed->pos[c->parent] : m->pos[c->parent])
                       : (c->parent == PARENT_SPAWN
                            ? (*haveSpawn ? *spawn : (*spawn = getSpawn(g), *haveSpawn = 1, *spawn))
                            : (Pos){0,0});
            int64_t lim = (int64_t)c->within * c->within;
            int step = c->scanStep > 0 ? c->scanStep : SCAN_FINE;
            int hit = 0;
            for (int dx = -c->within; dx <= c->within && !hit; dx += step)
            for (int dz = -c->within; dz <= c->within && !hit; dz += step) {
                if ((int64_t)dx*dx + (int64_t)dz*dz > lim) continue;
                int bx = centre.x + dx, bz = centre.z + dz;
                // surface sampling: biomes are 3D since 1.18; a y=63 probe
                // lands in cave biomes. 319>>2 is what viability checks use.
                int id = getBiomeAt(g, 0, (bx>>4)*4 + 2, 319 >> 2, (bz>>4)*4 + 2);
                if (id == c->biomeId) hit = 1;
            }
            if (!hit) return 0;
        }
    }
    return 1;
}
