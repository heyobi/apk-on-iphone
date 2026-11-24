/* Bit and byte reversal (rbit, rev16, rev32, rev; w and x forms) in a hot loop, so the
 * accelerator translates them: the checksum must match the interpreter's.
 * build: aarch64 gcc -O2 -static -nostdlib -fno-builtin -mgeneral-regs-only -fno-pic -no-pie */
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void puthex(u64 v)
{
    char b[17];
    int i;
    for (i = 15; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[16] = '\n';
    sys3(64, 1, (long)b, 17);
}

void _start(void)
{
    u64 x = 0x0123456789abcdefull, sum = 0, t;
    unsigned w, u;
    int i;
    for (i = 0; i < 300000; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        w = (unsigned)(x >> 17);
        __asm__("rbit %0, %1" : "=r"(t) : "r"(x)); sum = sum * 3 + t;
        __asm__("rev %0, %1" : "=r"(t) : "r"(x)); sum ^= t;
        __asm__("rev16 %0, %1" : "=r"(t) : "r"(x)); sum += t;
        __asm__("rev32 %0, %1" : "=r"(t) : "r"(x)); sum = sum * 5 + t;
        __asm__("rbit %w0, %w1" : "=r"(u) : "r"(w)); sum += u;
        __asm__("rev %w0, %w1" : "=r"(u) : "r"(w)); sum ^= (u64)u << 7;
        __asm__("rev16 %w0, %w1" : "=r"(u) : "r"(w)); sum += u;
    }
    puthex(sum);
    sys3(93, 0, 0, 0);
    for (;;) ;
}
