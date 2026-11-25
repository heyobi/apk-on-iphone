#!/bin/sh
# Linux userland mode on an RPM distribution's official arm64 root (Fedora, AlmaLinux):
# glibc, bash, coreutils, rpm, dnf. Programs run through bash: AlmaLinux's coreutils-single
# commands are #! scripts, which lxrun's first exec does not follow (the guest's execve does). ROOT from tests/linux/fetch-image.sh.
# usage: tests/linux/run_rpm.sh ROOT NAME [net]   (net: also dnf install)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1; N=$2
L="$DIR/build/lxrun"
fail=0
check() {   # check NAME EXPECTED-SUBSTRING TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s.%N)
    out="$(LXRUN_QUIET=1 timeout "$to" "$L" "$R" "$@" 2>&1)"; rc=$?
    secs=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then
        echo "OK   $N: $name (${secs}s)"
    else
        echo "FAIL $N: $name (rc=$rc, ${secs}s)"; fail=1
        printf '%s\n' "$out" | tail -n 20 | sed 's/^/     | /'
    fi
    printf '%s\n' "$out" | head -n 8 | sed 's/^/     > /'
}
check "os-release"       "NAME="          30  /bin/bash -c 'cat /etc/os-release'
check "ld.so --version"  "libc"          30  /lib/ld-linux-aarch64.so.1 --version
check "bash"             "bash 5"         30  /bin/bash -c 'echo bash ${BASH_VERSINFO[0]}'
check "ls -la /"         "etc"            30  /bin/bash -c 'ls -la /'
check "uname -m"         "aarch64"        30  /bin/bash -c 'uname -m'
check "id"               "uid=0(root)"    30  /bin/bash -c id
check "pipeline"         "3"              30  /bin/bash -c 'ls /etc | head -n 3 | wc -l'
check "login shell"      "login-ok"       60  /bin/bash -l -c 'echo login-ok'
if [ $fail -ne 0 ]; then                                          # where a login shell (LxTerm's tab) waits
    echo "== the login shell's last syscalls"
    LXRUN_QUIET=1 timeout 40 "$L" -t "$R" /bin/bash -l -c 'echo login-ok' 2>&1 | grep -E "^\[sys\]|exec|open " | tail -n 40
fi
check "rpm -q bash"      "bash-"          60  /usr/bin/rpm -q bash
check "rpm -qa count"    "pkgs"           120 /bin/bash -c 'echo "$(rpm -qa | wc -l) pkgs"'
if [ -x "$R/usr/bin/zypper" ]; then                               # openSUSE
check "zypper --version" "zypper"         120 /bin/bash -c 'zypper --version 2>&1 | head -n 2; echo zypper'
if [ "${3:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "zypper refresh"       "repositor"      2400 /bin/bash -c 'zypper --non-interactive refresh 2>&1 | tail -n 6'
    check "zypper install tmux"  "tmux "          2400 /bin/bash -c 'zypper --non-interactive install --no-recommends tmux 2>&1 | tail -n 6; tmux -V'
    check "zypper install python3" "499999500000" 2400 /bin/bash -c 'zypper --non-interactive install --no-recommends python3 2>&1 | tail -n 4; python3 -c "print(sum(range(10**6)))"'
fi
exit $fail
fi
check "dnf --version"    "dnf"            120 /bin/bash -c 'dnf --version 2>&1 | head -n 3; echo dnf'
if [ "${3:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "getent hosts"         "."              60   /usr/bin/getent hosts mirrors.fedoraproject.org
    check "dnf makecache"        "etadata"        2400 /bin/bash -c 'dnf -y makecache 2>&1 | tail -n 8'
    check "dnf install python3"  "Complete"       2400 /bin/bash -c 'dnf -y install --setopt=install_weak_deps=False python3 2>&1 | tail -n 12; command -v python3 && echo Complete'
    check "dnf install tmux"     "tmux "         2400 /bin/bash -c 'dnf -y install --setopt=install_weak_deps=False tmux 2>&1 | tail -n 6; tmux -V'
    check "python3"              "499999500000"   120  /bin/bash -c 'python3 -c "print(sum(range(10**6)))"'
    if [ -x "$R/usr/bin/systemd-sysusers" ]; then                  # rpm's %sysusers scriptlets
        check "systemd-sysusers" "sysusers-ok" 120 /bin/bash -c 'printf "u lxtest - \"test\" /tmp\n" | systemd-sysusers - 2>&1; getent passwd lxtest && echo sysusers-ok'
        echo "== failing syscalls of systemd-sysusers"
        LXRUN_QUIET=1 timeout 120 "$L" -t "$R" /bin/bash -c 'printf "u lxtest2 - \"t\" /tmp\n" | systemd-sysusers -' 2>&1 |
            grep -E "= -[0-9]+|not impl" | grep -v "= -11$" | tail -n 50
    fi
fi
exit $fail
