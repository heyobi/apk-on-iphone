/* pipe2 check for core/proc.c, freestanding (no libc): run with
 * `aoiproc build /pipes.elf`. Exit status 0 means every step passed; otherwise
 * the bits of the steps that failed.
 *   1  a read on an empty O_NONBLOCK pipe returns EAGAIN
 *   2  a blocking read waits while another thread sleeps, then gets its bytes */
typedef unsigned long u64;

static long sys(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static int fds[2];
static char stack[16384] __attribute__((aligned(16)));

/* the writer thread: sleep 20 ms, write 3 bytes, exit (not exit_group) */
void writer(void)
{
    u64 ts[2] = { 0, 20000000 };
    sys(101, (long)ts, 0, 0, 0, 0);                                    /* nanosleep */
    sys(64, fds[1], (long)"abc", 3, 0, 0);                             /* write */
    sys(93, 0, 0, 0, 0, 0);                                            /* exit */
}

/* clone a thread on `stack` that runs writer(); the child never returns here */
static long spawn(void)
{
    register long x8 __asm__("x8") = 220, x0 __asm__("x0") = 0x50f00;  /* VM|FS|FILES|SIGHAND|THREAD|SYSVSEM */
    register long x1 __asm__("x1") = (long)(stack + sizeof stack), x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0, x4 __asm__("x4") = 0;
    __asm__ volatile("svc #0\n cbnz x0, 1f\n bl writer\n 1:"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "x30");
    return x0;
}

void _start_c(void)
{
    long fail = 0, n;
    char buf[8] = { 0 };

    if (sys(59, (long)fds, 04000, 0, 0, 0) == 0) {                     /* pipe2(O_NONBLOCK) */
        if (sys(63, fds[0], (long)buf, 8, 0, 0) != -11) fail |= 1;     /* EAGAIN */
        sys(57, fds[0], 0, 0, 0, 0); sys(57, fds[1], 0, 0, 0, 0);
    } else fail |= 1;

    if (sys(59, (long)fds, 0, 0, 0, 0) == 0 && spawn() > 0) {         /* blocking pipe, writer thread */
        n = sys(63, fds[0], (long)buf, 8, 0, 0);
        if (n != 3 || buf[0] != 'a' || buf[2] != 'c') fail |= 2;
    } else fail |= 2;

    sys(94, fail, 0, 0, 0, 0);                                         /* exit_group */
}

__asm__(".globl _start\n _start: mov x29, #0\n bl _start_c\n");
