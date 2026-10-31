// See anyof.h for why OR is compiled away rather than evaluated.
#include "anyof.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static int one(const char *json, Query **out, char *err, size_t errlen)
{
    Query *q = malloc(sizeof *q);
    if (!q) { snprintf(err, errlen, "out of memory"); return 0; }
    if (queryParse(q, json, err, errlen)) { free(q); return 0; }
    if (queryPlan(q, err, errlen)) { free(q); return -1; }
    *out = q;
    return 1;
}

// Is `id` a member of any group? Members exist only in some alternatives, so a
// reference to one could not be resolved in the others.
static int isMember(const cJSON *conds, const char *id)
{
    const cJSON *e, *m;
    cJSON_ArrayForEach(e, conds) {
        const cJSON *jgid = cJSON_GetObjectItem(e, "id");
        const cJSON *any = cJSON_GetObjectItem(e, "any");
        if (!any) continue;
        if (cJSON_IsString(jgid) && !strcmp(jgid->valuestring, id)) return 1;
        cJSON_ArrayForEach(m, any) {
            const cJSON *jid = cJSON_GetObjectItem(m, "id");
            if (cJSON_IsString(jid) && !strcmp(jid->valuestring, id)) return 1;
        }
    }
    return 0;
}

static int checkRef(const cJSON *conds, const cJSON *c, char *err, size_t errlen)
{
    const cJSON *of = cJSON_GetObjectItem(c, "of");
    if (cJSON_IsString(of) && isMember(conds, of->valuestring)) {
        snprintf(err, errlen, "\"%s\" is inside an \"any\" group, so nothing can be "
                 "measured from it: it only exists in some alternatives", of->valuestring);
        return 1;
    }
    return 0;
}

// Give a member with no id one of its own: an id only matters to the output,
// and a model writing a group has no reason to think of it.
static void nameMembers(cJSON *group, const char *gid)
{
    int i = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, group) {
        if (!cJSON_HasObjectItem(m, "id")) {
            char id[ID_LEN];
            snprintf(id, sizeof id, "%.16s_%d", gid, i + 1);
            cJSON_AddStringToObject(m, "id", id);
        }
        i++;
    }
}

// Checks every group and returns the number of alternatives, or 0 with err set.
static int checkGroups(cJSON *root, cJSON *conds, char *err, size_t errlen)
{
    int nalt = 1, g = 0;
    cJSON *e, *m;
    cJSON_ArrayForEach(e, conds) {
        cJSON *any = cJSON_GetObjectItem(e, "any");
        if (!any) continue;
        g++;
        for (const cJSON *k = e->child; k; k = k->next)
            if (strcmp(k->string, "any") && strcmp(k->string, "id")) {
                snprintf(err, errlen, "an \"any\" group holds only \"id\" and \"any\"; "
                         "put \"%s\" on the conditions inside it", k->string);
                return 0;
            }
        int n = cJSON_IsArray(any) ? cJSON_GetArraySize(any) : 0;
        if (n < 1) {
            snprintf(err, errlen, "\"any\" must be a non-empty list of conditions");
            return 0;
        }
        cJSON_ArrayForEach(m, any) {
            if (!cJSON_IsObject(m) || cJSON_HasObjectItem(m, "any")) {
                snprintf(err, errlen, "\"any\" groups cannot be nested");
                return 0;
            }
        }
        if (nalt > MAX_ALT / n) {
            snprintf(err, errlen, "too many alternatives: \"any\" groups multiply, "
                     "and one search takes at most %d combinations", MAX_ALT);
            return 0;
        }
        nalt *= n;
        const cJSON *jgid = cJSON_GetObjectItem(e, "id");
        char gid[ID_LEN];
        if (cJSON_IsString(jgid)) snprintf(gid, sizeof gid, "%s", jgid->valuestring);
        else snprintf(gid, sizeof gid, "any%d", g);
        nameMembers(any, gid);
    }
    if (g && cJSON_HasObjectItem(root, "rank")) {
        snprintf(err, errlen, "a leaderboard ranks one condition, so it cannot use "
                 "an \"any\" group");
        return 0;
    }
    cJSON_ArrayForEach(e, conds) {
        cJSON *any = cJSON_GetObjectItem(e, "any");
        if (!any) { if (checkRef(conds, e, err, errlen)) return 0; continue; }
        cJSON_ArrayForEach(m, any)
            if (checkRef(conds, m, err, errlen)) return 0;
    }
    return nalt;
}

// Alternative k as ordinary query JSON: each group replaced by one member, the
// choice read off k's digits in the mixed radix of the group sizes.
static char *alternative(const cJSON *root, const cJSON *conds, int k)
{
    cJSON *copy = cJSON_Duplicate(root, 1);
    cJSON *list = cJSON_CreateArray();
    const cJSON *e;
    cJSON_ArrayForEach(e, conds) {
        const cJSON *any = cJSON_GetObjectItem(e, "any");
        if (!any) { cJSON_AddItemToArray(list, cJSON_Duplicate(e, 1)); continue; }
        int n = cJSON_GetArraySize(any);
        cJSON_AddItemToArray(list, cJSON_Duplicate(cJSON_GetArrayItem(any, k % n), 1));
        k /= n;
    }
    cJSON_ReplaceItemInObject(copy, "conditions", list);
    char *text = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return text;
}

int queryCompile(const char *json, Query **out, char *err, size_t errlen)
{
    *out = NULL;
    cJSON *root = cJSON_Parse(json);
    cJSON *conds = root ? cJSON_GetObjectItem(root, "conditions") : NULL;
    int groups = 0;
    const cJSON *e;
    if (cJSON_IsArray(conds))
        cJSON_ArrayForEach(e, conds) if (cJSON_HasObjectItem(e, "any")) groups++;
    if (!groups) {
        // Every query without a group, and every malformed one, which the real
        // parser explains far better than this file could.
        cJSON_Delete(root);
        return one(json, out, err, errlen);
    }

    int rc = 0;
    int nalt = checkGroups(root, conds, err, errlen);
    Query *plans = nalt ? malloc(nalt * sizeof *plans) : NULL;
    if (nalt && !plans) snprintf(err, errlen, "out of memory");
    if (!plans) { cJSON_Delete(root); return 0; }

    int kept = 0, dropped = 0;
    char note[256] = "", planErr[256] = "";
    for (int k = 0; k < nalt; k++) {
        char *text = alternative(root, conds, k);
        int bad = queryParse(&plans[kept], text, err, errlen);
        cJSON_free(text);
        if (bad) { free(plans); cJSON_Delete(root); return 0; }
        char why[256];
        if (queryPlan(&plans[kept], why, sizeof why)) {
            if (!dropped) {
                snprintf(planErr, sizeof planErr, "%s", why);
                snprintf(note, sizeof note, "an alternative can never match and was "
                         "left out: %s", why);
            }
            dropped++;
            continue;
        }
        kept++;
    }
    cJSON_Delete(root);
    if (!kept) {
        snprintf(err, errlen, "%s", planErr);
        free(plans);
        return -1;
    }
    if (dropped) {
        size_t len = strlen(plans[0].warn);
        snprintf(plans[0].warn + len, sizeof plans[0].warn - len, "%s%s",
                 len ? "; " : "", note);
    }
    rc = kept;
    *out = plans;
    return rc;
}
