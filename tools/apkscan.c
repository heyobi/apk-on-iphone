/* apkscan: report what an AArch64 Linux/Android ELF needs before it can run on iOS.
 * usage: apkscan FILE.so [FILE...]      (tools/apkscan.py unpacks an APK first) */
#include "../core/elf.h"
#include "../core/scan.h"

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
        (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf) *size = (size_t)n;
    fclose(f);
    return buf;
}

static int report(const char *path)
{
    struct aoi_elf elf;
    struct aoi_scan scan;
    const char *err;
    size_t size = 0;
    void *data = slurp(path, &size);
    int k;

    if (!data) { fprintf(stderr, "%s: cannot read\n", path); return 1; }
    if ((err = aoi_elf_parse(&elf, data, size))) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(data);
        return 1;
    }
    aoi_scan_elf(&elf, &scan);
    printf("%s\n", path);
    printf("  type %s, %d PT_LOAD, span 0x%" PRIx64 "-0x%" PRIx64 ", min align 0x%" PRIx64 "%s\n",
           elf.is_shared ? "shared/PIE" : "static exec", elf.nseg, elf.min_vaddr, elf.max_vaddr,
           elf.min_align, elf.min_align >= 0x4000 ? "" : "  <- below iOS's 16 KB page");
    printf("  %" PRIu64 " instructions scanned\n", scan.insns);
    for (k = 0; k < AOI_KIND_COUNT; k++)
        if (scan.count[k])
            printf("  %8" PRIu64 "  %s (first at 0x%" PRIx64 ")\n", scan.count[k],
                   aoi_site_name(k), scan.first[k]);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0;

    if (argc < 2) { fprintf(stderr, "usage: %s FILE.so [FILE...]\n", argv[0]); return 2; }
    for (i = 1; i < argc; i++) rc |= report(argv[i]);
    return rc;
}
