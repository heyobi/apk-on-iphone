#!/bin/sh
# glibc's string functions on the host (core/lxfast.c): tests/linux/libcfast.c built on
# the runner, run natively (the reference) and under lxrun in a glibc ROOT, with and
# without the routing; then the same workloads' timings.
# usage: tests/linux/run_libcfast.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
set -e
cc -O1 -fno-builtin -o "$R/tmp/libcfast" "$DIR/tests/linux/libcfast.c"
echo "native:   $("$R/tmp/libcfast")"
out=$(LXRUN_STATS=1 LXRUN_QUIET=1 timeout 900 "$L" "$R" /tmp/libcfast 2>&1) || true
echo "$out" | sed 's/^/lxrun:    /'
echo "$out" | grep -q "^libcfast ok" || { echo "FAIL: libcfast under lxrun"; exit 1; }
echo "$out" | grep -q "libc on the host: [1-9]" || { echo "FAIL: no call was routed"; exit 1; }
out=$(AOI_LXFAST=0 LXRUN_QUIET=1 timeout 900 "$L" "$R" /tmp/libcfast 2>&1) || true
echo "AOI_LXFAST=0: $out"
echo "$out" | grep -q "^libcfast ok" || { echo "FAIL: libcfast interpreted"; exit 1; }
