#!/bin/sh
# Exploration: single CPython tests that hang under lxrun, each alone for 90 s with a system
# call trace: the last calls before the hang.
# usage: sudo sh tools/explore/pyhang.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3-test' > /dev/null 2>&1
run() {   # FILE TEST
    echo "######## $1 $2"
    LXRUN_QUIET=1 timeout 90 build/lxrun "$R" /bin/bash -c "cd /tmp && python3 -m test -v $1 -m $2 2>&1 | tail -n 25" 2>&1 | cut -c1-200 | sed 's/^/  | /'
    echo "  --- trace (last calls)"
    LXRUN_QUIET=1 timeout 60 build/lxrun -t "$R" /bin/bash -c "cd /tmp && python3 -m test $1 -m $2" 2>&1 |
        grep -E "^\[(sys|sig|proc)\]" | grep -vE " (98|113|124)\(" | tail -n 40 | cut -c1-200 | sed 's/^/  > /'
}
run test_signal test_sigwait_thread
run test_signal test_sigpending
run test_signal test_sigtimedwait_timeout
run test.test_pydoc.test_pydoc test_server
run test_ssl test_handshake_timeout
run test_logging test_output
run test_logging test_udp
run test_tarfile test_realpath_limit_attack
