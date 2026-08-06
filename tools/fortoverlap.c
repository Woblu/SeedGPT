// fortoverlap -- how much do neighbouring nether fortresses actually interpenetrate?
//
//   fortoverlap <seed> <version> [regX] [regZ] [--quiet]
//   fortoverlap <seed> <version> --rank <seeds>      scan and rank seeds
//
// "Four fortresses close together" is the wrong measure, and it is the one a
// cluster search gives you. A fortress is not a point: it is up to 257 pieces
// sprawling as far as 112 blocks from its start, and -- crucially -- each one is
// generated WITHOUT KNOWLEDGE OF THE OTHERS. getFortressPieces rejects a piece
// that collides with an earlier piece of the SAME fortress; it has no idea the
// neighbouring fortress exists. So two fortresses whose starts are 80 blocks
// apart do not merely sit near each other, they grow through each other.
//
// That is what "fortresses inside each other" means, and it is measurable: build
// both piece lists and count the pairs whose bounding boxes actually intersect.
// Start distance only bounds the opportunity; this counts what happened.
//
// Fortresses in 1.18+ also share their grid with bastions -- same salt, same
// region, same range -- and a fortress exists only where the bastion roll fails.
// So every position is viability-checked, or the count includes bastions.
#include "finders.h"
#include "generator.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define PIECE_CAP 1024

static int boxesMeet(const Piece *a, const Piece *b)
{
    return a->bb0.x <= b->bb1.x && a->bb1.x >= b->bb0.x &&
           a->bb0.y <= b->bb1.y && a->bb1.y >= b->bb0.y &&
           a->bb0.z <= b->bb1.z && a->bb1.z >= b->bb0.z;
}

// Volume shared by two axis-aligned boxes.
static int64_t boxOverlap(const Piece *a, const Piece *b)
{
    int64_t dx = (a->bb1.x < b->bb1.x ? a->bb1.x : b->bb1.x)
               - (a->bb0.x > b->bb0.x ? a->bb0.x : b->bb0.x);
    int64_t dy = (a->bb1.y < b->bb1.y ? a->bb1.y : b->bb1.y)
               - (a->bb0.y > b->bb0.y ? a->bb0.y : b->bb0.y);
    int64_t dz = (a->bb1.z < b->bb1.z ? a->bb1.z : b->bb1.z)
               - (a->bb0.z > b->bb0.z ? a->bb0.z : b->bb0.z);
    if (dx <= 0 || dy <= 0 || dz <= 0) return 0;
    return dx * dy * dz;
}

typedef struct {
    int ok;                 // a fortress really generates here
    int n;                  // pieces
    Pos start;              // block position of the start piece
    Piece *pieces;
} Fort;

// Build one fortress's piece list. Returns 0 if this grid slot is a bastion
// (or has no structure), because counting a bastion as a fortress would inflate
// every number here.
static int buildFort(Fort *f, Generator *g, int mc, uint64_t s48, int regX, int regZ)
{
    memset(f, 0, sizeof *f);
    Pos p;
    if (!getStructurePos(Fortress, mc, s48, regX, regZ, &p)) return 0;
    if (!isViableStructurePos(Fortress, g, p.x, p.z, 0)) return 0;
    f->pieces = malloc((size_t)PIECE_CAP * sizeof(Piece));
    if (!f->pieces) return 0;
    f->n = getFortressPieces(f->pieces, PIECE_CAP, mc, s48, p.x >> 4, p.z >> 4);
    if (f->n <= 0 || f->n >= PIECE_CAP) { free(f->pieces); f->pieces = NULL; return 0; }
    f->start = p;
    f->ok = 1;
    return 1;
}

// Pieces that intersect, and the volume they share, across two fortresses.
static void pairOverlap(const Fort *a, const Fort *b, int *meets, int64_t *vol)
{
    *meets = 0; *vol = 0;
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            if (boxesMeet(&a->pieces[i], &b->pieces[j])) {
                (*meets)++;
                *vol += boxOverlap(&a->pieces[i], &b->pieces[j]);
            }
}

