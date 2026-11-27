#!/bin/sh
# Exploration: CPython's own test suite (python3 -m test) under lxrun on AlmaLinux: which test
# files fail or hang, and, for a few that hang, where (the last system calls of each process).
# usage: sudo sh tools/explore/pytests.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3-test' > /dev/null 2>&1
echo "== hangs, one by one (60 s each, traced)"
for t in test.test_asyncio.test_sock_lowlevel test.test_asyncio.test_sendfile test.test_concurrent_futures.test_process_pool; do
    rm -f /tmp/ct*
    AOI_CHILD_TRACE=/tmp/ct LXRUN_QUIET=1 timeout 60 build/lxrun -t "$R" /bin/bash -c "cd /tmp && python3 -m test -v $t" > /tmp/t.out 2> /tmp/t.trace
    echo "--- $t: rc=$?"
    grep -E "^(test_|ERROR|FAIL|OK|Ran )" /tmp/t.out | tail -n 15
    echo "   main process, last system calls:"; grep "^\[sys\]" /tmp/t.trace | tail -n 12 | sed 's/^/     /'
    [ -f /tmp/ct ] && { echo "   children, last system calls:"; grep "^\[sys\]" /tmp/ct | tail -n 12 | sed 's/^/     /'; }
done
echo "== the whole suite (-j4, 300 s per test file)"
LXRUN_QUIET=1 timeout 6000 build/lxrun "$R" /bin/bash -c 'cd /tmp && python3 -m test -j4 --timeout 300' > /tmp/all.out 2>&1
echo "rc=$?"
sed -n '/tests* OK\.\|tests* failed\|tests* skipped\|== Tests result\|Total duration\|Total tests\|Total test files/,$p' /tmp/all.out | head -n 120
