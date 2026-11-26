#!/bin/sh
# Exploration: dnf started from a saved process ("zygote") instead of from scratch. A Python
# process imports dnf's command line (what every dnf command loads first), asks the core
# for a snapshot (/dev/aoi_snapshot, core/snap.c) and, when resumed, reads the command's
# arguments from a file and runs dnf's main. Compared: time and output against plain dnf.
# usage: sudo sh tools/explore/zygote.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y makecache' > /dev/null 2>&1   # (metadata for -C runs)
cat > "$R/tmp/zygote.py" <<'EOF'
import os, sys, time
from dnf.cli import main                          # dnf, libdnf, hawkey, rpm, librepo...
open("/dev/aoi_snapshot").close()                 # saved at the next time slice...
while True:                                       # ...wait for it (EBUSY until taken)
    try:
        open("/dev/aoi_snapshot_pending").close()
    except OSError as e:
        if e.errno == 16:
            time.sleep(0.001); continue
    break
args = open("/tmp/zygote.args").read().split("\n")
args = [a for a in args if a]
if args == ["--saved"]:
    sys.exit(0)
sys.argv = ["dnf"] + args
main.user_main(args, exit_code=True)
EOF
secs() { echo "$(date +%s.%N) - $1" | bc; }

echo "== save"
echo "--saved" > "$R/tmp/zygote.args"
t0=$(date +%s.%N)
AOI_SNAPSHOT_SAVE=/tmp/dnf.snap LXRUN_QUIET=1 LXRUN_STATS=1 build/lxrun "$R" /usr/bin/python3 /tmp/zygote.py 2>&1 | tail -n 2
echo "   save run: $(secs $t0) s, snapshot $(du -h /tmp/dnf.snap | cut -f1)"

cmp_one() {   # cmp_one ARGS...
    printf '%s\n' "$@" > "$R/tmp/zygote.args"
    t0=$(date +%s.%N)
    LXRUN_QUIET=1 build/lxrun "$R" /usr/bin/dnf "$@" > /tmp/plain.out 2>&1
    a=$(secs $t0)
    t0=$(date +%s.%N)
    AOI_SNAPSHOT_LOAD=/tmp/dnf.snap LXRUN_QUIET=1 build/lxrun "$R" > /tmp/zyg.out 2>&1
    b=$(secs $t0)
    sed -i '/^Last metadata expiration check/d' /tmp/plain.out /tmp/zyg.out
    if cmp -s /tmp/plain.out /tmp/zyg.out; then same=same; else same=DIFFERENT; fi
    printf '%-28s plain %6.2f s   from snapshot %6.2f s   output %s\n' "dnf $*" "$a" "$b" "$same"
    [ $same = same ] || diff /tmp/plain.out /tmp/zyg.out | head -n 12 | sed 's/^/   | /'
}
echo "== plain dnf vs from the snapshot"
cmp_one --version
cmp_one -C search nginx
cmp_one -C info tmux
cmp_one -C list --installed
cmp_one -C repoquery --requires httpd
echo "== load alone"
echo "--saved" > "$R/tmp/zygote.args"
for k in 1 2 3; do
    t0=$(date +%s.%N); AOI_SNAPSHOT_LOAD=/tmp/dnf.snap LXRUN_QUIET=1 build/lxrun "$R" > /dev/null 2>&1; echo "   load + exit: $(secs $t0) s"
done
