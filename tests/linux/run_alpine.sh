#!/bin/sh
# Linux userland mode on Alpine's minirootfs (musl, busybox, apk): build/lxrun runs
# the root's own programs in the interpreter. ROOT from tests/linux/fetch-alpine.sh.
# usage: tests/linux/run_alpine.sh ROOT [net]   (net: also apk add python3 and run it)
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
        echo "OK   alpine: $name (${secs}s)"
    else
        echo "FAIL alpine: $name (rc=$rc, ${secs}s)"; fail=1
        printf '%s\n' "$out" | tail -n 15 | sed 's/^/     | /'
        LXRUN_QUIET=1 AOI_CHILD_TRACE=/tmp/lx-a-child.trace timeout "$to" "$L" -t "$R" "$@" > /tmp/lx-a.trace 2>&1
        grep -v '^\[sched\]' /tmp/lx-a.trace | tail -n 25 | sed 's/^/     t /'
        cat /tmp/lx-a.trace /tmp/lx-a-child.trace 2>/dev/null | grep -E '= -(22|38|95|1|13)$|not impl|ptmx|/dev/pts|\[tty\]|\[unix\]|stopped' | head -n 30 | sed 's/^/     e /'
        rm -f /tmp/lx-a-child.trace
    fi
    printf '%s\n' "$out" | head -n 12 | sed 's/^/     > /'
}
check "busybox echo"   "hello"             20  /bin/busybox echo hello
check "sh arithmetic"  "42"                20  /bin/sh -c 'echo $((6*7))'
check "ls /"           "etc"               20  /bin/ls /
check "cat"            "3."                20  /bin/cat /etc/alpine-release
check "uname -a"       "aarch64"           20  /bin/uname -a
check "sh: external"   "alpine-release"    20  /bin/sh -c '/bin/ls /etc'
check "sh: pipe"       "3"                 20  /bin/sh -c 'ls /etc | head -n 3 | wc -l'
check "sh: subshell"   "sub ok"            20  /bin/sh -c 'x=$(echo sub); echo "$x ok"'
check "sh: exit code"  "rc=3"              20  /bin/sh -c '(exit 3); echo rc=$?'
check "sh: script"     "done 5"            20  /bin/sh -c 'i=0; while [ $i -lt 5 ]; do i=$((i+1)); done; echo done $i'
check "id"             "uid=0(root)"       20  /usr/bin/id
check "ps"             "PID"               20  /bin/ps
check "apk --version"  "apk-tools"         20  /sbin/apk --version
check "find | sort"    "/etc/passwd"       30  /bin/sh -c 'find /etc -name passwd | sort'
check "tar roundtrip"  "hostname"          30  /bin/sh -c 'cd /tmp && tar -cf t.tar /etc/hostname 2>/dev/null; tar -tf t.tar'
# The terminal (core/tty.c): an interactive ash, keys typed as on the phone.
out="$(LXRUN_QUIET=1 LXRUN_KEYS='echo hi-$((40+2))\r\wsleep 30\r\w\x03\wecho after-ctrl-c\r\wexit\r' timeout 60 "$L" -i "$R" /bin/sh -i 2>&1)"; rc=$?
if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "hi-42" && printf '%s' "$out" | grep -q "after-ctrl-c"; then
    echo "OK   alpine: interactive sh, Ctrl-C"
else
    echo "FAIL alpine: interactive sh, Ctrl-C (rc=$rc)"; fail=1
fi
printf '%s\n' "$out" | tail -n 12 | sed 's/^/     > /'
# Job control: Ctrl-Z stops the job, jobs lists it, kill ends it.
out="$(LXRUN_QUIET=1 LXRUN_KEYS='sleep 30\r\w\w\x1a\w\wjobs\r\wkill %1\r\wecho jc-done\r\wexit\r' timeout 60 "$L" -i "$R" /bin/sh -i 2>&1)"; rc=$?
if printf '%s' "$out" | grep -qi "stopped" && printf '%s' "$out" | grep -q "jc-done"; then echo "OK   alpine: Ctrl-Z, jobs"
else echo "FAIL alpine: Ctrl-Z, jobs (rc=$rc)"; fail=1; fi
printf '%s\n' "$out" | tail -n 8 | sed 's/^/     > /'
check "ps sees processes" "sleep"           20  /bin/sh -c 'sleep 3 & ps'
check "top -bn1"         "Mem:"             20  /bin/sh -c 'top -bn1 | head -n 5'
check "free"             "Mem:"             20  /usr/bin/free
check "uptime"           "load average"     20  /usr/bin/uptime
check "/proc/self/stat"  "R"                20  /bin/cat /proc/self/stat
if [ "${2:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "apk update"      "OK:"           300 /sbin/apk update
    check "apk add python3" "OK:"           600 /sbin/apk add python3
    check "python3"         "499999500000"  120 /usr/bin/python3 -c 'print(sum(range(10**6)))'
    check "apk add tmux openssh" "OK:"       600 /sbin/apk add tmux openssh-client util-linux-misc
    check "script: a pty"     "/dev/pts/"    60  /bin/sh -c 'script -qc tty /dev/null'
    check "tmux session"      "1 windows"    90  /bin/sh -c 'tmux new-session -d "sleep 30"; sleep 2; tmux ls; tmux kill-server'
    check "ssh to github"     "Permission denied (publickey)" 90 /bin/sh -c 'ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -T git@github.com 2>&1; true'
    check "python3 script"  "sha256 ok"     120 /usr/bin/python3 -c 'import hashlib,json,os; h=hashlib.sha256(b"abc").hexdigest(); print("sha256", "ok" if h.startswith("ba7816bf") else h, json.dumps(os.uname().machine))'
fi
exit $fail
