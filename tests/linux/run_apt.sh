#!/bin/sh
# Linux userland mode on an apt distribution's official arm64 root other than the bundled
# Debian (Ubuntu): apt with its signature checks (gpgv), a package installed. On a failed
# check the syscalls of a gpgv run that fail are printed. ROOT from tests/linux/fetch-image.sh.
# usage: tests/linux/run_apt.sh ROOT NAME
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1; N=$2
L="$DIR/build/lxrun"
fail=0
check() {   # check NAME EXPECTED-SUBSTRING TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    out="$(LXRUN_QUIET=1 timeout "$to" "$L" "$R" "$@" 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then echo "OK   $N: $name"
    else echo "FAIL $N: $name (rc=$rc)"; fail=1; printf '%s\n' "$out" | tail -n 20 | sed 's/^/     | /'; fi
}
cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
check "os-release"      "NAME="       30   /bin/bash -c 'cat /etc/os-release'
check "gpgv keyring"    "gpgv"        60   /bin/bash -c 'gpgv --keyring /usr/share/keyrings/ubuntu-archive-keyring.gpg /dev/null 2>&1; echo gpgv'
check "apt-get update"  "Reading"     1200 /bin/bash -c 'apt-get update 2>&1 | tail -n 6; apt-get update 2>&1 | grep -q "^[EW]:" && exit 1; echo Reading'
check "apt-get install" "Hello"       1200 /bin/bash -c 'apt-get install -y hello 2>&1 | tail -n 3; hello'
if [ $fail -ne 0 ]; then
    echo "== failing syscalls of gpgv"
    LXRUN_QUIET=1 timeout 120 "$L" -t "$R" /bin/bash -c 'mkdir -p /tmp/g && gpgv --homedir /tmp/g --keyring /usr/share/keyrings/ubuntu-archive-keyring.gpg /dev/null' 2>&1 |
        grep -E "= -[0-9]+|not impl" | grep -v "= -11$" | tail -n 60
fi
exit $fail
