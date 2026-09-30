/* gmpdemo: load the real libgmp.so from an Android APK into the interpreter and
 * compute n! with it. usage: gmpdemo libgmp.so [n] */
#include "../core/bionic.h"
#include "../core/cpu.h"
#include "../core/dl.h"
#ifdef AOI_ORACLE
#include "../core/oracle.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *buf = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) *size = (size_t)n;
    fclose(f);
    return buf;
}

static int check(struct aoi_cpu *cpu, enum aoi_stop st, const char *what)
{
#ifdef AOI_ORACLE
    if (st != AOI_STOP_RETURN) aoi_oracle_report(cpu, st);
#endif
    if (st == AOI_STOP_RETURN) return 0;
    fprintf(stderr, "[gmpdemo] %s: ", what);
    switch (st) {
    case AOI_STOP_UNDEF:  fprintf(stderr, "undefined instruction 0x%08x at pc=0x%" PRIx64 "\n", cpu->fault_insn, cpu->pc); break;
    case AOI_STOP_FAULT:  fprintf(stderr, "fault at 0x%" PRIx64 " (pc=0x%" PRIx64 ")\n", cpu->fault_addr, cpu->pc); break;
    case AOI_STOP_IMPORT: fprintf(stderr, "missing import %s (called from 0x%" PRIx64 ")\n", cpu->stop_name, cpu->x[30]); break;
    case AOI_STOP_EXIT:   fprintf(stderr, "guest exited %d\n", cpu->exit_code); break;
    default:              fprintf(stderr, "stopped (%d) at pc=0x%" PRIx64 "\n", (int)st, cpu->pc); break;
    }
    return 1;
}

int main(int argc, char **argv)
{
    static struct aoi_dl dl;
    struct aoi_cpu cpu;
    const char *err;
    size_t size = 0;
    void *so;
    unsigned long n = argc > 2 ? strtoul(argv[2], NULL, 10) : 50;
    uint64_t z, str, strbuf, a[3];
    uint64_t f_init, f_fac, f_get;
    uint8_t *p;

    if (argc < 2) { fprintf(stderr, "usage: %s libgmp.so [n]\n", argv[0]); return 2; }
    if (!(so = slurp(argv[1], &size))) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    if ((err = aoi_bionic_init(&dl))) { fprintf(stderr, "init: %s\n", err); return 1; }
    if ((err = aoi_dl_load(&dl, "libgmp.so", so, size))) { fprintf(stderr, "load: %s\n", err); return 1; }

    f_init = aoi_dl_sym(&dl, "__gmpz_init");
    f_fac  = aoi_dl_sym(&dl, "__gmpz_fac_ui");
    f_get  = aoi_dl_sym(&dl, "__gmpz_get_str");
    if (!f_init || !f_fac || !f_get) { fprintf(stderr, "GMP symbols not found\n"); return 1; }
    fprintf(stderr, "[gmpdemo] libgmp.so at 0x%" PRIx64 ", %u imports bound\n", dl.lib[0].base, dl.nslots - 1);

    aoi_dl_cpu(&dl, &cpu);
#ifdef AOI_ORACLE
    if ((err = aoi_oracle_attach(&cpu))) { fprintf(stderr, "oracle: %s\n", err); return 1; }
#endif
    z = aoi_dl_malloc(&dl, 16);                       /* mpz_t: {int alloc; int size; mp_limb_t *d} */
    a[0] = z;                   if (check(&cpu, aoi_call(&cpu, f_init, a, 1, 0), "mpz_init")) return 1;
    a[0] = z; a[1] = n;         if (check(&cpu, aoi_call(&cpu, f_fac, a, 2, 0), "mpz_fac_ui")) return 1;
    strbuf = aoi_dl_malloc(&dl, 4096);               /* caller-provided output buffer */
    a[0] = strbuf; a[1] = 10; a[2] = z;
    if (check(&cpu, aoi_call(&cpu, f_get, a, 3, 0), "mpz_get_str")) return 1;
    str = cpu.x[0];

    printf("%lu! = ", n);
    while ((p = aoi_mem_ptr(&dl.mem, str++, 1)) && *p) putchar(*p);
    putchar('\n');
    fprintf(stderr, "[gmpdemo] %" PRIu64 " guest instructions interpreted\n", cpu.steps);
#ifdef AOI_ORACLE
    aoi_oracle_report(&cpu, AOI_STOP_RETURN);
#endif
    return 0;
}
