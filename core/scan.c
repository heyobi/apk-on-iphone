#include "scan.h"

#include <string.h>

const char *aoi_site_name(enum aoi_site_kind kind)
{
    static const char *const names[] = {
        "svc #0 (Linux syscall)",
        "mrs xN, tpidr_el0",
        "msr tpidr_el0, xN",
        "str x30, [x18], #8 (shadow stack push)",
        "ldr x30, [x18, #-8]! (shadow stack pop)",
    };
    return kind < AOI_KIND_COUNT ? names[kind] : "?";
}

enum aoi_site_kind aoi_classify(uint32_t insn)
{
    if (insn == 0xd4000001u) return AOI_SVC;
    if ((insn & ~0x1fu) == 0xd53bd040u) return AOI_MRS_TPIDR;
    if ((insn & ~0x1fu) == 0xd51bd040u) return AOI_MSR_TPIDR;
    if (insn == 0xf800865eu) return AOI_SCS_PUSH;
    if (insn == 0xf85f8e5eu) return AOI_SCS_POP;
    return AOI_KIND_COUNT;
}

void aoi_scan_elf(const struct aoi_elf *elf, struct aoi_scan *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < elf->nseg; i++) {
        const struct aoi_segment *s = &elf->seg[i];
        const uint8_t *p = elf->data + s->offset;
        uint64_t off;

        if (!s->exec) continue;
        for (off = 0; off + 4 <= s->filesz; off += 4) {
            uint32_t insn = (uint32_t)p[off] | (uint32_t)p[off + 1] << 8 |
                            (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;
            enum aoi_site_kind k = aoi_classify(insn);

            out->insns++;
            if (k == AOI_KIND_COUNT) continue;
            if (!out->count[k]) out->first[k] = s->vaddr + off;
            out->count[k]++;
        }
    }
}
