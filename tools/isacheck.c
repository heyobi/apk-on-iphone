/* isacheck: run every distinct instruction word of real code once on our CPU
 * and once on Unicorn, from several random register states, and report per
 * mnemonic which words are missing (our CPU says UNDEF) or wrong (results differ).
 *
 * usage: tools/isawords.py lib.so... > words.txt; isacheck words.txt [-v mnemonic]
 * Built with the oracle (make build/isacheck, needs pip install unicorn). */
#include "../core/cpu.h"
#include "../core/oracle.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE 0x5a00000000ULL          /* far from small random store addresses */
#define DATA_BASE 0x20000000ULL
#define DATA_SIZE (2ULL << 20)
#define TRIALS 4

struct word { uint32_t insn; char mn[24]; unsigned long count; int result; char what[16]; };
enum { OK, MISSING, WRONG, SKIPPED };

struct mn { char name[24]; unsigned long words[4], uses[4]; };

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int skip(uint32_t w)
{
    if ((w & 0xffffffe0u) == 0xd53bd040u || (w & 0xffffffe0u) == 0xd51bd040u) return 0;  /* tpidr_el0 */
    if ((w & 0xffc00000u) == 0xd5000000u) return 1;    /* system: hints (bti/pac), barriers, sysregs */
    if ((w & 0xff000000u) == 0xd4000000u) return 1;    /* svc/hvc/brk: exceptions */
    if ((w & 0xffff0000u) == 0x00000000u) return 1;    /* udf */
    return 0;
}

static void randomize(struct aoi_cpu *c, int trial)
{
    int i;
    uint64_t ptr = DATA_BASE + DATA_SIZE / 2;
    for (i = 0; i < 31; i++) {
        uint64_t p = ptr + (rnd() & 0xfff0), r = rnd();
        switch (trial) {
        case 0: c->x[i] = p; break;                                    /* all pointers */
        case 1: c->x[i] = (i & 1) ? r & 0x3f : p; break;               /* pointers + small ints */
        case 2: c->x[i] = r; break;                                     /* random bits */
        default: c->x[i] = (i & 1) ? p : (r & 1 ? r >> (r & 63) : -(r & 0xff)); break;
        }
        c->vreg[i][0] = rnd(); c->vreg[i][1] = rnd();
    }
    c->vreg[31][0] = rnd(); c->vreg[31][1] = rnd();
    c->sp = ptr + 0x8000;
    c->n = rnd() & 1; c->z = rnd() & 1; c->c = rnd() & 1; c->v = rnd() & 1;
    c->tpidr = ptr;
}

int main(int argc, char **argv)
{
    static struct aoi_mem mem;
    struct aoi_cpu cpu;
    struct word *w = NULL;
    struct mn *m = NULL;
    size_t nw = 0, cap = 0, nm = 0, i, j;
    const char *verbose = NULL, *err;
    uint8_t *code, *data;
    char line[128];
    FILE *f;
    unsigned long tot[4] = {0}, totuse[4] = {0};

    if (argc < 2) { fprintf(stderr, "usage: %s words.txt [-v mnemonic]\n", argv[0]); return 2; }
    if (argc > 3 && !strcmp(argv[2], "-v")) verbose = argv[3];
    if (!(f = fopen(argv[1], "r"))) { perror(argv[1]); return 1; }
    while (fgets(line, sizeof line, f)) {
        if (nw == cap) { cap = cap ? cap * 2 : 4096; w = realloc(w, cap * sizeof *w); }
        memset(&w[nw], 0, sizeof *w);
        if (sscanf(line, "%x %23s %lu", &w[nw].insn, w[nw].mn, &w[nw].count) == 3) nw++;
    }
    fclose(f);

    code = aligned_alloc(0x4000, ((nw * 4) | 0x3fff) + 1);
    data = aligned_alloc(0x4000, DATA_SIZE);
    for (i = 0; i < nw; i++) memcpy(code + 4 * i, &w[i].insn, 4);
    for (j = 0; j < DATA_SIZE; j += 8) { uint64_t r = rnd(); memcpy(data + j, &r, 8); }
    mem.n = 2;
    mem.r[0].base = CODE_BASE; mem.r[0].size = ((nw * 4) | 0x3fff) + 1; mem.r[0].host = code;
    mem.r[1].base = DATA_BASE; mem.r[1].size = DATA_SIZE; mem.r[1].host = data;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = &mem;
    if ((err = aoi_oracle_attach(&cpu))) { fprintf(stderr, "oracle: %s\n", err); return 1; }

    for (i = 0; i < nw; i++) {
        int t, ran = 0, res = OK;
        if (skip(w[i].insn)) { w[i].result = SKIPPED; continue; }
        for (t = 0; t < TRIALS && res != WRONG; t++) {
            enum aoi_stop st;
            memcpy(code + 4 * i, &w[i].insn, 4);         /* a random store may have hit it */
            randomize(&cpu, t);
            cpu.pc = CODE_BASE + 4 * i;
            aoi_oracle_reset(1);
            st = aoi_cpu_run(&cpu, cpu.steps + 1);
            if (!aoi_oracle_ref_ran()) continue;         /* Unicorn faulted too: no verdict */
            ran = 1;
            if (aoi_oracle_mismatch()) { res = WRONG; snprintf(w[i].what, sizeof w[i].what, "%s", aoi_oracle_what()); }
            else if (st == AOI_STOP_UNDEF) res = MISSING;
        }
        w[i].result = ran ? res : SKIPPED;
    }

    for (i = 0; i < nw; i++) {
        for (j = 0; j < nm && strcmp(m[j].name, w[i].mn); j++) {}
        if (j == nm) { m = realloc(m, (nm + 1) * sizeof *m); memset(&m[nm], 0, sizeof *m); strcpy(m[nm++].name, w[i].mn); }
        m[j].words[w[i].result]++; m[j].uses[w[i].result] += w[i].count;
        tot[w[i].result]++; totuse[w[i].result] += w[i].count;
        if (verbose && !strcmp(verbose, w[i].mn) && (w[i].result == MISSING || w[i].result == WRONG))
            printf("  %08x %s %s\n", w[i].insn, w[i].result == WRONG ? "WRONG  " : "missing", w[i].what);
    }
    printf("%-12s %9s %9s %9s %9s   (static uses in the code)\n", "mnemonic", "ok", "missing", "WRONG", "skipped");
    for (j = 0; j < nm; j++)
        if (m[j].uses[MISSING] || m[j].uses[WRONG])
            printf("%-12s %9lu %9lu %9lu %9lu\n", m[j].name, m[j].uses[OK], m[j].uses[MISSING], m[j].uses[WRONG], m[j].uses[SKIPPED]);
    printf("%-12s %9lu %9lu %9lu %9lu   (%zu distinct words)\n", "TOTAL", totuse[OK], totuse[MISSING], totuse[WRONG], totuse[SKIPPED], nw);
    return 0;
}
