/* A compile-like guest workload for the accelerator prototype (tests/accel): it makes
 * a pseudo-random "source file", tokenizes it, counts identifiers in an open-addressing
 * hash table (string compares), sorts the table with a recursive quicksort and prints a
 * checksum. Freestanding (no libc): static aarch64 Linux binary, integer registers only,
 * so the same file runs natively, in the interpreter and translated to WebAssembly.
 * build: aarch64 gcc -O2 -static -nostdlib -fno-builtin -mgeneral-regs-only -fno-pic -no-pie */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

#define TEXT (1u << 21)
#define TAB (1u << 15)
static u8 text[TEXT];
static struct ent { u32 off, len, count, hash; } tab[TAB];
static u32 order[TAB];
static u32 seed = 12345;

static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

static u32 gen(void)
{
    static const char *const kw[] = { "int", "return", "static", "if", "else", "while", "for", "struct", "char", "void" };
    u32 n = 0;
    while (n < TEXT - 64) {
        u32 r = rnd() % 10;
        if (r < 3) {                                   /* keyword */
            const char *k = kw[rnd() % 10];
            while (*k) text[n++] = (u8)*k++;
        } else if (r < 8) {                            /* identifier from a smallish vocabulary */
            u32 id = rnd() % 5000, len = 3 + id % 9, i;
            u32 h = id * 2654435761u;
            for (i = 0; i < len; i++) { text[n++] = (u8)('a' + h % 26); h = h / 26 + id * 7; }
        } else {                                       /* number */
            u32 v = rnd() % 100000;
            do { text[n++] = (u8)('0' + v % 10); v /= 10; } while (v);
        }
        text[n++] = (u8)(rnd() % 4 ? ' ' : (rnd() % 2 ? ';' : '\n'));
    }
    return n;
}

static int same(u32 a, u32 b, u32 len)
{
    u32 i;
    for (i = 0; i < len; i++) if (text[a + i] != text[b + i]) return 0;
    return 1;
}

static u32 count_idents(u32 n)
{
    u32 i = 0, distinct = 0;
    while (i < n) {
        u8 ch = text[i];
        if (ch >= 'a' && ch <= 'z') {
            u32 s = i, h = 2166136261u, slot;
            while (i < n && text[i] >= 'a' && text[i] <= 'z') { h = (h ^ text[i]) * 16777619u; i++; }
            slot = h & (TAB - 1);
            for (;;) {
                struct ent *e = &tab[slot];
                if (!e->count) { e->off = s; e->len = i - s; e->count = 1; e->hash = h; distinct++; break; }
                if (e->hash == h && e->len == i - s && same(e->off, s, i - s)) { e->count++; break; }
                slot = (slot + 1) & (TAB - 1);
            }
        } else i++;
    }
    return distinct;
}

static int less(u32 a, u32 b)
{
    if (tab[a].count != tab[b].count) return tab[a].count > tab[b].count;
    return tab[a].hash < tab[b].hash;
}

static void qsort_idx(u32 *v, long lo, long hi)
{
    while (lo < hi) {
        u32 p = v[(lo + hi) / 2], t;
        long i = lo, j = hi;
        while (i <= j) {
            while (less(v[i], p)) i++;
            while (less(p, v[j])) j--;
            if (i <= j) { t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qsort_idx(v, lo, j); lo = i; }
        else { qsort_idx(v, i, hi); hi = j; }
    }
}

static void puthex(u64 v)
{
    char b[17];
    int i;
    for (i = 15; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[16] = '\n';
    sys3(64, 1, (long)b, 17);
}

void _start(void)
{
    u64 sum = 0;
    int round;
    for (round = 0; round < 30; round++) {
        u32 n = gen(), d, i, k = 0;
        for (i = 0; i < TAB; i++) tab[i].count = 0;
        d = count_idents(n);
        for (i = 0; i < TAB; i++) if (tab[i].count) order[k++] = i;
        qsort_idx(order, 0, (long)k - 1);
        for (i = 0; i < k && i < 64; i++) sum = sum * 31 + tab[order[i]].count * 7 + tab[order[i]].len;
        sum += d + n;
    }
    puthex(sum);
    sys3(93, 0, 0, 0);
    for (;;) ;
}
