/* pipe2, eventfd and epoll check for core/proc.c, freestanding (no libc): run with
 * `aoiproc build /pipes.elf`. Exit status 0 means every step passed; otherwise
 * the bits of the steps that failed.
 *   1  a read on an empty O_NONBLOCK pipe returns EAGAIN
 *   2  a blocking read waits while another thread sleeps, then gets its bytes
 *   4  epoll on an eventfd: nothing ready with timeout 0; a blocking wait is woken
 *      by another thread's eventfd write and returns the registered data; the
 *      read takes the counter
 *   8  timerfd (Chromium's message loop): a 10 ms monotonic timer, 5 ms period: a
 *      non-blocking read before it is due is EAGAIN; epoll waits until it is due;
 *      a blocking read after 30 ms more reads several expirations; gettime shows the
 *      interval; disarmed, a non-blocking read is EAGAIN again
 *  16  fallocate (Realm's posix_fallocate): a new file grows to 8 KiB; KEEP_SIZE
 *      leaves it so; a hole punch is EOPNOTSUPP
 *  32  a FIFO (mknodat, Realm's notifications): opened O_RDWR|O_NONBLOCK, empty is
 *      EAGAIN, then reads back what was written
 *  64  truncate by path (WhatsApp's logs): a file of 8 KiB is cut to 100 bytes; and
 *      /proc/sys/kernel/random/boot_id (libcutils' ashmem) reads as a UUID
 * 128  preadv at an offset, sendfile into a pipe, an inotify fd that takes a watch and
 *      is never readable, wait4 with no children (ECHILD) */
typedef unsigned long u64;

