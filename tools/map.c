// map -- render a seed's biome map as a PNG, with structure markers.
//
//   usage: map <seed> <version> <radius> <px> [out.png|markers] [cx] [cz]
//              [large|-] [nether|end] [nomarkers]
//          ...   radius in blocks, px = output width/height in pixels
//   writes PNG to stdout when no path is given (binary; the UI streams it);
//   "markers" prints the marked structures as JSON instead. MAP_THREADS sets
//   the thread count (default: cores, at most 8).
//
// cubiomes ships savePPM, which browsers cannot display, so this writes a real
// PNG. No image library: a PNG is just zlib-deflate over filtered scanlines,
// and "stored" (uncompressed) deflate blocks are legal, so we emit those. The
// file is bigger than an optimised encoder would produce but is a valid PNG
// that every decoder accepts -- and it keeps the project dependency-free.
#include "query.h"
#include "finders.h"
#include "generator.h"
#include "util.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// ------------------------------------------------------------------ PNG out

static unsigned long crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (int n = 0; n < 256; n++) {
        unsigned long c = (unsigned long)n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long crc32_buf(const unsigned char *buf, size_t len, unsigned long crc)
{
    if (!crc_ready) crc_init();
    crc ^= 0xffffffffUL;
    for (size_t n = 0; n < len; n++) crc = crc_table[(crc ^ buf[n]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;  p[3] = v & 0xff;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t len)
{
    unsigned char hdr[4];
    be32(hdr, (unsigned long)len);
    fwrite(hdr, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    unsigned long crc = crc32_buf((const unsigned char *)type, 4, 0);
    if (len) crc = crc32_buf(data, len, crc);
    unsigned char c[4]; be32(c, crc);
    fwrite(c, 1, 4, f);
}

// adler32 over the raw (pre-deflate) stream, as zlib requires
static unsigned long adler32_buf(const unsigned char *d, size_t n)
{
    unsigned long a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static int write_png(FILE *f, const unsigned char *rgb, int w, int h)
{
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

    unsigned char ihdr[13];
    be32(ihdr, (unsigned long)w);
    be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8;      // bit depth
    ihdr[9] = 2;      // colour type 2 = truecolour RGB
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, sizeof ihdr);

    // filtered scanlines: one leading filter byte (0 = None) per row
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    unsigned char *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (1 + (size_t)w * 3);
        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }

    // zlib stream: 2-byte header, stored deflate blocks (<=65535 each), adler32
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    unsigned char *z = malloc(z_len);
    if (!z) { free(raw); return -1; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;          // CM=8, no preset dict, fastest
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off; if (n > 65535) n = 65535;
        int final = (off + n >= raw_len);
        z[zi++] = (unsigned char)final;       // BFINAL, BTYPE=00 (stored)
        z[zi++] = n & 0xff; z[zi++] = (n >> 8) & 0xff;
        z[zi++] = ~n & 0xff; z[zi++] = (~n >> 8) & 0xff;
        memcpy(z + zi, raw + off, n); zi += n; off += n;
    }
    unsigned long ad = adler32_buf(raw, raw_len);
    be32(z + zi, ad); zi += 4;
    chunk(f, "IDAT", z, zi);
    chunk(f, "IEND", NULL, 0);
    free(z); free(raw);
    return 0;
}

// ----------------------------------------------------------------- markers

static void blend(unsigned char *px, int w, int h, int x, int y,
                  int r, int g, int b, double a)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    unsigned char *p = px + ((size_t)y * w + x) * 3;
    p[0] = (unsigned char)(p[0] * (1 - a) + r * a);
    p[1] = (unsigned char)(p[1] * (1 - a) + g * a);
    p[2] = (unsigned char)(p[2] * (1 - a) + b * a);
}

// Hollow square + centre dot: readable against any biome colour without
// needing a font.
static void marker(unsigned char *px, int w, int h, int cx, int cy, int size,
                   int r, int g, int b)
{
    for (int d = -size; d <= size; d++) {
        blend(px, w, h, cx + d, cy - size, 0, 0, 0, .55);
        blend(px, w, h, cx + d, cy + size, 0, 0, 0, .55);
        blend(px, w, h, cx - size, cy + d, 0, 0, 0, .55);
        blend(px, w, h, cx + size, cy + d, 0, 0, 0, .55);
    }
    for (int d = -(size-1); d <= size-1; d++) {
        blend(px, w, h, cx + d, cy - size + 1, r, g, b, .95);
        blend(px, w, h, cx + d, cy + size - 1, r, g, b, .95);
        blend(px, w, h, cx - size + 1, cy + d, r, g, b, .95);
        blend(px, w, h, cx + size - 1, cy + d, r, g, b, .95);
    }
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            blend(px, w, h, cx + dx, cy + dy, r, g, b, 1.0);
}

static void crosshair(unsigned char *px, int w, int h, int cx, int cy)
{
    for (int d = -6; d <= 6; d++) {
        if (d > -3 && d < 3) continue;      // gap so the point stays visible
        blend(px, w, h, cx + d, cy, 255, 255, 255, .85);
        blend(px, w, h, cx, cy + d, 255, 255, 255, .85);
    }
}

// Landmarks only. Trial chambers and ruined portals are dense enough (30+
// inside 2000 blocks) that marking them buries everything else.
static const struct { const char *n; int rr, gg, bb; } STYLE[] = {
    {"mansion",       255,  70,  70}, {"village",        255, 210,  60},
    {"monument",       80, 190, 255}, {"ancient_city",   190, 110, 255},
    {"outpost",       255, 140,  40}, {"desert_pyramid", 250, 240, 160},
    {"jungle_temple", 120, 220, 120}, {"swamp_hut",      150, 110, 200},
    {"igloo",         230, 245, 255},
    // nether + end landmarks, drawn when those dimensions are shown
    {"fortress",      220,  60,  60}, {"bastion",        120,  90, 160},
    {"end_city",      210, 200, 120},
};
#define NSTYLE ((int)(sizeof STYLE / sizeof STYLE[0]))

// ------------------------------------------------------------- the workers
//
// A view is rendered by several threads, each with its own Generator (they
// are not shareable). Both halves split cleanly: a biome cell depends only on
// its position, so horizontal strips generated apart are the same cells the
// whole range would give; and every structure attempt is independent.

typedef struct { int si, x, z; } Mark;    // si: STYLE index, -1 = stronghold

// One unit of marker work: one structure type over one column of regions.
// Units are numbered in type-major, column-major order and their marks are
// emitted in that order, so the output is the same as a single thread's.
typedef struct { int type, si, rx, rz0, rz1; Mark *m; int n, cap; } MarkUnit;

typedef struct {
    int mc; uint32_t gflags; uint64_t seed; int dim, cx, cz, radius;
    MarkUnit *units; int nunits;
    volatile LONG next;
} MarkJob;

static void addMark(MarkUnit *u, int si, int x, int z)
{
    if (u->n == u->cap) {
        int cap = u->cap ? 2 * u->cap : 8;
        Mark *nm = realloc(u->m, (size_t)cap * sizeof *nm);
        if (!nm) return;
        u->m = nm; u->cap = cap;
    }
    u->m[u->n++] = (Mark){ si, x, z };
}

static DWORD WINAPI markWorker(LPVOID arg)
{
    MarkJob *j = arg;
    Generator g;
    setupGenerator(&g, j->mc, j->gflags);
    applySeed(&g, j->dim, j->seed);
    for (;;) {
        LONG k = InterlockedIncrement(&j->next) - 1;
        if (k >= j->nunits) break;
        MarkUnit *u = &j->units[k];
        for (int rz = u->rz0; rz <= u->rz1; rz++) {
            Pos p;
            if (!getStructurePos(u->type, j->mc, j->seed, u->rx, rz, &p)) continue;
            if (p.x < j->cx - j->radius || p.x > j->cx + j->radius ||
                p.z < j->cz - j->radius || p.z > j->cz + j->radius) continue;
            if (!structureGenerates(j->mc, j->gflags, j->seed, &g, u->type, p)) continue;
            addMark(u, u->si, p.x, p.z);
        }
    }
    return 0;
}

// Every landmark in the square [c +- radius], in a fixed order. Each attempt
// goes through structureGenerates -- the finder's own gate -- so the map shows
// exactly the structures a search would accept, not every biome-viable
// attempt. Returns the count; *out is malloc'd (NULL when there are none).
static int collectMarkers(Generator *g, int mc, uint32_t gflags, uint64_t seed, int dim,
                          int cx, int cz, int radius, int nthreads, Mark **out)
{
    MarkJob j = { mc, gflags, seed, dim, cx, cz, radius, NULL, 0, 0 };
    int cap = 0;
    for (int i = 0; i < queryStructureCount(); i++) {
        const char *name = queryStructureName(i);
        int type = queryStructureType(i);
        StructureConfig sc;
        if (!getStructureConfig(type, mc, &sc)) continue;
        if (sc.dim != dim) continue;
        int si = -1;
        for (int k = 0; k < NSTYLE; k++) if (!strcmp(STYLE[k].n, name)) si = k;
        if (si < 0) continue;               // only mark the notable ones
        double sp = sc.regionSize * 16.0;
        int rx0 = (int)floor((cx - radius) / sp), rx1 = (int)floor((cx + radius) / sp);
        int rz0 = (int)floor((cz - radius) / sp), rz1 = (int)floor((cz + radius) / sp);
        for (int rx = rx0; rx <= rx1; rx++) {
            if (j.nunits == cap) {
                cap = cap ? 2 * cap : 64;
                MarkUnit *nu = realloc(j.units, (size_t)cap * sizeof *nu);
                if (!nu) break;
                j.units = nu;
            }
            j.units[j.nunits++] = (MarkUnit){ type, si, rx, rz0, rz1, NULL, 0, 0 };
        }
    }
    if (nthreads > j.nunits) nthreads = j.nunits;
    if (nthreads > 0) {
        HANDLE th[64];
        for (int t = 0; t < nthreads; t++) th[t] = CreateThread(NULL, 0, markWorker, &j, 0, NULL);
        WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
        for (int t = 0; t < nthreads; t++) CloseHandle(th[t]);
    }

    int n = 0, mcap = 0;
    Mark *m = NULL;
    for (int k = 0; k < j.nunits; k++) {
        MarkUnit *u = &j.units[k];
        if (n + u->n > mcap) {
            mcap = 2 * (n + u->n) + 16;
            Mark *nm = realloc(m, (size_t)mcap * sizeof *nm);
            if (!nm) break;
            m = nm;
        }
        memcpy(m + n, u->m, (size_t)u->n * sizeof *m);
        n += u->n;
        free(u->m);
    }
    free(j.units);

    // Strongholds: ring-placed, not on the region grid, and the structure
    // people most often want to find on a map. Walk the rings until they are
    // past every point of the view.
    //
    // Each one costs a biome search, and a view far from the origin walks all
    // 128: over a second, alone, on one thread. Since 1.19.3 that search draws
    // from its own RNG, so skipping it (g = NULL) leaves every later
    // stronghold exactly where it was. It moves a stronghold at most 112
    // blocks from its approximate spot (then snaps it within its chunk), so
    // one whose approximate spot is further than that outside the view cannot
    // be in it. Before 1.19.3 the search consumes the shared RNG and cannot be
    // skipped.
    if (dim == DIM_OVERWORLD) {
        StrongholdIter sh;
        initFirstStronghold(&sh, mc, seed);
        double cdist = sqrt((double)cx * cx + (double)cz * cz);
        const int SLACK = 112 + 16 + 32;
        for (;;) {
            Pos ap = sh.nextapprox;
            int skip = mc > MC_1_19_2 &&
                (ap.x < cx - radius - SLACK || ap.x > cx + radius + SLACK ||
                 ap.z < cz - radius - SLACK || ap.z > cz + radius + SLACK);
            if (nextStronghold(&sh, skip ? NULL : g) <= 0) break;
            Pos p = sh.pos;
            if (!skip && p.x >= cx-radius && p.x <= cx+radius && p.z >= cz-radius && p.z <= cz+radius) {
                if (n == mcap) {
                    mcap = 2 * mcap + 16;
                    Mark *nm = realloc(m, (size_t)mcap * sizeof *nm);
                    if (!nm) break;
                    m = nm;
                }
                m[n++] = (Mark){ -1, p.x, p.z };
            }
            if (sh.dist * 16.0 > cdist + radius * 2.0 + 2048.0) break;
        }
    }
    *out = m;
    return n;
}

typedef struct { int mc; uint32_t gflags; uint64_t seed; int dim; Range r; int *out; int rc; } Strip;

static DWORD WINAPI stripWorker(LPVOID arg)
{
    Strip *s = arg;
    Generator g;
    setupGenerator(&g, s->mc, s->gflags);
    applySeed(&g, s->dim, s->seed);
    int *c = allocCache(&g, s->r);
    if (!c) { s->rc = 1; return 0; }
    s->rc = genBiomes(&g, c, s->r);
    if (!s->rc) memcpy(s->out, c, (size_t)s->r.sx * s->r.sz * sizeof(int));
    free(c);
    return 0;
}

// The whole range into `cache` (sx * sz ints, row-major in z), in strips.
static int genBiomesThreaded(int mc, uint32_t gflags, uint64_t seed, int dim,
                             Range r, int *cache, int nthreads)
{
    if (nthreads > r.sz) nthreads = r.sz;
    if (nthreads < 1) nthreads = 1;
    Strip st[64];
    HANDLE th[64];
    for (int t = 0; t < nthreads; t++) {
        int z0 = (int)((long long)r.sz * t / nthreads);
        int z1 = (int)((long long)r.sz * (t + 1) / nthreads);
        st[t] = (Strip){ mc, gflags, seed, dim, r, cache + (size_t)z0 * r.sx, 0 };
        st[t].r.z = r.z + z0;
        st[t].r.sz = z1 - z0;
        th[t] = CreateThread(NULL, 0, stripWorker, &st[t], 0, NULL);
    }
    WaitForMultipleObjects(nthreads, th, TRUE, INFINITE);
    int rc = 0;
    for (int t = 0; t < nthreads; t++) { CloseHandle(th[t]); rc |= st[t].rc; }
    return rc;
}

// World spawn on its own thread and Generator: on 1.18+ it is a climate
// search costing about as much as the rest of a small view, and nothing else
// waits on it until the end.
typedef struct { int mc; uint32_t gflags; uint64_t seed; Pos p; HANDLE th; } SpawnJob;

static DWORD WINAPI spawnWorker(LPVOID arg)
{
    SpawnJob *s = arg;
    Generator g;
    setupGenerator(&g, s->mc, s->gflags);
    applySeed(&g, DIM_OVERWORLD, s->seed);
    s->p = getSpawn(&g);
    return 0;
}

static void spawnStart(SpawnJob *s, int mc, uint32_t gflags, uint64_t seed, int dim)
{
    *s = (SpawnJob){ mc, gflags, seed, { INT32_MIN, INT32_MIN }, NULL };
    if (dim == DIM_OVERWORLD) s->th = CreateThread(NULL, 0, spawnWorker, s, 0, NULL);
}

static Pos spawnWait(SpawnJob *s)
{
    if (s->th) { WaitForSingleObject(s->th, INFINITE); CloseHandle(s->th); s->th = NULL; }
    return s->p;
}

static const char *markName(const Mark *m) { return m->si < 0 ? "stronghold" : STYLE[m->si].n; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: map <seed> [version] [radius] [px] [out.png|markers] [cx] [cz]"
                        " [large|-] [nether|end] [nomarkers]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc     = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
    if (mc <= MC_UNDEF) { fprintf(stderr, "unknown version\n"); return 2; }
    int radius = (argc > 3) ? atoi(argv[3]) : 2000;
    int px     = (argc > 4) ? atoi(argv[4]) : 512;
    const char *outpath = (argc > 5) ? argv[5] : NULL;
    int cx     = (argc > 6) ? atoi(argv[6]) : 0;   // map centre (blocks)
    int cz     = (argc > 7) ? atoi(argv[7]) : 0;
    // The world preset has to reach the renderer too: a Large Biomes result
    // drawn with the default generator is a picture of a different world.
    int large  = (argc > 8) && !strcmp(argv[8], "large");
    // Which dimension to draw. The finder searches all three, so a viewer that
    // can only show the overworld cannot check half of what it reports.
    // "nomarkers": biomes and spawn only -- the web map overlays its own
    // clickable markers from the "markers" list, so baking them in as well
    // would compute every structure twice per view.
    int dim = DIM_OVERWORLD, bake = 1;
    for (int i = 8; i < argc; i++) {
        if (!strcmp(argv[i], "nether")) dim = DIM_NETHER;
        else if (!strcmp(argv[i], "end")) dim = DIM_END;
        else if (!strcmp(argv[i], "nomarkers")) bake = 0;
    }
    if (px < 64) px = 64;
    if (px > 2048) px = 2048;

    // Threads: MAP_THREADS, else every core up to 8 -- a view is interactive
    // and short, and two of them (picture + markers) usually run at once.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nthreads = (int)si.dwNumberOfProcessors;
    if (nthreads > 8) nthreads = 8;
    const char *envT = getenv("MAP_THREADS");
    if (envT && atoi(envT) > 0) nthreads = atoi(envT);
    if (nthreads > 64) nthreads = 64;

    Generator g;
    uint32_t gflags = large ? LARGE_BIOMES : 0;
    setupGenerator(&g, mc, gflags);
    applySeed(&g, dim, seed);

    // out.png == "markers": print the structures this map would mark, as JSON,
    // and draw nothing. The PNG bakes its markers in, so a browser could see
    // them but not click them or read their coordinates; this is the same list
    // as data. Biome generation is skipped -- it is most of a render's cost.
    SpawnJob sj;
    spawnStart(&sj, mc, gflags, seed, dim);
    if (outpath && !strcmp(outpath, "markers")) {
        Mark *ms = NULL;
        int n = collectMarkers(&g, mc, gflags, seed, dim, cx, cz, radius, nthreads, &ms);
        printf("{\"markers\":[");
        for (int i = 0; i < n; i++)
            printf("%s{\"type\":\"%s\",\"x\":%d,\"z\":%d}", i ? "," : "", markName(&ms[i]),
                   ms[i].x, ms[i].z);
        printf("]");
        if (dim == DIM_OVERWORLD) {
            Pos sp = spawnWait(&sj);
            printf(",\"spawn\":{\"x\":%d,\"z\":%d}", sp.x, sp.z);
        }
        printf("}\n");
        free(ms);
        return 0;
    }

    // One biome cell per output pixel. scale must be a valid cubiomes scale,
    // so pick the coarsest that still fills the image.
    int span = 2 * radius;
    int scale = 4;
    while (scale < 256 && span / (scale * 4) > px) scale *= 4;
    int cells = span / scale;
    if (cells < 1) cells = 1;
    if (cells > px) cells = px;

    // Nether biomes are 3D and its "surface" probe has to sit inside the roof;
    // the overworld/end sample at build height like every other check here.
    int probeY = (dim == DIM_NETHER) ? (64 >> 2) : (319 >> 2);
    Range r = {scale, (cx - radius) / scale, (cz - radius) / scale, cells, cells, probeY, 1};
    int *cache = malloc((size_t)cells * cells * sizeof(int));
    if (!cache) { fprintf(stderr, "alloc failed\n"); return 1; }
    if (genBiomesThreaded(mc, gflags, seed, dim, r, cache, nthreads)) {
        fprintf(stderr, "genBiomes failed\n"); return 1;
    }

    unsigned char colors[256][3];
    initBiomeColors(colors);

    int scalePx = px / cells; if (scalePx < 1) scalePx = 1;
    int w = cells * scalePx, h = w;
    unsigned char *img = malloc((size_t)w * h * 3);
    if (!img) { fprintf(stderr, "alloc failed\n"); return 1; }
    biomesToImage(img, colors, cache, cells, cells, scalePx, 1);

    // block -> pixel (relative to the map centre cx/cz)
    #define PX(bx) (int)(((double)((bx) - (cx - radius)) / span) * w)
    #define PZ(bz) (int)(((double)((bz) - (cz - radius)) / span) * h)

    // Structures, drawn over the biomes (the same list "markers" prints).
    if (bake) {
        Mark *ms = NULL;
        int n = collectMarkers(&g, mc, gflags, seed, dim, cx, cz, radius, nthreads, &ms);
        for (int i = 0; i < n; i++) {
            if (ms[i].si < 0) marker(img, w, h, PX(ms[i].x), PZ(ms[i].z), 4, 240, 240, 240);
            else marker(img, w, h, PX(ms[i].x), PZ(ms[i].z), 4,
                        STYLE[ms[i].si].rr, STYLE[ms[i].si].gg, STYLE[ms[i].si].bb);
        }
        free(ms);
    }

    // Only the overworld has a world spawn; drawing one elsewhere would be a
    // crosshair over a place that means nothing.
    Pos spawn = spawnWait(&sj);
    if (spawn.x >= cx-radius && spawn.x <= cx+radius && spawn.z >= cz-radius && spawn.z <= cz+radius)
        crosshair(img, w, h, PX(spawn.x), PZ(spawn.z));

    FILE *f = stdout;
    if (outpath) {
        f = fopen(outpath, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", outpath); return 1; }
    } else {
        // stdout must be binary on Windows or the PNG is corrupted by CRLF
        // translation -- a silent failure that only shows up in the decoder.
#ifdef _WIN32
        _setmode(_fileno(stdout), 0x8000 /* _O_BINARY */);
#endif
    }
    int rc = write_png(f, img, w, h);
    if (outpath) fclose(f);
    free(img); free(cache);
    return rc;
}
