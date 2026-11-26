#!/bin/sh
# Exploration: a guest library translated ahead of time (tools/aotgen.c, core/aot.c).
# Translates AlmaLinux's libsolv (and libpython), builds lxrun with it, and runs the same
# dnf commands with and without: outputs must match, times are compared.
# usage: sudo sh tools/explore/aot.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
set -e
CORE=$(sed -n 's/^CORE := //p' Makefile)
cc -O2 -Wall -o build/aotgen tools/aotgen.c
LIBS="libsolv:usr/lib64/libsolv.so.1 libpython:usr/lib64/libpython3.12.so.1.0"
TABLE="#include \"aot.h\"\n"
OBJS=""
REFS=""
for l in $LIBS; do
    name=${l%%:*}; path=$R/${l#*:}
    real=$(readlink -f "$path")
    build/aotgen "$real" "$name" "build/aot_$name.c"
    ls -la "build/aot_$name.c" | awk '{print "   generated C: " $5 " bytes"}'
    t0=$(date +%s)
    cc -O1 -g0 -w -std=gnu11 -Icore -DAOI_AOT -c "build/aot_$name.c" -o "build/aot_$name.o"
    echo "   compiled in $(( $(date +%s) - t0 )) s, object $(stat -c %s build/aot_$name.o) bytes"
    TABLE="${TABLE}extern const struct aoi_aot_lib aot_$name;\n"
    OBJS="$OBJS build/aot_$name.o"
    REFS="$REFS &aot_$name,"
done
printf "${TABLE}const struct aoi_aot_lib *const aoi_aot_libs[] = {$REFS 0 };\n" > build/aot_table.c
cc -O2 -g -Wall -Wextra -std=c11 -DAOI_AOT -o build/lxrun-aot tools/lxrun.c $CORE $OBJS build/aot_table.c -lm -lpthread
set +e
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y makecache' > /dev/null 2>&1   # (metadata for both)

cmp_run() {   # cmp_run LABEL COMMAND
    for b in lxrun lxrun-aot; do
        t0=$(date +%s.%N)
        LXRUN_STATS=1 LXRUN_QUIET=1 timeout 1800 build/$b "$R" /bin/bash -c "$2" > /tmp/out.$b 2> /tmp/err.$b
        echo "$(echo "$(date +%s.%N) - $t0" | bc)" > /tmp/t.$b
    done
    if cmp -s /tmp/out.lxrun /tmp/out.lxrun-aot; then same=same; else same=DIFFERENT; fi
    printf 'AOT %-36s interpreted %7.2f s   translated %7.2f s   output %s\n' "$1" "$(cat /tmp/t.lxrun)" "$(cat /tmp/t.lxrun-aot)" "$same"
    grep "translated ahead" /tmp/err.lxrun-aot | sed 's/^/     /'
    [ $same = same ] || { diff /tmp/out.lxrun /tmp/out.lxrun-aot | head -n 10 | sed 's/^/     | /'; tail -n 5 /tmp/err.lxrun-aot | sed 's/^/     ! /'; }
}
cmp_run "python3 -c pass"            'python3 -c pass'
cmp_run "python3 loop + sha"         'python3 -c "import hashlib; s=0
for i in range(300000): s+=i*i
print(s, hashlib.sha256(str(s).encode()).hexdigest())"'
cmp_run "dnf --version"              'dnf --version'
cmp_run "dnf -C search nginx"        'dnf -C search nginx'
cmp_run "dnf -C list --installed"    'dnf -C list --installed'
cmp_run "dnf makecache (cached)"     'dnf -y makecache | grep -v "expiration check"'
cmp_run "dnf -C repoquery --requires httpd" 'dnf -C repoquery --requires httpd'
