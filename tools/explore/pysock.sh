#!/bin/sh
# Exploration: test_socket under lxrun, verbose: which tests fail, grouped by their error.
# usage: sudo sh tools/explore/pysock.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3-test' > /dev/null 2>&1
LXRUN_QUIET=1 timeout 1500 build/lxrun "$R" /bin/bash -c "cd /tmp && python3 -m test -v test_socket; echo EXIT=\$?" > /tmp/v.out 2>&1
echo "== exit"; grep -E "EXIT=|^Ran |^(OK|FAILED)" /tmp/v.out
echo "== failing test classes (count)"
grep -E "^(ERROR|FAIL): " /tmp/v.out | sed -E 's/.*\(test\.test_socket\.([A-Za-z0-9_]+)\..*/\1/' | sort | uniq -c | sort -rn | head -n 40
echo "== error kinds (count)"
awk '/^(ERROR|FAIL): /{f=1; next} f && /^[A-Za-z_.]*(Error|Exception)[:(]|^AssertionError/{print substr($0,1,150); f=0}' /tmp/v.out | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -n 40
echo "== first 12 failures in full"
awk '/^(ERROR|FAIL): /{n++} n>=1 && n<=12' /tmp/v.out | grep -vE "^  File .*unittest|^    ~|^    \^" | head -n 200
