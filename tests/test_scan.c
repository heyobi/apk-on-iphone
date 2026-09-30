#include "../core/elf.h"
#include "../core/scan.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    struct aoi_elf elf;
    struct aoi_scan scan;
    size_t size;
    void *data;
    static const unsigned char not_elf[64] = "hello";

    /* encodings, checked independently of the assembler */
    CHECK(aoi_classify(0xd4000001) == AOI_SVC);
    CHECK(aoi_classify(0xd4000021) == AOI_KIND_COUNT);   /* svc #1 */
    CHECK(aoi_classify(0xd53bd040 | 5) == AOI_MRS_TPIDR);
    CHECK(aoi_classify(0xd51bd040 | 30) == AOI_MSR_TPIDR);
    CHECK(aoi_classify(0xd53bd060) == AOI_KIND_COUNT);   /* mrs x0, tpidrro_el0 */
    CHECK(aoi_classify(0xd503201f) == AOI_KIND_COUNT);   /* nop */

    CHECK(aoi_elf_parse(&elf, not_elf, sizeof(not_elf)) != NULL);

    if (argc < 2 || !(data = slurp(argv[1], &size))) { printf("FAIL: fixture missing\n"); return 1; }
    CHECK(aoi_elf_parse(&elf, data, size) == NULL);
    CHECK(!elf.is_shared);
    CHECK(elf.nseg >= 1);
    aoi_scan_elf(&elf, &scan);
    CHECK(scan.count[AOI_SVC] == 2);
    CHECK(scan.count[AOI_MRS_TPIDR] == 2);
    CHECK(scan.count[AOI_MSR_TPIDR] == 1);
    CHECK(scan.count[AOI_SCS_PUSH] == 1);
    CHECK(scan.count[AOI_SCS_POP] == 1);
    CHECK(scan.first[AOI_SVC] > scan.first[AOI_MSR_TPIDR]);

    /* truncated file must be rejected, not read past */
    CHECK(aoi_elf_parse(&elf, data, 100) != NULL);
    free(data);

    printf(failures ? "%d failure(s)\n" : "all tests passed\n", failures);
    return failures != 0;
}