// The whole 2x2 corner. Returns the total intersecting piece pairs, or -1 when
// fewer than two real fortresses are present.
static int cornerScore(Generator *g, int mc, uint64_t s48, int regX, int regZ,
                       int64_t *volOut, int *nfort, int quiet)
{
    Fort f[4];
    int RX[4] = {regX, regX+1, regX, regX+1};
    int RZ[4] = {regZ, regZ, regZ+1, regZ+1};
    int live = 0;
    for (int i = 0; i < 4; i++) if (buildFort(&f[i], g, mc, s48, RX[i], RZ[i])) live++;
    *nfort = live;

    int total = 0; int64_t vol = 0;
    if (live >= 2) {
        for (int a = 0; a < 4; a++) {
            if (!f[a].ok) continue;
            for (int b = a + 1; b < 4; b++) {
                if (!f[b].ok) continue;
                int m; int64_t v;
                pairOverlap(&f[a], &f[b], &m, &v);
                total += m; vol += v;
                if (!quiet && m)
                    printf("    fortress %d x %d : %d piece pairs intersect, "
                           "%" PRId64 " blocks of shared box\n", a, b, m, v);
            }
        }
    }
    if (!quiet) {
        for (int i = 0; i < 4; i++)
            if (f[i].ok)
                printf("    fortress %d at block (%d,%d)  %d pieces\n",
                       i, f[i].start.x, f[i].start.z, f[i].n);
            else
                printf("    slot %d is not a fortress (bastion, or absent)\n", i);
    }
    for (int i = 0; i < 4; i++) free(f[i].pieces);
    *volOut = vol;
    return live >= 2 ? total : -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: fortoverlap <seed> <version> [regX] [regZ]\n"
            "       fortoverlap <seed> <version> --rank <seeds>\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc = str2mc(argv[2]);
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }

    int rank = 0, stdinMode = 0; uint64_t nseeds = 0;
    int regX = 0, regZ = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--rank") && i + 1 < argc) { rank = 1; nseeds = strtoull(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--stdin")) stdinMode = 1;
        else if (argv[i][0] != '-' && !rank) {
            if (regX == 0 && i == 3) regX = atoi(argv[i]);
            else regZ = atoi(argv[i]);
        }
    }

    Generator g;
    setupGenerator(&g, mc, 0);

    // Two-tier: the solver proposes seeds whose four fortress SLOTS are tight,
    // and this scores what the pieces actually did with that opportunity. Start
    // distance only bounds the chance to interpenetrate; the piece lists say
    // whether it happened, and by how much.
    if (stdinMode) {
        char line[256];
        while (fgets(line, sizeof line, stdin)) {
            char *q = line;
            while (*q && (*q < '0' || *q > '9') && *q != '-') q++;
            if (!*q) continue;
            uint64_t s = strtoull(q, NULL, 10) & ((1ULL << 48) - 1);
            applySeed(&g, DIM_NETHER, s);
            int nf; int64_t vol;
            int sc = cornerScore(&g, mc, s, 0, 0, &vol, &nf, 1);
            if (sc < 0) continue;
            printf("%d %" PRId64 " %d %" PRIu64 "\n", sc, vol, nf, s);
            fflush(stdout);
        }
        return 0;
    }

    if (!rank) {
        applySeed(&g, DIM_NETHER, seed);
        int nf; int64_t vol;
        printf("seed %" PRIu64 " corner (%d,%d)..(%d,%d)\n", seed, regX, regZ, regX+1, regZ+1);
        int sc = cornerScore(&g, mc, seed & ((1ULL << 48) - 1), regX, regZ, &vol, &nf, 0);
        if (sc < 0) { printf("  fewer than two fortresses here\n"); return 1; }
        printf("  %d real fortresses, %d intersecting piece pairs, "
               "%" PRId64 " blocks of shared bounding box\n", nf, sc, vol);
        return sc > 0 ? 0 : 1;
    }

    // Ranked scan: the cluster is portable, so this walks seeds at one corner.
    int bestScore = -1; uint64_t bestSeed = 0; int64_t bestVol = 0; int bestN = 0;
    for (uint64_t s = 0; s < nseeds; s++) {
        applySeed(&g, DIM_NETHER, s);
        int nf; int64_t vol;
        int sc = cornerScore(&g, mc, s, 0, 0, &vol, &nf, 1);
        if (sc > bestScore) {
            bestScore = sc; bestSeed = s; bestVol = vol; bestN = nf;
            printf("seed %-14" PRIu64 " %d fortresses  %4d intersecting piece pairs  "
                   "%" PRId64 " shared blocks\n", s, nf, sc, vol);
            fflush(stdout);
        }
    }
    printf("\nbest: seed %" PRIu64 " with %d fortresses, %d intersecting piece pairs, "
           "%" PRId64 " shared blocks\n", bestSeed, bestN, bestScore, bestVol);
    return 0;
}
