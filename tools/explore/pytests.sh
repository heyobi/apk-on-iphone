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
for t in ${PYT_HANGS:-}; do
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
echo "--- failed test files, with their first error"
grep -E "\] test[^ ]* (failed|crashed|worker non-zero exit code|timed out)" /tmp/all.out | sed 's/^.*\] //' | sort -u
echo "--- first error lines per failure"
awk '/^(ERROR|FAIL): /{name=$0} /^[A-Za-z]*Error:|^AssertionError/{if(name){print name" -> "$0; name=""}}' /tmp/all.out | sort | uniq -c | sort -rn | head -n 120
echo "--- totals"; grep -E "^Total|^Result" /tmp/all.out