static long sys(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static int fds[2], efd;
static char stack[16384] __attribute__((aligned(16)));

/* the writer thread: sleep 20 ms, write 3 bytes, exit (not exit_group) */
void writer(void)
{
    u64 ts[2] = { 0, 20000000 };
    sys(101, (long)ts, 0, 0, 0, 0);                                    /* nanosleep */
    sys(64, fds[1], (long)"abc", 3, 0, 0);                             /* write */
    sys(93, 0, 0, 0, 0, 0);                                            /* exit */
}

/* the eventfd thread: sleep 20 ms, add 5, exit */
void poker(void)
{
    u64 ts[2] = { 0, 20000000 }, v = 5;
    sys(101, (long)ts, 0, 0, 0, 0);
    sys(64, efd, (long)&v, 8, 0, 0);
    sys(93, 0, 0, 0, 0, 0);
}

/* clone a thread on `stack` that runs writer() or poker(); the child never returns here */
static long spawn(int poke)
{
    register long x8 __asm__("x8") = 220, x0 __asm__("x0") = 0x50f00;  /* VM|FS|FILES|SIGHAND|THREAD|SYSVSEM */
    register long x1 __asm__("x1") = (long)(stack + sizeof stack), x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0, x4 __asm__("x4") = 0;
    if (poke)
        __asm__ volatile("svc #0\n cbnz x0, 1f\n bl poker\n 1:"
                         : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "x30");
    else
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

    if (sys(59, (long)fds, 0, 0, 0, 0) == 0 && spawn(0) > 0) {         /* blocking pipe, writer thread */
        n = sys(63, fds[0], (long)buf, 8, 0, 0);
        if (n != 3 || buf[0] != 'a' || buf[2] != 'c') fail |= 2;
    } else fail |= 2;

    {
        long ep = sys(20, 0, 0, 0, 0, 0);                              /* epoll_create1 */
        u64 ev[2] = { 1, 0x1234 }, out[2] = { 0, 0 }, v = 0;           /* EPOLLIN, data */
        efd = (int)sys(19, 0, 0, 0, 0, 0);                             /* eventfd2(0, 0) */
        if (ep < 0 || efd < 0 || sys(21, ep, 1, efd, (long)ev, 0) != 0) fail |= 4;   /* EPOLL_CTL_ADD */
        else if (sys(22, ep, (long)out, 1, 0, 0) != 0) fail |= 4;      /* timeout 0: nothing yet */
        else if (spawn(1) <= 0 || sys(22, ep, (long)out, 1, -1, 0) != 1 || out[1] != 0x1234) fail |= 4;
        else if (sys(63, efd, (long)&v, 8, 0, 0) != 8 || v != 5) fail |= 4;
    }

    {
        long tfd = sys(85, 1, 04000, 0, 0, 0);                         /* timerfd_create(MONOTONIC, NONBLOCK) */
        long ep = sys(20, 0, 0, 0, 0, 0);
        u64 it[4] = { 0, 5000000, 0, 10000000 }, cur[4], ev[2] = { 1, 7 }, out[2] = { 0, 0 }, v = 0;
        u64 ts[2] = { 0, 30000000 }, off[4] = { 0, 0, 0, 0 };
        if (tfd < 0 || ep < 0 || sys(86, tfd, 0, (long)it, 0, 0) != 0) fail |= 8;   /* timerfd_settime */
        else if (sys(63, tfd, (long)&v, 8, 0, 0) != -11) fail |= 8;    /* not due: EAGAIN */
        else if (sys(21, ep, 1, tfd, (long)ev, 0) != 0 || sys(22, ep, (long)out, 1, -1, 0) != 1 || out[1] != 7) fail |= 8;
        else if (sys(63, tfd, (long)&v, 8, 0, 0) != 8 || v < 1) fail |= 8;
        else {
            sys(101, (long)ts, 0, 0, 0, 0);                            /* 30 ms: about 6 more */
            if (sys(63, tfd, (long)&v, 8, 0, 0) != 8 || v < 3) fail |= 8;
            else if (sys(87, tfd, (long)cur, 0, 0, 0) != 0 || cur[1] != 5000000) fail |= 8;
            else if (sys(86, tfd, 0, (long)off, 0, 0) != 0 || sys(63, tfd, (long)&v, 8, 0, 0) != -11) fail |= 8;
        }
    }

    {
        u64 st[16];
        long fd = sys(56, -100, (long)"/fallocate.tmp", 0102 | 01000, 0644, 0);   /* openat O_RDWR|O_CREAT|O_TRUNC */
        if (fd < 0 || sys(47, fd, 0, 0, 8192, 0) != 0 || sys(80, fd, (long)st, 0, 0, 0) != 0 || st[6] != 8192) fail |= 16;
        else if (sys(47, fd, 1, 0, 65536, 0) != 0 || sys(80, fd, (long)st, 0, 0, 0) != 0 || st[6] != 8192) fail |= 16;
        else if (sys(47, fd, 3, 0, 4096, 0) != -95) fail |= 16;      /* PUNCH_HOLE|KEEP_SIZE */
        sys(57, fd, 0, 0, 0, 0);
        sys(35, -100, (long)"/fallocate.tmp", 0, 0, 0);                /* unlinkat */
    }

    {
        char b[4];
        long fd;
        if (sys(33, -100, (long)"/fifo.tmp", 0010000 | 0600, 0, 0) != 0) fail |= 32;   /* mknodat S_IFIFO */
        else if ((fd = sys(56, -100, (long)"/fifo.tmp", 02 | 04000, 0, 0)) < 0) fail |= 32;
        else {
            if (sys(63, fd, (long)b, 4, 0, 0) != -11) fail |= 32;      /* EAGAIN */
            else if (sys(64, fd, (long)"xy", 2, 0, 0) != 2 || sys(63, fd, (long)b, 4, 0, 0) != 2 || b[1] != 'y') fail |= 32;
            sys(57, fd, 0, 0, 0, 0);
        }
        sys(35, -100, (long)"/fifo.tmp", 0, 0, 0);
    }

    {
        u64 st[16];
        char id[64];
        long fd = sys(56, -100, (long)"/truncate.tmp", 0102 | 01000, 0644, 0);
        if (fd < 0 || sys(47, fd, 0, 0, 8192, 0) != 0) fail |= 64;
        else if (sys(45, (long)"/truncate.tmp", 100, 0, 0, 0) != 0 || sys(80, fd, (long)st, 0, 0, 0) != 0 || st[6] != 100) fail |= 64;
        if (fd >= 0) sys(57, fd, 0, 0, 0, 0);
        sys(35, -100, (long)"/truncate.tmp", 0, 0, 0);
        fd = sys(56, -100, (long)"/proc/sys/kernel/random/boot_id", 0, 0, 0);
        if (fd < 0 || sys(63, fd, (long)id, sizeof id, 0, 0) != 37 || id[8] != '-' || id[36] != '\n') fail |= 64;
        if (fd >= 0) sys(57, fd, 0, 0, 0, 0);
    }

    {
        long fd = sys(56, -100, (long)"/vec.tmp", 0102 | 01000, 0644, 0);
        int pf[2];
        char a[4] = { 0 }, b[4] = { 0 }, got[8];
        u64 iov[4] = { (u64)a, 3, (u64)b, 3 }, off = 2;
        long in;
        if (fd < 0 || sys(64, fd, (long)"abcdefghij", 10, 0, 0) != 10) fail |= 128;
        else if (sys(69, fd, (long)iov, 2, 2, 0) != 6 || a[0] != 'c' || b[2] != 'h') fail |= 128;   /* preadv at 2 */
        else if (sys(59, (long)pf, 0, 0, 0, 0) != 0) fail |= 128;                                    /* pipe2 */
        else if (sys(71, pf[1], fd, (long)&off, 4, 0) != 4 || off != 6) fail |= 128;                 /* sendfile */
        else if (sys(63, pf[0], (long)got, 8, 0, 0) != 4 || got[0] != 'c' || got[3] != 'f') fail |= 128;
        if ((in = sys(26, 04000, 0, 0, 0, 0)) < 0) fail |= 128;                                       /* IN_NONBLOCK */
        else if (sys(27, in, (long)"/", 0x100, 0, 0) < 1 || sys(63, in, (long)got, 8, 0, 0) != -11) fail |= 128;
        if (sys(260, -1, 0, 0, 0, 0) != -10) fail |= 128;                                             /* wait4: ECHILD */
        sys(35, -100, (long)"/vec.tmp", 0, 0, 0);
    }

    sys(94, fail, 0, 0, 0, 0);                                         /* exit_group */
}

__asm__(".globl _start\n _start: mov x29, #0\n bl _start_c\n");
