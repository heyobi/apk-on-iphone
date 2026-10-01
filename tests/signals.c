/* Signal delivery check for core/proc.c, freestanding (no libc): run with
 * `aoiproc build /signals.elf`. Exit status 0 means every step passed; otherwise
 * the bits of the steps that failed.
 *   1  SIGSEGV from a bad load reaches the handler, on the altstack, with si_addr
 *   2  the handler edits the saved context (pc += 4, x0 = 42) and rt_sigreturn obeys
 *   4  tgkill to itself runs the SIGUSR1 handler before tgkill returns to the caller
 *   8  a blocked SIGUSR1 stays pending until rt_sigprocmask unblocks it */
typedef unsigned long u64;

static long sys(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

__asm__(".globl restorer\n restorer: mov x8, #139\n svc #0\n");   /* rt_sigreturn */
void restorer(void);

struct ksigaction { u64 handler, flags, restorer, mask; };
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTORER 0x04000000

static volatile u64 segv_addr, segv_on_alt, usr1_count;
static char altstack[16384] __attribute__((aligned(16)));

static void on_segv(int sig, void *info, void *uc)
{
    char probe;
    u64 *mc = (u64 *)((char *)uc + 176);           /* mcontext: fault, x0..x30, sp, pc */
    (void)sig;
    segv_addr = *(u64 *)((char *)info + 16);
    segv_on_alt = (u64)&probe >= (u64)altstack && (u64)&probe < (u64)altstack + sizeof altstack;
    mc[1] = 42;                                     /* x0 */
    mc[33] += 4;                                    /* pc: skip the faulting load */
}

static void on_usr1(int sig, void *info, void *uc) { (void)sig; (void)info; (void)uc; usr1_count++; }

static long bad_load(void)
{
    register long x0 __asm__("x0") = 7, x1 __asm__("x1") = 0x10;
    __asm__ volatile("ldr x0, [x1]" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}

void _start_c(void)
{
    struct ksigaction sa = { (u64)on_segv, SA_SIGINFO | SA_ONSTACK | SA_RESTORER, (u64)restorer, 0 };
    u64 ss[3] = { (u64)altstack, 0, sizeof altstack }, set = 1UL << (10 - 1);
    long fail = 0, tid = sys(178, 0, 0, 0, 0);                         /* gettid */

    sys(132, (long)ss, 0, 0, 0);                                       /* sigaltstack */
    sys(134, 11, (long)&sa, 0, 8);                                     /* rt_sigaction(SIGSEGV) */
    sa.handler = (u64)on_usr1; sa.flags = SA_SIGINFO | SA_RESTORER;
    sys(134, 10, (long)&sa, 0, 8);                                     /* rt_sigaction(SIGUSR1) */

    if (bad_load() != 42) fail |= 2;
    if (segv_addr != 0x10 || !segv_on_alt) fail |= 1;

    sys(131, sys(172, 0, 0, 0, 0), tid, 10, 0);                        /* tgkill(self, SIGUSR1) */
    if (usr1_count != 1) fail |= 4;

    sys(135, 0, (long)&set, 0, 8);                                     /* block SIGUSR1 */
    sys(131, sys(172, 0, 0, 0, 0), tid, 10, 0);
    if (usr1_count != 1) fail |= 8;                                    /* not yet */
    sys(135, 1, (long)&set, 0, 8);                                     /* unblock: delivered on return */
    if (usr1_count != 2) fail |= 8;

    sys(94, fail, 0, 0, 0);                                            /* exit_group */
}

__asm__(".globl _start\n _start: mov x29, #0\n bl _start_c\n");
