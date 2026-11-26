/* glibc's routed string functions (core/lxfast.c) against byte-by-byte references:
 * random lengths, alignments and contents, overlapping moves, and strings that end on
 * the last byte before a PROT_NONE page. Built on the arm64 runner, run under lxrun in
 * a glibc root (tests/linux/run_libcfast.sh). Prints "libcfast ok N" or the first
 * difference. Comparisons are checked by sign: the C standard promises only that, and
 * glibc's own values depend on its variant (-128 where the bytes differ by one).
 * cc -O1 -fno-builtin -o libcfast tests/linux/libcfast.c */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long long seed = 88172645463325252ULL;
static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (unsigned)seed; }
static long checks;

#define FAIL(...) do { printf("libcfast FAIL " __VA_ARGS__); printf("\n"); exit(1); } while (0)
static int sgn(long v) { return (v > 0) - (v < 0); }

static size_t r_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int r_strcmp(const char *a, const char *b, size_t max)
{
    size_t i;
    for (i = 0; i < max; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
static int r_memcmp(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

static void fill(char *p, size_t n, int alphabet)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (char)(alphabet ? 'a' + rnd() % alphabet : rnd() % 255 + 1);
}

static void strings(char *buf, size_t cap)
{
    int it;
    for (it = 0; it < 20000; it++) {
        size_t la = rnd() % 300, lb, oa = rnd() % 16, ob = rnd() % 16, n;
        char *a = buf + oa, *b = buf + cap / 2 + ob;
        int alpha = rnd() % 3 ? 3 : 0, ch = 'a' + rnd() % 4;
        fill(a, la, alpha); a[la] = 0;
        if (rnd() % 2) { lb = la; memmove(b, a, la + 1); if (la && rnd() % 2) b[rnd() % la] ^= (char)(1 + rnd() % 3); }
        else { lb = rnd() % 300; fill(b, lb, alpha); b[lb] = 0; }
        n = rnd() % 320;
        if (strlen(a) != r_strlen(a)) FAIL("strlen %zu", la);
        if (strnlen(a, n) != (r_strlen(a) < n ? r_strlen(a) : n)) FAIL("strnlen");
        if (sgn(strcmp(a, b)) != sgn(r_strcmp(a, b, (size_t)-1))) FAIL("strcmp sign");
        if (sgn(strncmp(a, b, n)) != sgn(r_strcmp(a, b, n))) FAIL("strncmp");
        {
            char *q = a, *last = NULL;
            while (*q && *q != ch) q++;
            if (strchr(a, ch) != (*q ? q : NULL)) FAIL("strchr");
            if (strchrnul(a, ch) != q) FAIL("strchrnul");
            if (strchr(a, 0) != a + la) FAIL("strchr nul");
            for (q = a; *q; q++) if (*q == ch) last = q;
            if (strrchr(a, ch) != last) FAIL("strrchr");
            if (strrchr(a, 0) != a + la) FAIL("strrchr nul");
        }
        {
            size_t lim = la + 1 < 64 ? la + 1 : 64;
            const char *q = NULL;
            size_t i;
            for (i = 0; i < lim; i++) if (a[i] == ch) { q = a + i; break; }
            if (memchr(a, ch, lim) != q) FAIL("memchr");
            if (rawmemchr(a, 0) != a + la) FAIL("rawmemchr");
        }
        {
            size_t m = la < lb ? la : lb;
            int x = memcmp(a, b, m), y = r_memcmp((unsigned char *)a, (unsigned char *)b, m);
            if (sgn(x) != sgn(y)) FAIL("memcmp %zu", m);
        }
        {
            char *d = buf + cap / 4 + rnd() % 16;
            if (strcpy(d, a) != d || strcmp(d, a)) FAIL("strcpy");
            if (stpcpy(d, a) != d + la) FAIL("stpcpy");
        }
        checks += 16;
    }
}

static void memory(unsigned char *buf, size_t cap)
{
    unsigned char *ref = malloc(cap);
    int it;
    for (it = 0; it < 4000; it++) {
        size_t n = rnd() % (rnd() % 8 ? 200 : 20000), s = rnd() % (cap - n), d = rnd() % (cap - n), i;
        int v = rnd() % 256, op = rnd() % 3;
        for (i = 0; i < cap; i += 64) buf[i] = (unsigned char)rnd();
        for (i = 0; i < cap; i++) ref[i] = buf[i];
        if (op == 0) {                                             /* memmove: overlap allowed */
            unsigned char *r = memmove(buf + d, buf + s, n);
            if (r != buf + d) FAIL("memmove return");
            if (s < d) for (i = n; i-- > 0;) ref[d + i] = ref[s + i]; else for (i = 0; i < n; i++) ref[d + i] = ref[s + i];
        } else if (op == 1) {                                      /* memcpy: no overlap */
            if ((s < d ? d - s : s - d) < n) continue;
            if (memcpy(buf + d, buf + s, n) != buf + d) FAIL("memcpy return");
            for (i = 0; i < n; i++) ref[d + i] = ref[s + i];
        } else {
            if (memset(buf + d, v, n) != buf + d) FAIL("memset return");
            for (i = 0; i < n; i++) ref[d + i] = (unsigned char)v;
        }
        for (i = 0; i < cap; i++) if (buf[i] != ref[i]) FAIL("op %d n %zu s %zu d %zu: byte %zu", op, n, s, d, i);
        checks++;
    }
    free(ref);
}

/* strings and copies that end on the last byte before an inaccessible page */
static void edges(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    char *m = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int k;
    if (m == MAP_FAILED || mprotect(m + pg, pg, PROT_NONE)) FAIL("mmap");
    for (k = 1; k < 200; k++) {
        char *s = m + pg - k, *t = m + pg - (k > 1 ? k - 1 : 1);
        memset(m, 'x', pg);
        s[k - 1] = 0;
        if (strlen(s) != (size_t)k - 1) FAIL("edge strlen %d", k);
        if (strchr(s, 'y') != NULL || strchr(s, 0) != s + k - 1) FAIL("edge strchr %d", k);
        if (strrchr(s, 'x') != (k > 1 ? s + k - 2 : NULL)) FAIL("edge strrchr %d", k);
        if (k > 1 && strcmp(s, t) <= 0) FAIL("edge strcmp %d", k);
        memset(s, 'q', (size_t)k);
        if (memchr(s, 'z', (size_t)k) != NULL || memchr(s, 'q', (size_t)k) != s) FAIL("edge memchr %d", k);
        memcpy(s, m, (size_t)k);
        if (memcmp(s, m, (size_t)k)) FAIL("edge memcpy %d", k);
        checks += 8;
    }
}

int main(void)
{
    size_t cap = 1 << 18;
    char *buf = malloc(cap);
    unsigned char *mb = malloc(1 << 18);
    strings(buf, cap);
    memory(mb, 1 << 16);
    edges();
    printf("libcfast ok %ld\n", checks);
    return 0;
}
