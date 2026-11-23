#!/bin/sh
# The workloads LxTerm measures on the phone (lxios/main.m, -measure), run on the host
# with build/lxrun: wall time, and guest instructions of the main process.
# usage: tests/linux/bench.sh ROOT   (an Alpine root with python3)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
run() {   # run NAME PROGRAM ARGS...
    name=$1; shift
    t0=$(date +%s.%N)
    stats=$(LXRUN_STATS=1 LXRUN_QUIET=1 "$L" "$R" "$@" 2>&1 >/dev/null | grep '^\[lxrun\] exit' | tail -n 1)
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    ins=$(echo "$stats" | sed -n 's/.*, \([0-9]*\) instructions, \([0-9.]*\) s.*/\1 \2/p')
    set -- $ins
    if [ -n "${1:-}" ] && [ "${2:-0}" != "0.00" ]; then mips=$(echo "$1 / $2 / 1000000" | bc); else mips="?"; fi
    printf 'BENCH %-34s %7.3f s   %s M instructions/s (main process)\n' "$name" "$t" "$mips"
}
run "process start (sh -c true)"     /bin/sh -c true
run "python3 -c pass"                /usr/bin/python3 -c pass
run "python3 1M-iteration loop"      /usr/bin/python3 -c 'import time
t=time.time()
s=0
for i in range(1000000): s+=i
print(time.time()-t)'
run "sh while-loop 100k"             /bin/sh -c 'i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done'
run "sha256sum of 16 MiB"            /bin/sh -c 'dd if=/dev/zero bs=1M count=16 2>/dev/null | sha256sum'
