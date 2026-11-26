#!/bin/sh
# Checks for dnf's fast start (aoi_proc_zygote, tests/linux/zygote-dnf.py) on an AlmaLinux
# root: the first dnf saves a snapshot, the next ones resume it (the snapshot file stays as it was), and
# every output matches plain dnf's; stdin, cwd and the environment reach the resumed dnf; an
# unrelated change keeps the snapshot, a change to dnf's folder replaces it; without the
# script, or with AOI_ZYGOTE=0, dnf runs as usual.
# usage: sudo sh tests/linux/run_zygote.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
fail=0
cp /etc/resolv.conf "$R/etc/resolv.conf"
mkdir -p "$R/usr/lib/lxzygote" && cp tests/linux/zygote-dnf.py "$R/usr/lib/lxzygote/zygote-dnf.py"
rm -rf "$R/var/cache/lxzygote"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q makecache' > /dev/null 2>&1
P=/usr/lib/python3.12/site-packages
RULE="/usr/bin/dnf-3:/usr/bin/python3:/usr/lib/lxzygote/zygote-dnf.py:$P,/usr/lib64/python3.12/site-packages,$P/dnf,/usr/lib64/python3.12/site-packages/libdnf,/usr/lib64/python3.12/site-packages/hawkey,/usr/lib64/python3.12/site-packages/rpm,/usr/lib64/python3.12"

snapid() { stat -c '%i %y' "$R/var/cache/lxzygote/dnf-3.snap" 2> /dev/null || echo none; }
fast() {   # fast COMMAND → /tmp/zf.out (stdout and stderr, as plain's); $resumed: the snapshot was used, not saved again
    before=$(snapid)
    AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c "$1" > /tmp/zf.out 2>&1
    after=$(snapid)
    if [ "$before" != none ] && [ "$before" = "$after" ]; then resumed=yes; else resumed=no; fi
}
plain() { LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c "$1" > /tmp/zp.out 2>&1; }
ok()  { echo "OK   zygote: $1"; }
bad() { echo "FAIL zygote: $1"; fail=1; }
same() {   # same COMMAND: resumed, and the output of plain dnf
    plain "$1"; fast "$1"
    sed -i '/^Last metadata expiration check/d' /tmp/zp.out /tmp/zf.out
    if [ $resumed = yes ] && cmp -s /tmp/zp.out /tmp/zf.out; then ok "$1"
    else bad "$1 (resumed: $resumed)"; diff /tmp/zp.out /tmp/zf.out | head -n 8 | sed 's/^/     | /'; fi
}

fast 'dnf --version'
if [ $resumed = no ] && [ -s "$R/var/cache/lxzygote/dnf-3.snap" ] && [ -s "$R/var/cache/lxzygote/dnf-3.stamp" ] && grep -q "^4\." /tmp/zf.out
then ok "the first run saves a snapshot"; else bad "the first run saves a snapshot (resumed: $resumed)"; fi
same 'dnf --version'
same 'dnf -C search nginx'
same 'dnf -C info tmux'
same 'yum -C list --installed bash'
same 'echo n | dnf -C install tmux; echo "rc=$?"'
same 'cd /etc && dnf -C --assumeno install tmux; pwd'
same 'LANG=C DNF_VAR_releasever=10 dnf -C repolist'
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'ldconfig; echo x >> /etc/hosts' > /dev/null 2>&1
fast 'dnf --version'
[ $resumed = yes ] && ok "an unrelated change (ldconfig, /etc) keeps the snapshot" || bad "an unrelated change keeps the snapshot"
touch "$R$P/dnf"
fast 'dnf --version'
r1=$resumed
fast 'dnf --version'
[ $r1 = no ] && [ $resumed = yes ] && ok "a change to dnf's folder saves a new snapshot" || bad "a change to dnf's folder saves a new snapshot ($r1, $resumed)"
mv "$R/usr/lib/lxzygote/zygote-dnf.py" /tmp/zd.py
fast 'dnf --version'
grep -q "^4\." /tmp/zf.out && ok "without the script dnf runs as usual" || bad "without the script dnf runs as usual"
mv /tmp/zd.py "$R/usr/lib/lxzygote/zygote-dnf.py"
ms() { date +%s%3N; }
t0=$(ms); AOI_ZYGOTE=0 AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf --version' > /tmp/zf.out 2>&1; off=$(( $(ms) - t0 ))
grep -q "^4\." /tmp/zf.out || bad "AOI_ZYGOTE=0: dnf runs"
fast 'dnf --version'
t0=$(ms); fast 'dnf --version'; on=$(( $(ms) - t0 ))
if [ $resumed = yes ] && [ $((on * 10)) -lt $((off * 6)) ]; then ok "resumed is faster than AOI_ZYGOTE=0 (${on} ms vs ${off} ms)"
else bad "resumed is faster than AOI_ZYGOTE=0 (${on} ms vs ${off} ms, resumed: $resumed)"; fi
exit $fail
