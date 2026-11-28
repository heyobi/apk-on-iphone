#!/bin/sh
# Exploration: the CPython test files that fail under lxrun, one by one, verbose: each failing
# test's name and the last lines of its traceback.
# usage: sudo sh tools/explore/pyfail.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3-test' > /dev/null 2>&1
for t in test_termios test_tty test_signal test_os test_posix test_time test_selectors test_epoll test_io test_subprocess; do
    LXRUN_QUIET=1 timeout 400 build/lxrun "$R" /bin/bash -c "cd /tmp && python3 -m test -v $t" > /tmp/v.out 2>&1
    echo "######## $t (rc=$?)"
    awk '/^(ERROR|FAIL): /{if(n){print "   ...";} name=$0; n=0; print; next} name && /^[A-Za-z_.]*(Error|Exception|Warning)[:(]|^AssertionError|^OSError|^    raise|Error: \[Errno/{ if(n<3){print "      "$0; n++} }' /tmp/v.out | head -n 60
done
