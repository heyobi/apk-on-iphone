/* The "Wine part": a handful of Linux/aarch64 syscalls served on the host.
 *
 * The guest calls svc #0 with the syscall number in x8 and arguments in x0..x5,
 * exactly as Linux defines them. We map the few a freestanding program needs
 * onto host behaviour. This is where an Android bionic program's futex, mmap,
 * openat, etc. will eventually land; for the first milestone, write/writev/exit
 * are enough to see output. */
#include "cpu.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Linux aarch64 syscall numbers */
#define NR_write    64
#define NR_writev   66
#define NR_exit     93
#define NR_exit_group 94

static uint64_t sys_write(struct aoi_cpu *c, int fd, uint64_t buf, uint64_t len)
{
    uint8_t *p = aoi_mem_ptr(c->mem, buf, len);
    if (!p) { c->stop = AOI_STOP_FAULT; c->fault_addr = buf; return (uint64_t)-14; }
    if (fd != 1 && fd != 2) return (uint64_t)-9;   /* only stdout/stderr for now */
    return (uint64_t)fwrite(p, 1, len, fd == 1 ? stdout : stderr);
}

uint64_t aoi_linux_syscall(struct aoi_cpu *c)
{
    uint64_t nr = c->x[8];
    switch (nr) {
    case NR_write:
        return sys_write(c, (int)c->x[0], c->x[1], c->x[2]);
    case NR_writev: {
        /* struct iovec { void *base; size_t len; } array at x1, count x2 */
        uint64_t iov = c->x[1], n = c->x[2], total = 0, i;
        for (i = 0; i < n; i++) {
            uint8_t *e = aoi_mem_ptr(c->mem, iov + i * 16, 16);
            uint64_t base, len;
            if (!e) { c->stop = AOI_STOP_FAULT; c->fault_addr = iov; return (uint64_t)-14; }
            memcpy(&base, e, 8); memcpy(&len, e + 8, 8);
            total += sys_write(c, (int)c->x[0], base, len);
            if (c->stop != AOI_RUN) break;
        }
        return total;
    }
    case NR_exit:
    case NR_exit_group:
        c->exit_code = (int)c->x[0];
        c->stop = AOI_STOP_EXIT;
        return 0;
    default:
        /* Unknown syscall: stop so the gap is visible rather than silently wrong. */
        c->stop = AOI_STOP_SYSCALL;
        c->fault_insn = (uint32_t)nr;
        return (uint64_t)-38;   /* -ENOSYS */
    }
}
