#!/bin/sh
# Linux userland mode on Debian's arm64 root (glibc, bash, coreutils, dpkg, apt):
# build/lxrun runs the root's own programs in the interpreter, as root.
# ROOT from tests/linux/fetch-debian.sh.
# usage: tests/linux/run_debian.sh ROOT [net]   (net: also apt update / apt install)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
fail=0
check() {   # check NAME EXPECTED-SUBSTRING TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s.%N)
    out="$(LXRUN_QUIET=1 timeout "$to" "$L" "$R" "$@" 2>&1)"; rc=$?
    t1=$(date +%s.%N)
    secs=$(echo "$t1 - $t0" | bc)
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then
        echo "OK   debian: $name (${secs}s)"
    else
        echo "FAIL debian: $name (rc=$rc, ${secs}s)"; fail=1
        printf '%s\n' "$out" | tail -n 15 | sed 's/^/     | /'
        AOI_CHILD_TRACE=/dev/null LXRUN_QUIET=1 timeout "$to" "$L" -t "$R" "$@" 2>&1 | grep -v '^\[sched\]' | tail -n 50 | sed 's/^/     t /'
    fi
    printf '%s\n' "$out" | head -n 12 | sed 's/^/     > /'
}
check "ld.so --version"  "GNU C Library"   30  /lib/ld-linux-aarch64.so.1 --version
check "bash"             "bash 5"          30  /bin/bash -c 'echo bash ${BASH_VERSINFO[0]}'
check "debian_version"   "13"              30  /bin/cat /etc/debian_version
check "ls -la /"         "etc"             30  /bin/ls -la /
check "uname -a"         "aarch64"         30  /bin/uname -a
check "bash: pipeline"   "3"               30  /bin/bash -c 'ls /etc | head -n 3 | wc -l'
check "bash: subst"      "x=etc ok"        30  /bin/bash -c 'x=$(basename /etc); echo "x=$x ok"'
check "id"               "uid=0(root)"     30  /usr/bin/id
check "dpkg --version"   "dpkg"            30  /usr/bin/dpkg --version
check "dpkg -l"          "ii  bash"        60  /usr/bin/dpkg -l bash
check "apt --version"    "apt"             30  /usr/bin/apt-get --version
check "sort | uniq"      "2 b"             30  /bin/bash -c 'printf "a\nb\nb\n" | sort | uniq -c | sed "s/^ *//" | grep b'
check "date"             "20"              30  /bin/date -u +%Y
check "perl"             "perl ok"         30  /usr/bin/perl -e 'print "perl ok\n"'
if [ "${2:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "apt update"             "Reading package lists" 900 /usr/bin/apt-get update
    check "apt install python3"    "Setting up python3"    1800 /usr/bin/apt-get install -y --no-install-recommends python3
    check "python3"                "499999500000"          120 /usr/bin/python3 -c 'print(sum(range(10**6)))'
    check "apt install git"        "Setting up git"        1800 /usr/bin/apt-get install -y --no-install-recommends git
    check "git"                    "git version"           60  /usr/bin/git --version
fi
exit $fail
