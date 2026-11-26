#define _GNU_SOURCE                 /* pread */
#define _DARWIN_C_SOURCE
/* Ahead-of-time translated guest libraries: matching a mapped file to a carried
 * translation (core/aot.h). The tables come from tools/aotgen.c's output, compiled in with
 * AOI_AOT; without it the list is empty and nothing changes. */
#include "aot.h"
#include "cpu.h"
#include "proc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t aoi_aot_runs, aoi_aot_insns;

#ifndef AOI_AOT
const struct aoi_aot_lib *const aoi_aot_libs[] = { NULL };
#endif

static int rd_at(int fd, uint64_t off, void *b, size_t n) { return pread(fd, b, n, (off_t)off) == (ssize_t)n; }

/* The file's NT_GNU_BUILD_ID (from its PT_NOTE segments) and its executable PT_LOAD
 * (offset, vaddr, filesz); 0 if it is not an aarch64 ELF64 with both. */
static int identify(int fd, uint8_t id[20], int *idlen, uint64_t x[3])
{
    uint8_t eh[64], ph[56], note[512];
    uint64_t phoff;
    uint16_t phentsize, phnum;
    int i, found = 0;
    *idlen = 0;
    if (!rd_at(fd, 0, eh, 64) || memcmp(eh, "\177ELF\2\1", 6) || *(uint16_t *)(eh + 18) != 183) return 0;
    memcpy(&phoff, eh + 32, 8); memcpy(&phentsize, eh + 54, 2); memcpy(&phnum, eh + 56, 2);
    for (i = 0; i < phnum; i++) {
        uint32_t type, flags;
        uint64_t off, vaddr, filesz;
        if (!rd_at(fd, phoff + (uint64_t)i * phentsize, ph, 56)) return 0;
        memcpy(&type, ph, 4); memcpy(&flags, ph + 4, 4);
        memcpy(&off, ph + 8, 8); memcpy(&vaddr, ph + 16, 8); memcpy(&filesz, ph + 32, 8);
        if (type == 1 && (flags & 1) && !found) { x[0] = off; x[1] = vaddr; x[2] = filesz; found = 1; }
        if (type == 4 && filesz <= sizeof note && rd_at(fd, off, note, (size_t)filesz)) {
            uint64_t p = 0;
            while (p + 12 <= filesz) {
                uint32_t namesz, descsz, ntype;
                memcpy(&namesz, note + p, 4); memcpy(&descsz, note + p + 4, 4); memcpy(&ntype, note + p + 8, 4);
                {
                    uint64_t name = p + 12, desc = name + ((namesz + 3) & ~3u);
                    if (desc + descsz > filesz) break;
                    if (ntype == 3 && namesz == 4 && !memcmp(note + name, "GNU", 4) && descsz <= 20) {
                        memcpy(id, note + desc, descsz); *idlen = (int)descsz;
                    }
                    p = desc + ((descsz + 3) & ~3u);
                }
            }
        }
    }
    return found && *idlen;
}

void aoi_aot_mapped(struct aoi_proc *p, uint64_t a, uint64_t len, uint64_t off, int fd)
{
    uint8_t id[20];
    uint64_t x[3], bias;
    int idlen, i, k;
    if (!aoi_aot_libs[0] || !p->linux_mode || p->cpu.naot >= AOI_AOT_MAX) return;
    {
        static int off = -1;                                       /* AOI_AOT=0: interpreted only */
        if (off < 0) { const char *e = getenv("AOI_AOT"); off = e && e[0] == '0'; }
        if (off) return;
    }
    if (!identify(fd, id, &idlen, x)) return;
    for (i = 0; aoi_aot_libs[i]; i++) {
        const struct aoi_aot_lib *L = aoi_aot_libs[i];
        if (L->build_id_len != idlen || memcmp(L->build_id, id, (size_t)idlen)) continue;
        if (x[0] != x[1]) return;                                  /* (translated with vaddr = file offset) */
        bias = a - off;
        if (L->text_lo < off || L->text_hi > off + len) return;   /* not this mapping's part of the file */
        for (k = -1; k < AOI_PROC_THREADS; k++) {
            struct aoi_cpu *c = k < 0 ? &p->cpu : &p->th[k].cpu;
            int n = c->naot;
            if (n >= AOI_AOT_MAX) continue;
            c->aot_lo[n] = bias + L->text_lo;
            c->aot_hi[n] = bias + L->text_hi;
            c->aot_biases[n] = bias;
            c->aot_lib[n] = L;
            c->naot = n + 1;
        }
        return;
    }
}
