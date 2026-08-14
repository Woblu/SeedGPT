// structoverlap -- do two structures ACTUALLY share space, piece by piece?
//
//   structoverlap <seed> <version> <structA> <ax> <az> <structB> <bx> <bz>
//
// The `overlap` query condition compares NOMINAL boxes -- a rectangle of the
// structure's typical size around its origin. That is a good candidate filter
// and a bad answer, and the two disagree loudly: for one pair it reported
// "mansion 57 blocks away, 192 overlap" while the buildings were ~15-20 apart,
// because the distance is origin-to-origin and a mansion sprawls far past its
// origin.
//
// Trying to settle it from blocks in a real world failed: a woodland mansion and
// a pillager outpost are both made of cobblestone, oak logs, birch planks and
// oak stairs, so material sniffing cannot say which structure a block belongs
// to. That is not a tuning problem, it is the wrong question.
//
// This asks the right one. cubiomes enumerates a structure's actual PIECES with
// their bounding boxes, so intersection becomes arithmetic instead of inference
// -- the same thing fortoverlap.c does for nether fortresses, where it works
// cleanly. A piece pair either shares volume or it does not.
#include "finders.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIECE_CAP 2048

static int pieceCount(Piece *list, int cap, int stype, int mc, uint64_t seed,
                      int x, int z)
{
    StructureSaltConfig ssc;
    // biome -1: the salt config for structures whose salt does not vary by biome.
    // Village does vary, which is why village is not claimed as supported here.
    if (!getStructureSaltConfig(stype, mc, -1, &ssc)) return -1;
    StructureVariant sv;
    memset(&sv, 0, sizeof sv);
    return getStructurePieces(list, cap, stype, ssc, &sv, mc, seed, x, z);
}

// Overlap of two boxes on one axis: 0 when they miss each other.
static int ov1(int a0, int a1, int b0, int b1)
{
    int lo = a0 > b0 ? a0 : b0;
    int hi = a1 < b1 ? a1 : b1;
    return hi >= lo ? hi - lo + 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 9) {
        fprintf(stderr, "usage: structoverlap <seed> <version> "
                        "<structA> <ax> <az> <structB> <bx> <bz>\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { printf("ERROR unknown version\n"); return 2; }

    const char *nameA = argv[3], *nameB = argv[6];
    int ax = atoi(argv[4]), az = atoi(argv[5]);
    int bx = atoi(argv[7]), bz = atoi(argv[8]);

    int sa = -1, sb = -1;
    for (int i = 0; i < 64; i++) {
        const char *n = struct2str(i);
        if (!n) continue;
        if (!strcmp(n, nameA)) sa = i;
        if (!strcmp(n, nameB)) sb = i;
    }
    // Unknown names go to STDOUT as well: a caller redirecting stderr would
    // otherwise read silence as "these do not overlap".
    if (sa < 0) { printf("ERROR unknown structure \"%s\"\n", nameA); return 2; }
    if (sb < 0) { printf("ERROR unknown structure \"%s\"\n", nameB); return 2; }

    Piece *pa = malloc(PIECE_CAP * sizeof(Piece));
    Piece *pb = malloc(PIECE_CAP * sizeof(Piece));
    if (!pa || !pb) { printf("ERROR out of memory\n"); return 2; }

    int na = pieceCount(pa, PIECE_CAP, sa, mc, seed, ax, az);
    int nb = pieceCount(pb, PIECE_CAP, sb, mc, seed, bx, bz);

    printf("seed %lld  %s(%d,%d) x %s(%d,%d)\n",
           (long long)seed, nameA, ax, az, nameB, bx, bz);

    // A structure this engine cannot assemble is UNKNOWN, never "no overlap".
    // Only fortresses, end cities and a few others have piece generators; for
    // the rest the honest answer is that this tool cannot say.
    if (na <= 0 || nb <= 0) {
        printf("  %s pieces: %d\n  %s pieces: %d\n", nameA, na, nameB, nb);
        printf("UNKNOWN: this engine cannot enumerate the pieces of %s, so "
               "whether they overlap is not decidable here -- which is not the "
               "same as them not overlapping.\n",
               na <= 0 ? nameA : nameB);
        return 3;
    }

    long long pairs = 0, shared = 0;
    int minGap = 1 << 30;
    for (int i = 0; i < na; i++)
    for (int j = 0; j < nb; j++) {
        int ox = ov1(pa[i].bb0.x, pa[i].bb1.x, pb[j].bb0.x, pb[j].bb1.x);
        int oy = ov1(pa[i].bb0.y, pa[i].bb1.y, pb[j].bb0.y, pb[j].bb1.y);
        int oz = ov1(pa[i].bb0.z, pa[i].bb1.z, pb[j].bb0.z, pb[j].bb1.z);
        if (ox && oy && oz) {
            pairs++;
            shared += (long long)ox * oy * oz;
        } else {
            // Horizontal separation between this pair, for the near-miss report.
            int gx = ox ? 0 : (pa[i].bb0.x > pb[j].bb1.x
                               ? pa[i].bb0.x - pb[j].bb1.x
                               : pb[j].bb0.x - pa[i].bb1.x);
            int gz = oz ? 0 : (pa[i].bb0.z > pb[j].bb1.z
                               ? pa[i].bb0.z - pb[j].bb1.z
                               : pb[j].bb0.z - pa[i].bb1.z);
            int g = gx > gz ? gx : gz;
            if (g < minGap) minGap = g;
        }
    }

    printf("  %s: %d pieces\n  %s: %d pieces\n", nameA, na, nameB, nb);
    if (pairs) {
        printf("OVERLAP: %lld piece pairs intersect, %lld blocks of shared "
               "volume. The two structures really occupy the same space.\n",
               pairs, shared);
        return 0;
    }
    printf("SEPARATE: no piece of one meets any piece of the other; nearest "
           "pieces are %d blocks apart horizontally.\n", minGap);
    return 1;
}
