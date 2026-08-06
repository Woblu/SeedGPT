// lootitems -- dump the items a structure's loot tables can produce, as JSON.
// The UI reads this so the item dropdown only offers things that can appear.
#include "query.h"
#include "loot.h"
#include "finders.h"
#include "util.h"
#include "loot/loot_tables.h"
#include "loot/loot_table_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The loot-table names each supported structure draws from.
static const char *tablesFor(int type, const char **out)
{
    switch (type) {
    case Desert_Pyramid: out[0]="desert_pyramid"; return NULL;
    case Jungle_Pyramid: out[0]="jungle_temple"; out[1]="jungle_temple_dispenser"; return NULL;
    case Igloo:          out[0]="igloo_chest"; return NULL;
    case Outpost:        out[0]="pillager_outpost"; return NULL;
    case Shipwreck:      out[0]="shipwreck_treasure"; out[1]="shipwreck_supply"; out[2]="shipwreck_map"; return NULL;
    case Ruined_Portal:  out[0]="ruined_portal"; return NULL;
    // Fortress corridor-turn chests all draw from one table. Bastions draw from
    // two: the guaranteed starting-piece chests are "bastion_other" for three of
    // the four types and "bastion_bridge" for the bridge type, so the offered
    // items are the union -- what any bastion could yield from a modelled chest.
    case Fortress:       out[0]="nether_bridge"; return NULL;
    case Bastion:        out[0]="bastion_other"; out[1]="bastion_bridge"; return NULL;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int mc = (argc > 1) ? str2mc(argv[1]) : MC_1_21;
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    printf("{\n  \"version\": \"%s\",\n  \"structures\": {", mc2str(mc));
    int firstS = 1;
    for (int i = 0; i < queryStructureCount(); i++) {
        int type = queryStructureType(i);
        if (!lootStructureSupported(type)) continue;
        const char *tables[4] = {0,0,0,0};
        tablesFor(type, tables);

        // Collect the union of item names across this structure's tables.
        char items[256][64]; int ni = 0;
        for (int t = 0; t < 4 && tables[t]; t++) {
            LootTableContext *ctx = NULL;
            if (!init_loot_table_name(&ctx, tables[t], mc) || !ctx) continue;
            for (int k = 0; k < ctx->item_count; k++) {
                const char *nm = ctx->item_names[k];
                if (!nm) continue;
                int j; for (j = 0; j < ni; j++) if (!strcmp(items[j], nm)) break;
                if (j == ni && ni < 256) snprintf(items[ni++], 64, "%s", nm);
            }
        }
        printf("%s\n    \"%s\": [", firstS ? "" : ",", queryStructureName(i));
        firstS = 0;
        for (int j = 0; j < ni; j++) {
            const char *nm = items[j];
            if (!strncmp(nm, "minecraft:", 10)) nm += 10;
            printf("%s\"%s\"", j ? "," : "", nm);
        }
        printf("]");
    }
    printf("\n  }\n}\n");
    return 0;
}
