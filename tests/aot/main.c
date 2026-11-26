/* Driver for the ahead-of-time translation's differential test (tests/aot/run.sh): calls every
 * function of libfuzz.so (tests/aot/fuzzgen.py) with many random register states and buffers
 * and prints, per function, a hash of the states and buffers they leave. Run natively, under the
 * interpreter and with libfuzz.so translated, the three outputs must be the same. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void (*fuzz_table[])(uint64_t *state, uint8_t *buf);
extern int fuzz_count;

static uint64_t seed;
static uint64_t next(void)                       /* splitmix64 */
{
    uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Values that find the edges: zero, all ones, sign bits, small numbers, 32-bit boundaries. */
static uint64_t value(void)
{
    static const uint64_t edge[] = { 0, 1, 2, 0xffffffffffffffffULL, 0x8000000000000000ULL, 0x7fffffffffffffffULL,
                                     0x80000000ULL, 0x7fffffffULL, 0xffffffffULL, 0x100000000ULL, 0xff, 0x8000, 63, 64 };
    uint64_t r = next();
    switch (r & 7) {
    case 0: return edge[(r >> 8) % (sizeof edge / sizeof edge[0])];
    case 1: return (r >> 8) & 0xffff;
    case 2: return (uint64_t)(int64_t)(int32_t)next();
    default: return next();
    }
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 0x100000001b3ULL; }
    return h;
}

int main(int argc, char **argv)
{
    int calls = argc > 1 ? atoi(argv[1]) : 40, f, k;
    static uint8_t buf[4096] __attribute__((aligned(16)));
    uint64_t st[32];
    for (f = 0; f < fuzz_count; f++) {
        uint64_t h = 0xcbf29ce484222325ULL;
        seed = 0x1234567ULL + (uint64_t)f * 1000003ULL;
        for (k = 0; k < calls; k++) {
            int i;
            for (i = 0; i < 32; i++) st[i] = value();
            st[31] = (next() >> 60) << 28;                /* NZCV */
            for (i = 0; i < (int)sizeof buf; i += 8) { uint64_t v = value(); memcpy(buf + i, &v, 8); }
            fuzz_table[f](st, buf);
            st[18] = 0; st[28] = st[29] = st[30] = 0;   /* (not the function's: x18, buf, fp, lr) */
            h = fnv(h, st, sizeof st);
            h = fnv(h, buf, sizeof buf);
        }
        printf("fuzz_%d %016llx\n", f, (unsigned long long)h);
    }
    return 0;
}
