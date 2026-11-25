#!/bin/sh
# Linux userland mode on Arch Linux ARM (root from tests/linux/fetch-alarm.py): pacman's
# keyring set up, the base packages reinstalled with pacman's own signature checks, then
# python and tmux installed. On a failed check the failing syscalls of pacman are printed.
# usage: tests/linux/run_pacman.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1; N=archlinuxarm
L="$DIR/build/lxrun"
fail=0
check() {   # check NAME EXPECTED-SUBSTRING TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s)
    out="$(LXRUN_QUIET=1 timeout "$to" "$L" "$R" "$@" 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then echo "OK   $N: $name ($(( $(date +%s) - t0 ))s)"
    else echo "FAIL $N: $name (rc=$rc)"; fail=1; printf '%s\n' "$out" | tail -n 25 | sed 's/^/     | /'; fi
    printf '%s\n' "$out" | head -n 4 | sed 's/^/     > /'
}
cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
printf 'Server = https://de3.mirror.archlinuxarm.org/$arch/$repo
Server = https://fl.us.mirror.archlinuxarm.org/$arch/$repo
Server = https://ca.us.mirror.archlinuxarm.org/$arch/$repo
' > "$R/etc/pacman.d/mirrorlist"
check "bash"                 "bash 5"     30   /bin/bash -c 'echo bash ${BASH_VERSINFO[0]}; cat /etc/os-release | head -n 2'
check "pacman --version"     "Pacman v"   30   /bin/bash -c 'pacman --version'
# no Landlock or seccomp here (as in WSL1 and many containers): pacman's documented setting for that
grep -q '^DisableSandbox' "$R/etc/pacman.conf" || sed -i '/^\[options\]/a DisableSandbox' "$R/etc/pacman.conf"
grep -n '^\[' "$R/etc/pacman.conf" | sed 's/^/     repos: /'
check "systemd-sysusers"     "alpm:"      120  /bin/bash -c 'systemd-sysusers > /tmp/su.log 2>&1; tail -n 3 /tmp/su.log; getent passwd alpm; ls -la /usr/lib/os-release 2>&1; true'
check "update-ca-trust"      "ca-ok"      300  /bin/bash -c 'update-ca-trust 2>&1 | tail -n 3; test -s /etc/ssl/certs/ca-certificates.crt && echo ca-ok'
check "pacman-key --init"    "keyring-ok" 900  /bin/bash -c 'pacman-key --init 2>&1 | tail -n 3 && echo keyring-ok'
check "pacman-key --populate" "keyring-ok" 900 /bin/bash -c 'pacman-key --populate archlinuxarm 2>&1 | tail -n 3 && echo keyring-ok'
check "pacman -Sy"           "synced"     900  /bin/bash -c 'pacman -Sy --noconfirm > /tmp/sy.log 2>&1; r=$?; tail -n 5 /tmp/sy.log; [ $r = 0 ] && echo synced'
check "reinstall, signed"    "base-ok"    2400 /bin/bash -c 'pacman -S --noconfirm --overwrite "*" $(sed -n "s/^archlinuxarm //p" /.lxrun-image) 2>&1 | tail -n 8; pacman -Qk pacman 2>&1 | tail -n 1; pacman -Q | wc -l; echo base-ok'
check "pacman -S python"     "499999500000" 2400 /bin/bash -c 'pacman -S --noconfirm python 2>&1 | tail -n 4; python -c "print(sum(range(10**6)))"'
check "pacman -S tmux"       "tmux "      2400 /bin/bash -c 'pacman -S --noconfirm tmux 2>&1 | tail -n 3; tmux -V'
if [ $fail -ne 0 ]; then
    echo "== failing syscalls of pacman -Sy"
    LXRUN_QUIET=1 timeout 600 "$L" -t "$R" /bin/bash -c 'pacman -Sy --noconfirm' 2>&1 |
        grep -E "= -[0-9]+$|not impl" | grep -vE "= -(11|2|25)$" | sort | uniq -c | sort -rn | head -n 40
fi
exit $fail
