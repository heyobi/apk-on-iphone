#!/bin/sh
# Where dnf's time goes under lxrun (AlmaLinux): guest instruction and syscall counts
# against the native run, the Python functions and imports that dominate (cProfile and
# -X importtime inside the guest), and the host-side hot spots of the interpreter (perf).
# usage: sudo tests/linux/profile_dnf.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
NR="$R.native"
rm -rf "$NR"; cp -a "$R" "$NR"
for r in "$R" "$NR"; do cp /etc/resolv.conf "$r/etc/resolv.conf"; done
mount -t proc proc "$NR/proc"; mount --rbind /dev "$NR/dev"; mount --rbind /sys "$NR/sys"
trap 'umount -R "$NR/dev" "$NR/sys" "$NR/proc" 2>/dev/null' EXIT
lx() { LXRUN_QUIET=1 timeout 2400 "$L" "$R" /bin/bash -c "$1"; }
lxs() { LXRUN_STATS=1 LXRUN_QUIET=1 timeout 2400 "$L" "$R" /bin/bash -c "$1" 2>&1 >/dev/null | grep '^\[lxrun\]'; }

echo "::group::warm the metadata cache (both)"
lx 'dnf -y makecache' | tail -n 3
chroot "$NR" dnf -y makecache | tail -n 3
echo "::endgroup::"

W1='dnf --version'
W2='dnf -y makecache'
W3='dnf -C search nginx'
for w in "$W1" "$W2" "$W3" 'python3 -c pass'; do
    echo "== $w"
    echo "lxrun:  $(lxs "$w")"
    if command -v perf >/dev/null 2>&1; then
        perf stat -x, -e instructions:u -o /tmp/ps.txt chroot "$NR" /bin/bash -c "$w" >/dev/null 2>&1
        echo "native: $(grep instructions /tmp/ps.txt | cut -d, -f1) instructions (user)"
    fi
    if command -v strace >/dev/null 2>&1; then
        strace -f -c -o /tmp/st.txt chroot "$NR" /bin/bash -c "$w" >/dev/null 2>&1
        echo "native: $(tail -n 1 /tmp/st.txt | awk '{print $(NF-2)}') syscalls; top:"
        sed -n '3,12p' /tmp/st.txt | awk '{print "   ", $NF, $(NF-2) " calls"}'
    fi
done

echo "== imports of dnf (python3 -X importtime, top 25 by cumulative us), lxrun"
lx 'python3 -X importtime -c "import dnf, dnf.cli.main" 2>&1 >/dev/null' | sort -t'|' -k2 -n -r | head -n 25

cat > "$R/tmp/prof.py" <<'PY'
import cProfile, pstats, sys, io
sort, n = sys.argv[1], int(sys.argv[2])
sys.argv = ["dnf"] + sys.argv[3:]
from dnf.cli import main
pr = cProfile.Profile(); pr.enable()
try: main.main(sys.argv[1:])
except SystemExit: pass
pr.disable()
s = io.StringIO(); pstats.Stats(pr, stream=s).sort_stats(sort).print_stats(n)
sys.stderr.write(s.getvalue())
PY
for w in "--version" "-y makecache" "-C search nginx"; do
    echo "== cProfile: dnf $w (top 30 by cumulative time), lxrun"
    lx "python3 /tmp/prof.py cumulative 30 $w 2>&1 >/dev/null" | grep -v '^$' | head -n 45
    echo "== cProfile: dnf $w (top 25 by own time), lxrun"
    lx "python3 /tmp/prof.py tottime 25 $w 2>&1 >/dev/null" | grep -v '^$' | head -n 40
done

echo "::group::debug symbols for the libraries dnf runs on (native, into the copy)"
PKGS="glibc python3-libs libsolv libdnf librepo rpm-libs openssl-libs libxml2 glib2 zlib-ng-compat libzstd xz-libs libcurl sqlite-libs libgcrypt gpgme libmodulemd libyaml"
chroot "$NR" /bin/bash -c "dnf -y -q --enablerepo='*-debuginfo' --setopt=strict=0 install \$(for p in $PKGS; do printf '%s-debuginfo ' \$p; done) 2>&1 | tail -n 5; ls /usr/lib/debug/.build-id | wc -l"
echo "::endgroup::"
for w in "dnf -y makecache" "cold: dnf -y makecache" "rpm -qa" "dnf -C search nginx" "dnf -y install --setopt=install_weak_deps=False httpd"; do
    echo "== guest profile (LXRUN_PROF): $w"
    case $w in cold:*) w=${w#cold: }; lx 'dnf clean all' >/dev/null 2>&1 ;; esac
    # (one simple command: bash execs it, so it is the main process the profile follows)
    LXRUN_PROF=/tmp/lx.prof LXRUN_QUIET=1 timeout 2400 "$L" "$R" /bin/bash -c "$w" >/dev/null 2>&1
    python3 "$DIR/tests/linux/prof_resolve.py" /tmp/lx.prof "$NR" 45
done

if command -v perf >/dev/null 2>&1; then
    echo "== perf: host-side hot spots of lxrun running dnf -y makecache"
    LXRUN_QUIET=1 perf record -q -F 999 -g -o /tmp/perf.data "$L" "$R" /bin/bash -c 'dnf -y makecache' >/dev/null 2>&1
    perf report -i /tmp/perf.data --no-children --sort symbol --stdio 2>/dev/null | grep -v '^#' | grep '%' | head -n 40
fi
