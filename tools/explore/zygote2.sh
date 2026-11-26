#!/bin/sh
# Exploration: dnf's fast start through the core (aoi_proc_zygote, tests/linux/zygote-dnf.py):
# exec of /usr/bin/dnf-3 resumes a saved process. Plain and fast runs must print the same;
# an install (the package database changes) must make the next run save a new snapshot.
# usage: sudo sh tools/explore/zygote2.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
mkdir -p "$R/usr/lib/lxzygote" && cp tests/linux/zygote-dnf.py "$R/usr/lib/lxzygote/zygote-dnf.py"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y makecache' > /dev/null 2>&1
RULE=/usr/bin/dnf-3:/usr/bin/python3:/usr/lib/lxzygote/zygote-dnf.py:/usr/lib/python3.12/site-packages,/usr/lib64/python3.12/site-packages,/usr/lib/python3.12/site-packages/dnf,/usr/lib64/python3.12/site-packages/libdnf,/usr/lib64/python3.12/site-packages/hawkey,/usr/lib64/python3.12/site-packages/rpm,/usr/lib64/python3.12
secs() { echo "$(date +%s.%N) - $1" | bc; }
z() {   # z LABEL COMMAND  (fast start on)
    t0=$(date +%s.%N)
    AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c "$2" > /tmp/z.out 2>&1; rc=$?
    printf '   %-40s %6.2f s rc=%s  snapshot: %s\n' "$1" "$(secs $t0)" "$rc" "$(ls -la "$R/var/cache/lxzygote/" 2>/dev/null | grep -c snap$) file(s)"
}
cmp_one() {   # cmp_one COMMAND
    t0=$(date +%s.%N)
    LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c "$1" > /tmp/plain.out 2>&1
    a=$(secs $t0)
    t0=$(date +%s.%N)
    AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c "$1" > /tmp/zyg.out 2>&1
    b=$(secs $t0)
    sed -i '/^Last metadata expiration check/d' /tmp/plain.out /tmp/zyg.out
    if cmp -s /tmp/plain.out /tmp/zyg.out; then same=same; else same=DIFFERENT; fi
    printf '%-44s plain %6.2f s   fast %6.2f s   output %s\n' "$1" "$a" "$b" "$same"
    [ $same = same ] || diff /tmp/plain.out /tmp/zyg.out | head -n 12 | sed 's/^/   | /'
}
echo "== first run saves, the next ones resume"
z "dnf --version (saves)" 'dnf --version'
z "dnf --version" 'dnf --version'
z "dnf --version" 'dnf --version'
echo "== plain vs fast"
cmp_one 'dnf --version'
cmp_one 'dnf -C search nginx'
cmp_one 'dnf -C info tmux'
cmp_one 'dnf -C list --installed | tail -n 5'
cmp_one 'dnf -C repoquery --qf "%{name} %{arch}" --requires httpd'
cmp_one 'yum -C list --installed bash'
cmp_one 'echo n | dnf -C install tmux; echo "rc=$?"'
cmp_one 'cd /etc && dnf -C --setopt=install_weak_deps=False --assumeno install tmux; pwd'
cmp_one 'DNF_VAR_releasever=10 LANG=C dnf -C repolist'
echo "== an unrelated install keeps the snapshot (it depends only on dnf's and Python's files)"
z "dnf install tmux" 'dnf -y --setopt=install_weak_deps=False install tmux > /dev/null 2>&1; rpm -q tmux'
cat /tmp/z.out | sed 's/^/   | /'
z "dnf --version (still fast)" 'dnf --version'
z "dnf --version" 'dnf --version'
cmp_one 'dnf -C list --installed tmux'
z "dnf remove tmux" 'dnf -y remove tmux > /dev/null 2>&1; rpm -q tmux'
cat /tmp/z.out | sed 's/^/   | /'
echo "== a change to what it depends on saves again"
touch "$R/usr/lib/python3.12/site-packages/dnf"
z "dnf --version (dnf's folder changed: saves)" 'dnf --version'
z "dnf --version" 'dnf --version'
echo "== without the script dnf runs as usual"
mv "$R/usr/lib/lxzygote/zygote-dnf.py" /tmp/zd.py
cmp_one 'dnf --version'
mv /tmp/zd.py "$R/usr/lib/lxzygote/zygote-dnf.py"
echo "   stamp:"; head -n 12 "$R/var/cache/lxzygote/dnf-3.stamp" | sed 's/^/   | /'; echo "   ($(wc -l < "$R/var/cache/lxzygote/dnf-3.stamp") lines)"
echo "== where the time goes (dnf --version, instructions)"
AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 LXRUN_STATS=1 build/lxrun "$R" /bin/bash -c 'dnf --version' 2>&1 >/dev/null | grep -E "exit|libc" | sed 's/^/   fast: /'
AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 LXRUN_STATS=1 build/lxrun "$R" /bin/bash -c 'true' 2>&1 >/dev/null | grep -E "exit" | sed 's/^/   bash -c true: /'
AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun -t "$R" /bin/bash -c 'dnf --version' 2>/tmp/trace >/dev/null; grep -E "execve|exec .* from|snapshot" /tmp/trace | head -n 8 | sed 's/^/   trace: /'
cat > "$R/tmp/prof.py" <<'PEOF'
import cProfile, pstats, sys, io
sys.argv = ["dnf", "--version"]
from dnf.cli import main
pr = cProfile.Profile(); pr.enable()
try:
    main.user_main(["--version"], exit_code=False)
except SystemExit:
    pass
pr.disable()
s = io.StringIO(); pstats.Stats(pr, stream=s).sort_stats("cumulative").print_stats(18); print(s.getvalue()[:3500])
PEOF
echo "   after imports, dnf --version spends (cProfile):"
LXRUN_QUIET=1 build/lxrun "$R" /usr/bin/python3 /tmp/prof.py 2>&1 | grep -v "^4.20\|Installed\|Built" | head -n 40 | sed 's/^/   | /'
echo "== AOI_ZYGOTE=0"
t0=$(date +%s.%N); AOI_ZYGOTE=0 AOI_ZYGOTE_RULE=$RULE LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf --version' > /dev/null 2>&1; echo "   $(secs $t0) s"
ls -la "$R/var/cache/lxzygote/" "$R/tmp/" | grep -i "lxzygote\|snap\|stamp"
