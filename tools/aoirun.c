/* aoirun: load a static AArch64 Linux ELF and run it in the interpreter.
 * usage: aoirun PROGRAM [args...] */
#include "../core/cpu.h"
#include "../core/elf.h"
#include "../core/load.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv)
{
    struct aoi_elf elf;
    struct aoi_image img;
    struct aoi_cpu cpu;
    const char *err;
    size_t size = 0;
    void *data;
    enum aoi_stop stop;

    if (argc < 2) { fprintf(stderr, "usage: %s PROGRAM [args...]\n", argv[0]); return 2; }
    if (!(data = slurp(argv[1], &size))) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    if ((err = aoi_elf_parse(&elf, data, size))) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
    if ((err = aoi_load(&img, &elf, 0x400000, argc - 1, (const char *const *)(argv + 1)))) {
        fprintf(stderr, "load: %s\n", err); return 1;
    }

    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = &img.mem;
    cpu.pc = img.entry;
    cpu.sp = img.sp;

    stop = aoi_cpu_run(&cpu, 100000000);

    switch (stop) {
    case AOI_STOP_EXIT:
        fprintf(stderr, "[aoirun] exit %d after %" PRIu64 " instructions\n", cpu.exit_code, cpu.steps);
        return cpu.exit_code;
    case AOI_STOP_UNDEF:
        fprintf(stderr, "[aoirun] undefined instruction 0x%08x at pc=0x%" PRIx64 " (step %" PRIu64 ")\n",
                cpu.fault_insn, cpu.pc, cpu.steps);
        return 3;
    case AOI_STOP_SYSCALL:
        fprintf(stderr, "[aoirun] unimplemented syscall %u at pc=0x%" PRIx64 "\n", cpu.fault_insn, cpu.pc);
        return 4;
    case AOI_STOP_FAULT:
        fprintf(stderr, "[aoirun] fault at guest addr 0x%" PRIx64 " (pc=0x%" PRIx64 ")\n", cpu.fault_addr, cpu.pc);
        return 5;
    default:
        fprintf(stderr, "[aoirun] stopped (step limit?) at pc=0x%" PRIx64 "\n", cpu.pc);
        return 6;
    }
}
