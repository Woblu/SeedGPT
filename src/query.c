#include "query.h"
#include "util.h"
#include "cJSON.h"
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
#define NS_SPAWN    2300000.0   // getSpawn: ~437/s, the most expensive call here

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

        cJSON *js = cJSON_GetObjectItem(e, "structure");
        cJSON *jb = cJSON_GetObjectItem(e, "biome");
        if (js && cJSON_IsString(js)) {
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
            if (done[i] || q->cond[i].type != CT_STRUCTURE) continue;
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
        if (q->cond[i].type == CT_STRUCTURE && !done[i]) {
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
                     const Match *m, Match *fixed, int *haveSpawn, Pos *spawn);

int queryStage2(const Query *q, Generator *g, uint64_t worldSeed, Match *m)
{
    // Outer loop over dimensions: applySeed (~25 us) is the dominant per-seed
    // cost, so pay it once per dimension rather than once per condition. A
    // single-dimension query -- the common case -- pays exactly one.
    Match fixed = *m;
    int haveSpawn = 0; Pos spawn = {0,0};
    for (int d = 0; d < q->ndims; d++) {
        int dim = q->dims[d];
        applySeed(g, dim, worldSeed);
        if (!stage2Dim(q, g, dim, worldSeed, m, &fixed, &haveSpawn, &spawn)) return 0;
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

static int stage2Dim(const Query *q, Generator *g, int dim, uint64_t worldSeed,
                     const Match *m, Match *fixed, int *haveSpawn, Pos *spawn)
{
    uint64_t s48 = worldSeed & ((1ULL << 48) - 1);
    for (int i = 0; i < q->nviab; i++) {
        int k = q->viab[i];
        const Cond *c = &q->cond[k];
        if (c->dim != dim) continue;   // handled in another dimension's pass

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
