// vocab -- dump the valid query vocabulary for a version, as JSON.
//
// The NL layer reads this instead of carrying its own hardcoded lists, so the
// model can only ever name a structure/biome the engine actually accepts for
// the requested version. Structures unsupported in the target version are
// filtered out here rather than failing later at parse time.
//
//   usage: vocab [version]      (default 1.21)
#include "query.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *ver = (argc > 1) ? argv[1] : "1.21";
    int mc = str2mc(ver);
    if (mc < 0) { fprintf(stderr, "unknown version %s\n", ver); return 2; }

    printf("{\n  \"version\": \"%s\",\n", mc2str(mc));

    printf("  \"structures\": [");
    int first = 1;
    for (int i = 0; i < queryStructureCount(); i++) {
        StructureConfig sc;
        if (!getStructureConfig(queryStructureType(i), mc, &sc)) continue;  // not in this version
        printf("%s\n    \"%s\"", first ? "" : ",", queryStructureName(i));
        first = 0;
    }
    printf("\n  ],\n");

    printf("  \"biomes\": [");
    first = 1;
    for (int id = 0; id < 256; id++) {
        if (!biomeExists(mc, id)) continue;
        const char *n = biome2str(mc, id);
        if (!n) continue;
        printf("%s\n    \"%s\"", first ? "" : ",", n);
        first = 0;
    }
    printf("\n  ]\n}\n");
    return 0;
}
