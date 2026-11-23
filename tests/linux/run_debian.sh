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
        n=$((${n:-0}+1)); T=${LX_TRACES:-/tmp/lx-traces}; mkdir -p "$T"
        AOI_CHILD_TRACE=$T/$n-children.trace LXRUN_QUIET=1 timeout "$to" "$L" -t "$R" "$@" > "$T/$n-main.trace" 2>&1
        echo "$name: $*" > "$T/$n-what.txt"
        grep -v '^\[sched\]' "$T/$n-main.trace" | grep -E "exec|stopped|not impl|\[sig\]" | tail -n 30 | sed 's/^/     t /'
    fi
    printf '%s\n' "$out" | head -n 12 | sed 's/^/     > /'
}
check "ld.so --version"  "GLIBC"   30  /lib/ld-linux-aarch64.so.1 --version
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
check "rm -rf / refused"  "dangerous"      30  /bin/sh -c 'rm -rf / 2>&1; true'
check "/proc/meminfo"     "MemTotal"       30  /bin/cat /proc/meminfo
check "groupadd"          "lxgroup"        60  /bin/sh -c 'groupadd -r lxgroup; echo rc=$?; getent group lxgroup' 
out="$(LXRUN_QUIET=1 LXRUN_KEYS='sleep 30\r\w\w\x1a\w\wjobs\r\wfg\r\w\w\x03\wecho jc-done\r\wexit\r' timeout 60 "$L" -i "$R" /bin/bash -i 2>&1)"; rc=$?
if printf '%s' "$out" | grep -q "Stopped" && printf '%s' "$out" | grep -q "jc-done"; then echo "OK   debian: Ctrl-Z, jobs, fg, Ctrl-C"
else echo "FAIL debian: Ctrl-Z, jobs, fg, Ctrl-C (rc=$rc)"; fail=1; fi
printf '%s\n' "$out" | tail -n 10 | sed 's/^/     > /'
if [ "${2:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "getent hosts"           "deb.debian.org"        60  /usr/bin/getent hosts deb.debian.org
    check "apt update"             "Get:"                  900 /usr/bin/apt-get update
    # libc6 upgrades run debconf from the preinst (the phone failed with /usr/lib/cdebconf/debconf)
    check "debconf env"            "BOS"                   30  /bin/sh -c 'env | grep -i debconf; [ "$DEBCONF_USE_CDEBCONF" ] && echo DOLU || echo BOS'
    check "upgrade libc6"          "libc6"                 1200 /bin/sh -c 'apt-get install -y --only-upgrade libc6 libc-bin zlib1g 2>&1 | tail -n 15; dpkg -l libc6 | tail -n 1'
    check "apt install python3"    "Setting up python3"    1800 /usr/bin/apt-get install -y --no-install-recommends python3
    check "python3 files"          "python3.13"            30  /bin/sh -c 'ls -la /usr/bin/python3* /usr/bin/*.dpkg-new 2>&1; od -c -N 8 /usr/bin/python3.13'
    check "venv with pip"          "pip 2"                 900 /bin/sh -c 'apt-get install -y --no-install-recommends python3-venv >/dev/null 2>&1; t0=$(date +%s); python3 -m venv /tmp/venv 2>&1 | tail -n 5; echo "venv took $(( $(date +%s) - t0 )) s"; /tmp/venv/bin/pip --version'
    check "python3"                "499999500000"          120 /usr/bin/python3 -c 'print(sum(range(10**6)))'
    check "apt install git"        "Setting up git"        1800 /usr/bin/apt-get install -y --no-install-recommends git
    check "git"                    "git version"           60  /usr/bin/git --version
    check "apt install procps tmux ssh" "Setting up"       1800 /usr/bin/apt-get install -y --no-install-recommends procps tmux openssh-client ca-certificates
    check "addgroup (adduser)"    "lxgrp2"                60  /bin/sh -c 'addgroup --system --debug lxgrp2 2>&1 | tail -n 8; getent group lxgrp2' 
    check "ps aux"                 "PID"                   30  /bin/sh -c 'sleep 3 & ps aux'
    check "tmux session"           "1 windows"             90  /bin/sh -c 'tmux new-session -d "sleep 30"; sleep 2; tmux ls; tmux kill-server'
    # tmux attached to the terminal (as on the phone): the client hands its tty to the server
    T=${LX_TRACES:-/tmp/lx-traces}; mkdir -p "$T"
    out="$(LXRUN_QUIET=1 LXRUN_KEYS='tmux\r\w\w\w\wecho in-tmux-$((2*21))\r\w\w\wexit\r\w\w\wexit\r' timeout 90 "$L" -i "$R" /bin/bash -i 2>&1)"; rc=$?
    if printf '%s' "$out" | grep -q "in-tmux-42" && ! printf '%s' "$out" | grep -q "server exited"; then echo "OK   debian: tmux attached"
    else
        echo "FAIL debian: tmux attached (rc=$rc)"; fail=1
        printf '%s\n' "$out" | tr -d '\033' | tail -n 15 | sed 's/^/     | /'
        LXRUN_QUIET=1 AOI_CHILD_TRACE=$T/tmux-children.trace LXRUN_KEYS='tmux\r\w\w\w\wecho in-tmux\r\w\w\wexit\r\w\w\wexit\r' timeout 90 "$L" -t -i "$R" /bin/bash -i > "$T/tmux-main.trace" 2>&1
        cat "$T"/tmux-*.trace | grep -E "stopped|not impl|exec|\[unix\]|\[tty\]|= -(22|38|95|9|14)$" | head -n 40 | sed 's/^/     e /'
    fi
    check "ssh to github"          "Permission denied (publickey)" 90 /bin/sh -c 'ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -T git@github.com 2>&1; true'
    check "git clone (https)"      "README"                300 /bin/sh -c 'cd /tmp && git clone -q --depth 1 https://github.com/heyobi/lxterm.git && ls lxterm' 
fi
exit $fail
