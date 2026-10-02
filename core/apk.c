#include "apk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }

/* End of central directory: last 22..65557 bytes. Returns the CD offset and count. */
static int find_cd(const uint8_t *z, size_t size, size_t *cd, unsigned *count)
{
    size_t i;
    if (size < 22) return 0;
    for (i = size - 22;; i--) {
        if (u32(z + i) == 0x06054b50u) {
            *cd = u32(z + i + 16);
            *count = u16(z + i + 10);
            return *cd < size;
        }
        if (i == 0 || size - i > 65557) return 0;
    }
}

/* Walks the central directory; for each entry calls visit(entry header). */
static const uint8_t *walk(const uint8_t *z, size_t size, const char *want, size_t wantlen,
                           int (*fn)(const char *, size_t, void *), void *ctx)
{
    size_t cd, p;
    unsigned count, k;
    if (!find_cd(z, size, &cd, &count)) return NULL;
    for (p = cd, k = 0; k < count && p + 46 <= size && u32(z + p) == 0x02014b50u; k++) {
        size_t nlen = u16(z + p + 28), xlen = u16(z + p + 30), clen = u16(z + p + 32);
        const char *name = (const char *)z + p + 46;
        if (p + 46 + nlen > size) return NULL;
        if (want && nlen == wantlen && !memcmp(name, want, nlen)) return z + p;
        if (fn && fn(name, nlen, ctx)) return NULL;
        p += 46 + nlen + xlen + clen;
    }
    return NULL;
}

void aoi_apk_list(const void *zip, size_t size, int (*fn)(const char *, size_t, void *), void *ctx)
{
    walk(zip, size, NULL, 0, fn, ctx);
}

void *aoi_apk_extract(const void *zip, size_t size, const char *name, size_t *out_size, const char **err)
{
    const uint8_t *z = zip, *e = walk(z, size, name, strlen(name), NULL, NULL), *data;
    size_t method, csize, usize, loc;
    uint8_t *out;
    if (!e) { *err = "entry not found in the APK"; return NULL; }
    method = u16(e + 10); csize = u32(e + 20); usize = u32(e + 24); loc = u32(e + 42);
    if (loc + 30 > size || u32(z + loc) != 0x04034b50u) { *err = "bad local header"; return NULL; }
    data = z + loc + 30 + u16(z + loc + 26) + u16(z + loc + 28);
    if ((size_t)(data - z) + csize > size) { *err = "entry out of range"; return NULL; }
    if (!(out = malloc(usize ? usize : 1))) { *err = "no memory"; return NULL; }
    if (method == 0) {
        if (csize != usize) { free(out); *err = "stored entry size mismatch"; return NULL; }
        memcpy(out, data, usize);
    } else if (method == 8) {
        z_stream s;
        int r;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(out); *err = "inflateInit failed"; return NULL; }
        s.next_in = (Bytef *)data; s.avail_in = (uInt)csize;
        s.next_out = out; s.avail_out = (uInt)usize;
        r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (r != Z_STREAM_END || s.total_out != usize) { free(out); *err = "inflate failed"; return NULL; }
    } else { free(out); *err = "unsupported compression method"; return NULL; }
    *out_size = usize;
    return out;
}

/* ---------- AndroidManifest.xml (binary XML) ---------- */

/* String i of a string pool chunk at p (size n), as UTF-8 into out. */
static void pool_string(const uint8_t *p, size_t n, uint32_t i, char *out, size_t outn)
{
    uint32_t count = u32(p + 8), flags = u32(p + 16), start = u32(p + 20), off;
    size_t o = 0, k, len;
    const uint8_t *s;
    out[0] = 0;
    if (i >= count || 28 + 4 * (size_t)i + 4 > n) return;
    off = u32(p + 28 + 4 * i);
    if ((size_t)start + off + 4 > n) return;
    s = p + start + off;
    if (flags & 0x100) {                                       /* UTF-8: char count, byte count, bytes */
        s += (s[0] & 0x80) ? 2 : 1;
        len = (s[0] & 0x80) ? (size_t)((s[0] & 0x7f) << 8 | s[1]) : s[0];
        s += (s[0] & 0x80) ? 2 : 1;
        for (k = 0; k < len && o + 1 < outn && s + k < p + n; k++) out[o++] = (char)s[k];
    } else {                                                   /* UTF-16 units, kept when ASCII */
        len = u16(s); s += 2;
        if (len & 0x8000) { len = (len & 0x7fff) << 16 | u16(s); s += 2; }
        for (k = 0; k < len && o + 1 < outn && s + 2 * k + 1 < p + n; k++) {
            uint16_t ch = u16(s + 2 * k);
            out[o++] = ch < 0x80 ? (char)ch : '?';
        }
    }
    out[o] = 0;
}

int aoi_apk_manifest(const void *zip, size_t size, char *pkg, size_t pkgn, char *label, size_t labeln)
{
    const char *err;
    size_t n = 0, o;
    uint8_t *x = aoi_apk_extract(zip, size, "AndroidManifest.xml", &n, &err);
    const uint8_t *pool = NULL;
    size_t pooln = 0;
    pkg[0] = label[0] = 0;
    if (!x) return -1;
    for (o = 8; o + 8 <= n; ) {                                /* chunks after the XML header */
        uint16_t type = u16(x + o);
        uint32_t csize = u32(x + o + 4);
        if (csize < 8 || o + csize > n) break;
        if (type == 0x0001 && !pool) { pool = x + o; pooln = csize; }
        else if (type == 0x0102 && pool && csize >= 36) {      /* start element */
            const uint8_t *e = x + o + 16;                     /* ns, name, attrStart, attrSize, attrCount */
            char name[64], an[64];
            uint16_t astart = u16(e + 8), asize = u16(e + 10), acount = u16(e + 12), a;
            pool_string(pool, pooln, u32(e + 4), name, sizeof name);
            for (a = 0; a < acount && 16 + astart + (size_t)(a + 1) * asize <= csize; a++) {
                const uint8_t *at = e + astart + (size_t)a * asize;
                pool_string(pool, pooln, u32(at + 4), an, sizeof an);
                if (!strcmp(name, "manifest") && !strcmp(an, "package"))
                    pool_string(pool, pooln, u32(at + 8), pkg, pkgn);
                else if (!strcmp(name, "application") && !strcmp(an, "label") && at[15] == 0x03)
                    pool_string(pool, pooln, u32(at + 16), label, labeln);   /* a plain string label */
            }
            if (!strcmp(name, "application")) break;
        }
        o += csize;
    }
    free(x);
    if (pkg[0] && !label[0]) {                                 /* com.example.foo -> Foo */
        const char *d = strrchr(pkg, '.');
        snprintf(label, labeln, "%s", d ? d + 1 : pkg);
        if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 32);
    }
    return pkg[0] ? 0 : -1;
}
