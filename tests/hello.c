/* A freestanding AArch64 Linux program: no libc. Writes a line and exits,
 * using raw svc syscalls — exactly what a bionic program does at the bottom. */
static long sys3(long nr, long a, long b, long c) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static unsigned slen(const char *s){unsigned n=0;while(s[n])n++;return n;}
void _start(void){
    const char *msg = "hello from guest aarch64 (interpreted, no JIT)\n";
    /* a little arithmetic so the run exercises more than syscalls */
    long sum = 0; for (int i = 1; i <= 100; i++) sum += i;   /* 5050 */
    sys3(64, 1, (long)msg, slen(msg));                       /* write(1, msg, len) */
    sys3(93, sum == 5050 ? 0 : 1, 0, 0);                     /* exit(0 if arithmetic ok) */
    for(;;){}
}
