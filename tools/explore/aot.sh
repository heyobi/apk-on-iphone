#!/bin/sh
# Exploration: guest libraries translated ahead of time (tools/aotgen.c, core/aot.c), only
# their hot functions: a training run under LXRUN_PROF picks, in each library, the functions
# that hold 99% of its samples (tests/linux/prof_funcs.py, bounds from AlmaLinux's debuginfo). Then lxrun is built with them, and the
# training commands and some others (not seen in training) run with and without:
# outputs must match, times are compared.
# usage: sudo sh tools/explore/aot.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
CORE=$(sed -n 's/^CORE := //p' Makefile)
cc -O2 -Wall -o build/aotgen tools/aotgen.c 2>/dev/null || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y makecache' > /dev/null 2>&1   # (metadata for all runs)

echo "== training (interpreted, sampled)"
mkdir -p build/prof; rm -f build/prof/*
k=0
for cmd in 'dnf --version' 'dnf -C search nginx' 'dnf -C list --installed' 'dnf -y makecache' \
           'dnf -C repoquery --requires httpd' 'python3 -c "import hashlib; s=0
for i in range(200000): s+=i*i
print(hashlib.sha256(str(s).encode()).hexdigest())"' 'rpm -qa'; do
    k=$((k + 1))
    LXRUN_PROF=build/prof/$k.prof LXRUN_QUIET=1 timeout 1800 build/lxrun "$R" /bin/bash -c "$cmd" > /dev/null 2>&1
done
echo "== debug symbols (function bounds), natively"
mount -t proc proc "$R/proc"; mount --rbind /dev "$R/dev"
chroot "$R" /bin/bash -c "dnf -y -q --enablerepo='*-debuginfo' --setopt=strict=0 install python3-libs-debuginfo libsolv-debuginfo libsqlite3x-debuginfo sqlite-libs-debuginfo rpm-libs-debuginfo libdnf-debuginfo libxml2-debuginfo glib2-debuginfo openssl-libs-debuginfo zlib-ng-compat-debuginfo > /dev/null 2>&1; ls /usr/lib/debug/.build-id | wc -l"
umount -R "$R/dev" "$R/proc"
python3 tests/linux/prof_funcs.py "$R" 0.99 build/ranges build/prof/*.prof
LIBS="libsolv:usr/lib64/libsolv.so.1 libpython:usr/lib64/libpython3.12.so.1.0 libsqlite:usr/lib64/libsqlite3.so.0 librpm:usr/lib64/librpm.so.10 librpmio:usr/lib64/librpmio.so.10 libdnf:usr/lib64/libdnf.so.2 libxml2:usr/lib64/libxml2.so.2 libglib:usr/lib64/libglib-2.0.so.0 libcrypto:usr/lib64/libcrypto.so.3 libz:usr/lib64/libz.so.1 librpmseq:usr/lib64/librpm_sequoia.so.1 libsolvext:usr/lib64/libsolvext.so.1 bash:usr/bin/bash ldso:usr/lib/ld-linux-aarch64.so.1 libgcc:usr/lib64/libgcc_s.so.1 libcurl:usr/lib64/libcurl.so.4"
TABLE="#include \"aot.h\"
"; OBJS=""; REFS=""; total=0
for l in $LIBS; do
    name=${l%%:*}; real=$(readlink -f "$R/${l#*:}")
    rf="build/ranges/$(basename "$real").ranges"
    [ -s "$rf" ] || continue
    build/aotgen "$real" "$name" "build/aot_$name.c" "$rf" || continue
    t0=$(date +%s)
    cc -O1 -g0 -w -std=gnu11 -Icore -DAOI_AOT -c "build/aot_$name.c" -o "build/aot_$name.o" || continue
    sz=$(stat -c %s "build/aot_$name.o"); total=$((total + sz))
    echo "   $name: object $((sz / 1024)) KiB, compiled in $(( $(date +%s) - t0 )) s"
    TABLE="${TABLE}extern const struct aoi_aot_lib aot_$name;
"; OBJS="$OBJS build/aot_$name.o"; REFS="$REFS &aot_$name,"
done
echo "   all translated code: $((total / 1024)) KiB of objects"
printf "${TABLE}const struct aoi_aot_lib *const aoi_aot_libs[] = {$REFS 0 };\n" > build/aot_table.c
cc -O2 -g -std=c11 -Icore -DAOI_AOT -o build/lxrun-aot tools/lxrun.c $CORE $OBJS build/aot_table.c -lm -lpthread 2>/dev/null || { echo "link failed"; exit 1; }

cmp_run() {   # cmp_run LABEL COMMAND
    for b in lxrun lxrun-aot; do
        t0=$(date +%s.%N)
        LXRUN_STATS=1 LXRUN_QUIET=1 timeout 1800 build/$b "$R" /bin/bash -c "$2" > /tmp/out.$b 2> /tmp/err.$b
        echo "$(echo "$(date +%s.%N) - $t0" | bc)" > /tmp/t.$b
    done
    sed -i -e 's/ *[0-9.]* [kM]B\/s.*//' -e '/^Last metadata expiration check/d' \
        /tmp/out.lxrun /tmp/out.lxrun-aot   # (download speeds, the metadata's age differ)
    if cmp -s /tmp/out.lxrun /tmp/out.lxrun-aot; then same=same; else same=DIFFERENT; fi
    a=$(cat /tmp/t.lxrun); b=$(cat /tmp/t.lxrun-aot)
    printf 'AOT %-34s interpreted %7.2f s   translated %7.2f s   %4.0f%%   output %s\n' "$1" "$a" "$b" "$(echo "100 * (1 - $b / $a)" | bc -l)" "$same"
    grep "translated ahead" /tmp/err.lxrun-aot | sed 's/^/     /'
    [ $same = same ] || { diff /tmp/out.lxrun /tmp/out.lxrun-aot | head -n 10 | sed 's/^/     | /'; tail -n 4 /tmp/err.lxrun-aot | sed 's/^/     ! /'; }
}
echo "== trained on"
cmp_run "dnf --version"                'dnf --version'
cmp_run "dnf -C search nginx"          'dnf -C search nginx'
cmp_run "dnf -C repoquery --requires"  'dnf -C repoquery --requires httpd'
cmp_run "rpm -qa"                      'rpm -qa'
echo "== not seen in training"
cmp_run "dnf -C info httpd"            'dnf -C info httpd'
cmp_run "dnf -C list --available"      'dnf -C list --available | wc -l'
cmp_run "dnf -C search python3 tool"   'dnf -C search python3 tool | head -n 20'
cmp_run "python3 json + re"            'python3 -c "import json, re; d=[{\"k\": i, \"v\": str(i)*5} for i in range(40000)]; s=json.dumps(d); print(len(s), len(re.findall(r\"[0-9]{5}\", s)), len(json.loads(s)))"'
cp -a "$R" "$R.b"
cmp_run "dnf install tmux"             'dnf -y -C --setopt=install_weak_deps=False install tmux >/dev/null 2>&1; rpm -q tmux; dnf -y -C remove tmux >/dev/null 2>&1; rpm -q tmux'
