/* Flat guest address space: mapping, protection, faults, and running code in it. */
#include "../core/cpu.h"
#include "../core/vm.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } } while (0)

int main(void)
{
    static struct aoi_vm vm;
    struct aoi_mem mem;
    struct aoi_cpu cpu;
    uint64_t a, b;
    uint8_t *p;
    /* add x0, x0, #1; str x0, [x1]; ret */
    static const uint32_t code[] = { 0x91000400u, 0xf9000020u, 0xd65f03c0u };

    CHECK(!aoi_vm_init(&vm, 1ULL << 32), "reserve 4 GiB");
    a = aoi_vm_map(&vm, 0, 3 * AOI_VM_PAGE, AOI_PROT_R | AOI_PROT_W, 0);
    CHECK(a < (1ULL << 32) && !(a % AOI_VM_PAGE), "anonymous map below 4 GiB");
    CHECK((p = aoi_vm_ptr(&vm, a, 3 * AOI_VM_PAGE, AOI_PROT_W)) && !p[0] && !p[3 * AOI_VM_PAGE - 1], "zeroed and writable");
    memset(p, 0xab, 3 * AOI_VM_PAGE);
    CHECK(!aoi_vm_unmap(&vm, a + AOI_VM_PAGE, AOI_VM_PAGE), "unmap middle page");
    CHECK(!aoi_vm_ptr(&vm, a + AOI_VM_PAGE, 1, 0), "unmapped page faults");
    CHECK(aoi_vm_ptr(&vm, a, 1, AOI_PROT_R) && p[0] == 0xab, "neighbour keeps its data");
    CHECK(!aoi_vm_ptr(&vm, a + AOI_VM_PAGE - 4, 8, AOI_PROT_R), "access across into a hole faults");
    b = aoi_vm_map(&vm, a + AOI_VM_PAGE, AOI_VM_PAGE, AOI_PROT_R | AOI_PROT_W, 1);
    CHECK(b == a + AOI_VM_PAGE && p[AOI_VM_PAGE] == 0 && p[0] == 0xab, "fixed remap is zero, neighbour intact");
    CHECK(!aoi_vm_protect(&vm, a, AOI_VM_PAGE, AOI_PROT_R), "mprotect read-only");
    CHECK(!aoi_vm_ptr(&vm, a, 1, AOI_PROT_W) && aoi_vm_ptr(&vm, a, 1, AOI_PROT_R), "write refused, read allowed");
    CHECK(aoi_vm_protect(&vm, 0x100000, AOI_VM_PAGE, AOI_PROT_R) == -12, "mprotect of unmapped is ENOMEM");

    /* run code from an R+X page that writes into an R+W page */
    memset(&mem, 0, sizeof mem);
    mem.vm = &vm;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = &mem;
    b = aoi_vm_map(&vm, 0x400000, AOI_VM_PAGE, AOI_PROT_R | AOI_PROT_W, 1);
    memcpy(aoi_vm_ptr(&vm, b, sizeof code, AOI_PROT_W), code, sizeof code);
    aoi_vm_protect(&vm, b, AOI_VM_PAGE, AOI_PROT_R | AOI_PROT_X);
    cpu.thunk_base = 0x1000; cpu.thunk_slots = 1;            /* returning to 0x1000 = done */
    {
        uint64_t args[2] = { 41, a + 2 * AOI_VM_PAGE };
        enum aoi_stop st = aoi_call(&cpu, b, args, 2, 100);
        CHECK(st == AOI_STOP_RETURN && cpu.x[0] == 42, "guest code ran from an X page");
        CHECK(*(uint64_t *)aoi_vm_ptr(&vm, a + 2 * AOI_VM_PAGE, 8, AOI_PROT_R) == 42, "guest store landed");
    }
    {
        uint64_t args[2] = { 1, b };                            /* store into its own code page */
        enum aoi_stop st = aoi_call(&cpu, b, args, 2, 100);
        CHECK(st == AOI_STOP_FAULT && cpu.fault_addr == b, "store to a read-only code page faults");
    }
    {
        uint64_t args[2] = { 1, a };
        enum aoi_stop st = aoi_call(&cpu, a + 2 * AOI_VM_PAGE, args, 2, 100);
        CHECK(st == AOI_STOP_FAULT, "executing a non-X page faults");
    }
    aoi_vm_free(&vm);
    if (!fails) printf("OK  flat guest address space (map/unmap/protect/faults/execute)\n");
    return fails != 0;
}
