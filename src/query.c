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

static double geomCost(const Query *q, int i)
{
    const Cond *c = &q->cond[i];
    StructureConfig sc;
    if (!getStructureConfig(c->structType, q->mc, &sc)) return NS_STRUCT_POS;
    // regions we must scan to cover a disc of radius `within`
    double span = sc.regionSize * 16.0;
    double n = 2.0 * (c->within / span) + 1.0;
    return n * n * NS_STRUCT_POS;
}

static double viabCost(const Query *q, int i)
{
    const Cond *c = &q->cond[i];
    if (c->type == CT_STRUCTURE) return NS_VIABLE;
    // biome scan: samples on a 64-block lattice across the disc
    double n = 2.0 * (c->within / 64.0) + 1.0;
    return n * n * NS_BIOME_AT;
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
    {"mansion", Mansion}, {"village", Village}, {"monument", Monument},
    {"desert_pyramid", Desert_Pyramid}, {"jungle_temple", Jungle_Pyramid},
    {"swamp_hut", Swamp_Hut}, {"igloo", Igloo}, {"shipwreck", Shipwreck},
    {"outpost", Outpost}, {"ancient_city", Ancient_City},
    {"ruined_portal", Ruined_Portal}, {"trail_ruins", Trail_Ruins},
    {"trial_chambers", Trial_Chambers}, {"treasure", Treasure},
    {"ocean_ruin", Ocean_Ruin}, {"end_city", End_City},
};
#define NSTRUCT (int)(sizeof(STRUCT_TBL)/sizeof(STRUCT_TBL[0]))

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
        snprintf(c->ofId, ID_LEN, "%s", (x && cJSON_IsString(x)) ? x->valuestring : "spawn");

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
        } else if (jb && cJSON_IsString(jb)) {
            c->type = CT_BIOME;
            c->biomeId = str2biome_(q->mc, jb->valuestring);
            if (c->biomeId < 0) {
                snprintf(err, errlen, "condition \"%s\": unknown biome \"%s\"", c->id, jb->valuestring);
                goto done;
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
        if (!strcmp(c->ofId, "spawn") || !strcmp(c->ofId, "origin")) { c->parent = -1; continue; }
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
    snprintf(buf, n, "%s within %d of %s", what ? what : "?", c->within, c->ofId);
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
        Pos centre = (c->parent < 0) ? (Pos){0,0} : m->pos[c->parent];

        StructureConfig sc;
        if (!getStructureConfig(c->structType, q->mc, &sc)) return 0;
        double span = sc.regionSize * 16.0;
        int r0x = (int)floor((centre.x - c->within) / span);
        int r1x = (int)floor((centre.x + c->within) / span);
        int r0z = (int)floor((centre.z - c->within) / span);
        int r1z = (int)floor((centre.z + c->within) / span);

        int64_t lim = (int64_t)c->within * c->within;
        int64_t best = INT64_MAX; Pos bp = {0,0}; int found = 0;
        for (int rx = r0x; rx <= r1x; rx++)
        for (int rz = r0z; rz <= r1z; rz++) {
            Pos p;
            if (!getStructurePos(c->structType, q->mc, s48, rx, rz, &p)) continue;
            int64_t d = d2(p, centre);
            if (d > lim) continue;
            if (d < best) { best = d; bp = p; found = 1; }
        }
        if (!found) return 0;      // reject: cheap, and kills 65536 world seeds
        m->pos[k] = bp;
    }
    return 1;
}

int queryStage2(const Query *q, Generator *g, const Match *m)
{
    for (int i = 0; i < q->nviab; i++) {
        int k = q->viab[i];
        const Cond *c = &q->cond[k];

        if (c->type == CT_STRUCTURE) {
            if (!isViableStructurePos(c->structType, g, m->pos[k].x, m->pos[k].z, 0))
                return 0;
        } else {
            Pos centre = (c->parent < 0) ? (Pos){0,0} : m->pos[c->parent];
            int64_t lim = (int64_t)c->within * c->within;
            int hit = 0;
            for (int dx = -c->within; dx <= c->within && !hit; dx += 64)
            for (int dz = -c->within; dz <= c->within && !hit; dz += 64) {
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
