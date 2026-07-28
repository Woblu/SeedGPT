// map -- render a seed's biome map as a PNG, with structure markers.
//
//   usage: map <seed> <version> <radius> <px> [out.png]
//          ...   radius in blocks, px = output width/height in pixels
//   writes PNG to stdout when no path is given (binary; the UI streams it)
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: map <seed> [version] [radius] [px] [out.png]\n");
        return 2;
    }
    uint64_t seed = (uint64_t)strtoll(argv[1], NULL, 10);
    int mc     = (argc > 2) ? str2mc(argv[2]) : MC_1_21;
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
    int dim = DIM_OVERWORLD;
    for (int i = 8; i < argc; i++) {
        if (!strcmp(argv[i], "nether")) dim = DIM_NETHER;
        else if (!strcmp(argv[i], "end")) dim = DIM_END;
    }
    if (mc < 0) { fprintf(stderr, "unknown version\n"); return 2; }
    if (px < 64) px = 64;
    if (px > 2048) px = 2048;

    Generator g;
    setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
    applySeed(&g, dim, seed);

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
    int *cache = allocCache(&g, r);
    if (!cache) { fprintf(stderr, "alloc failed\n"); return 1; }
    if (genBiomes(&g, cache, r)) { fprintf(stderr, "genBiomes failed\n"); return 1; }

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

    // Structures, drawn over the biomes.
    // Landmarks only. Trial chambers and ruined portals are dense enough
    // (30+ inside 2000 blocks) that marking them buries everything else.
    struct { const char *n; int rr, gg, bb; } style[] = {
        {"mansion",       255,  70,  70}, {"village",        255, 210,  60},
        {"monument",       80, 190, 255}, {"ancient_city",   190, 110, 255},
        {"outpost",       255, 140,  40}, {"desert_pyramid", 250, 240, 160},
        {"jungle_temple", 120, 220, 120}, {"swamp_hut",      150, 110, 200},
        {"igloo",         230, 245, 255},
        // nether + end landmarks, drawn when those dimensions are shown
        {"fortress",      220,  60,  60}, {"bastion",        120,  90, 160},
        {"end_city",      210, 200, 120},
    };
    int nstyle = (int)(sizeof(style)/sizeof(style[0]));

    for (int i = 0; i < queryStructureCount(); i++) {
        const char *name = queryStructureName(i);
        int type = queryStructureType(i);
        StructureConfig sc;
        if (!getStructureConfig(type, mc, &sc)) continue;
        if (sc.dim != dim) continue;

        int si = -1;
        for (int k = 0; k < nstyle; k++) if (!strcmp(style[k].n, name)) si = k;
        if (si < 0) continue;               // only mark the notable ones

        double sp = sc.regionSize * 16.0;
        int rx0 = (int)((cx - radius) / sp) - 1, rx1 = (int)((cx + radius) / sp) + 1;
        int rz0 = (int)((cz - radius) / sp) - 1, rz1 = (int)((cz + radius) / sp) + 1;
        for (int rx = rx0; rx <= rx1; rx++)
        for (int rz = rz0; rz <= rz1; rz++) {
            Pos p;
            if (!getStructurePos(type, mc, seed, rx, rz, &p)) continue;
            if (p.x < cx-radius || p.x > cx+radius || p.z < cz-radius || p.z > cz+radius) continue;
            if (!isViableStructurePos(type, &g, p.x, p.z, 0)) continue;
            marker(img, w, h, PX(p.x), PZ(p.z), 4,
                   style[si].rr, style[si].gg, style[si].bb);
        }
    }

    // Only the overworld has a world spawn; drawing one elsewhere would be a
    // crosshair over a place that means nothing.
    Pos spawn = (dim == DIM_OVERWORLD) ? getSpawn(&g) : (Pos){INT32_MIN, INT32_MIN};
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
