#!/bin/sh
# Stage 1 checks: single-process programs in the wasm32 core under node (build/lxrun.js).
# usage: tests/wasm/run.sh ROOT   (an Alpine root from tests/linux/fetch-alpine.sh)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
fail=0
check() {   # check NAME EXPECTED TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s.%N)
    out="$(LXRUN_STATS=1 timeout "$to" node "$DIR/build/lxrun.js" "$R" "$@" 2>&1)"; rc=$?
    secs=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then echo "OK   wasm: $name (${secs}s)"
    else echo "FAIL wasm: $name (rc=$rc, ${secs}s)"; fail=1; fi
    printf '%s\n' "$out" | tail -n 6 | sed 's/^/     > /'
}
check "busybox echo"      "hello"     60  /bin/busybox echo hello
check "busybox ls /"      "etc"       60  /bin/busybox ls /
check "busybox uname"     "aarch64"   60  /bin/busybox uname -m
check "cat os-release"    "Alpine"    60  /bin/busybox cat /etc/os-release
check "sha256sum"         "  /etc/os-release" 60 /bin/busybox sha256sum /etc/os-release
check "sh builtins"       "42"        60  /bin/busybox sh -c 'echo $((6*7))'
# the accelerator benchmark (static, no libc), for the interpreter's speed on a Wasm host
mkdir -p "$R/accel"
gcc -O2 -static -nostdlib -fno-builtin -mgeneral-regs-only -fno-pic -no-pie -mbranch-protection=none \
    -o "$R/accel/bench" "$DIR/tests/accel/bench.c"
check "accel bench (wasm interpreter)" "b73dbdcb3015bb01" 600 /accel/bench
# multi-process (fork/exec, pipes): guest processes run on host threads today, which the
# web view does not have; informational until the single-thread scheduler exists
LXRUN_QUIET=1 timeout 60 node "$DIR/build/lxrun.js" "$R" /bin/busybox sh -c 'ls / | wc -l' > /tmp/mp.txt 2>&1; echo "INFO wasm: sh pipeline (fork) rc=$?: $(head -c 300 /tmp/mp.txt | tr '
' ' ')"
exit $fail
