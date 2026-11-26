#!/bin/sh
# AlmaLinux (or any dnf distribution) benchmarks: the package-manager work people do in a
# tab, timed under build/lxrun and natively (chroot on the same arm64 runner), so each
# number comes with the slowdown against real hardware. Output: BENCH lines, a Markdown
# table ($GITHUB_STEP_SUMMARY when set) and a JSON file for history.
# usage: sudo tests/linux/bench_dnf.sh ROOT NAME OUT.json
#   ROOT from tests/linux/fetch-image.sh; a copy is made for the native runs.
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1; N=$2; OUT=${3:-bench.json}
L="$DIR/build/lxrun"
NR="$R.native"
rm -rf "$NR"; cp -a "$R" "$NR"
for r in "$R" "$NR"; do
    cp /etc/resolv.conf "$r/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$r/etc/resolv.conf"
done
mkdir -p "$NR/proc" "$NR/dev" "$NR/sys"
mount -t proc proc "$NR/proc"; mount --rbind /dev "$NR/dev"; mount --rbind /sys "$NR/sys"
trap 'umount -R "$NR/dev" "$NR/sys" "$NR/proc" 2>/dev/null' EXIT

ROWS=""
tm() {   # tm WHERE TIMEOUT CMD -> prints seconds (or "fail")
    where=$1; to=$2; cmd=$3
    t0=$(date +%s.%N)
    if [ "$where" = lx ]; then
        LXRUN_QUIET=1 timeout "$to" "$L" "$R" /bin/bash -c "$cmd" > /tmp/bench.out 2>&1; rc=$?
    else
        timeout "$to" chroot "$NR" /bin/bash -c "$cmd" > /tmp/bench.out 2>&1; rc=$?
    fi
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ $rc -ne 0 ]; then echo "fail"; tail -n 5 /tmp/bench.out >&2; else printf '%.2f' "$t"; fi
}
bench() {   # bench KEY "Label" TIMEOUT CMD
    key=$1; label=$2; to=$3; cmd=$4
    n=$(tm native "$to" "$cmd"); x=$(tm lx "$to" "$cmd")
    if [ "$n" != fail ] && [ "$x" != fail ]; then r=$(echo "scale=1; $x / ($n + 0.005)" | bc); else r="?"; fi
    printf 'BENCH %-40s native %8s s   lxterm %8s s   x%s\n' "$label" "$n" "$x" "$r"
    ROWS="$ROWS$key|$label|$n|$x|$r
"
}
DNF="dnf -y --setopt=install_weak_deps=False"
bench py_start    "python3 -c pass"                    120  'python3 -c pass'
bench py_loop     "python3 1M-iteration loop"          300  'python3 -c "s=0
for i in range(1000000): s+=i"'
bench rpm_q       "rpm -q bash"                        120  'rpm -q bash'
bench rpm_qa      "rpm -qa (all installed packages)"   300  'rpm -qa > /dev/null'
bench dnf_version "dnf --version (dnf start-up)"       300  'dnf --version'
bench dnf_cold    "dnf makecache, cold (download)"     2400 'dnf clean all >/dev/null; dnf -y makecache'
bench dnf_warm    "dnf makecache, metadata cached"     1200 'dnf -y makecache'
bench dnf_list    "dnf -C list installed"              1200 'dnf -C list --installed > /dev/null'
bench dnf_search  "dnf -C search nginx"                1200 'dnf -C search nginx > /dev/null'
bench dnf_tmux    "dnf install tmux"                   2400 "$DNF install tmux"
bench dnf_rm      "dnf remove tmux"                    1200 "$DNF remove tmux"
bench dnf_httpd   "dnf install httpd (+ deps)"         2400 "$DNF install httpd"

VER=$(sed -n 's/^PRETTY_NAME="\(.*\)"/\1/p' "$R/etc/os-release")
DNFV=$(chroot "$NR" dnf --version 2>/dev/null | head -n 1)
{
    echo "### $VER ($N), dnf $DNFV"
    echo
    echo "| Workload | Native (arm64 runner) | LxTerm engine | Slowdown |"
    echo "|---|---:|---:|---:|"
    printf '%s' "$ROWS" | while IFS='|' read -r k l n x r; do
        [ -n "$k" ] && echo "| $l | $n s | $x s | ×$r |"
    done
    echo
    echo "lxrun commit $(git -C "$DIR" rev-parse --short HEAD 2>/dev/null), $(date -u +%Y-%m-%dT%H:%MZ)"
} > /tmp/bench.md
cat /tmp/bench.md
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat /tmp/bench.md >> "$GITHUB_STEP_SUMMARY"
printf '%s' "$ROWS" | python3 -c '
import json, sys, subprocess
rows = []
for line in sys.stdin:
    k, l, n, x, r = line.rstrip("\n").split("|")
    num = lambda v: None if v in ("fail", "?") else float(v)
    rows.append({"key": k, "label": l, "native_s": num(n), "lxterm_s": num(x), "slowdown": num(r)})
json.dump({"distro": sys.argv[1], "version": sys.argv[2], "dnf": sys.argv[3], "commit": sys.argv[4], "results": rows},
          open(sys.argv[5], "w"), indent=1)
' "$N" "$VER" "$DNFV" "$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null)" "$OUT"
