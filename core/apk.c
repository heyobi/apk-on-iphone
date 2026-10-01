#include "apk.h"

#include <stdint.h>
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
