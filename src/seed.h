// Turning what a player types into the seed Minecraft would use.
//
// Minecraft (Java) does exactly this with the "Seed for the World Generator"
// box: if the text parses as a signed 64-bit integer (Long.parseLong) that IS
// the seed; otherwise the seed is the text's String.hashCode(), a 32-bit value
// sign-extended to 64. So "-4172144997902289642" is that number, "glacier" is
// its hash, and -- the case that bit us -- "18446744073709551615" is NOT the
// all-ones seed: it overflows a signed long, so Minecraft hashes it as text.
// strtoll silently clamped it to 9223372036854775807 instead, a different world.
#pragma once

#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

// Java's String.hashCode over UTF-16 code units. The input is UTF-8; code
// points above U+FFFF become surrogate pairs, exactly as Java stores them.
static inline int32_t javaStringHash(const char *s)
{
    uint32_t h = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        if (p[0] < 0x80)                       { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { cp = 0xFFFD; p += 1; }          // malformed byte: Java's replacement char
        if (cp >= 0x10000) {
            cp -= 0x10000;
            h = 31 * h + (0xD800 + (cp >> 10));
            h = 31 * h + (0xDC00 + (cp & 0x3FF));
        } else {
            h = 31 * h + cp;
        }
    }
    return (int32_t)h;
}

// Is `s` a plain base-10 long the way Long.parseLong accepts it: an optional
// sign then digits only, no spaces, within range.
static inline int parseJavaLong(const char *s, int64_t *out)
{
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!*p) return 0;
    for (const char *d = p; *d; d++) if (*d < '0' || *d > '9') return 0;
    errno = 0;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE || *end) return 0;
    *out = (int64_t)v;
    return 1;
}

// The world seed for what the player typed. Like the game, surrounding
// whitespace is trimmed first (String.trim: anything <= ' '). Empty text means
// "random" in game; there is nothing to reproduce, so it is refused (returns 0).
static inline int parseWorldSeed(const char *s, uint64_t *seed)
{
    if (!s) return 0;
    char buf[1024];
    while (*s && (unsigned char)*s <= ' ') s++;
    size_t n = 0;
    while (s[n] && n < sizeof buf - 1) { buf[n] = s[n]; n++; }
    while (n > 0 && (unsigned char)buf[n-1] <= ' ') n--;
    buf[n] = 0;
    if (!n) return 0;
    int64_t v;
    if (parseJavaLong(buf, &v)) { *seed = (uint64_t)v; return 1; }
    *seed = (uint64_t)(int64_t)javaStringHash(buf);
    return 1;
}
