#!/bin/sh
# Differential test of the ahead-of-time translation (tools/aotgen.c, core/aot.c): random
# functions (tests/aot/fuzzgen.py) in a shared library, called with random states
# (tests/aot/main.c), run natively, under the interpreter (build/lxrun) and with the library
# translated (build/lxrun-aotfuzz). Every function's hash must be the same three times, and the
# translated run must actually have run translated code. Needs an arm64 Linux host (the host's
# own root is the guest's).
# usage: sh tests/aot/run.sh [seed] [functions] [instructions] [calls]
set -u
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$DIR" || exit 1
SEED=${1:-1}; NF=${2:-300}; NI=${3:-48}; CALLS=${4:-40}
W=/tmp/aotfuzz.$$
rm -rf "$W"; mkdir -p "$W" build
CORE=$(sed -n 's/^CORE *:= //p' Makefile)
make -s build/lxrun || exit 1
cc -O2 -w -o build/aotgen tools/aotgen.c || exit 1

python3 tests/aot/fuzzgen.py "$W/fuzz.S" "$NF" "$NI" "$SEED" || exit 1
cc -shared -fPIC -Wl,--build-id -o "$W/libfuzz.so" "$W/fuzz.S" || { echo "FAIL aot fuzz: assembling"; exit 1; }
cc -O1 -o "$W/fuzzmain" tests/aot/main.c -L"$W" -lfuzz -Wl,-rpath,'$ORIGIN' || exit 1

# the whole of .text, translated
set -- $(readelf -SW "$W/libfuzz.so" | awk '{ for (i = 1; i < NF; i++) if ($i == ".text") { print $(i + 2), $(i + 4); exit } }')
[ $# = 2 ] || { echo "FAIL aot fuzz: no .text in libfuzz.so"; exit 1; }
printf '%x %x\n' "0x$1" "$((0x$1 + 0x$2))" > "$W/fuzz.ranges"
build/aotgen "$W/libfuzz.so" fuzz "$W/aot_fuzz.c" "$W/fuzz.ranges" || { echo "FAIL aot fuzz: aotgen"; exit 1; }
cc -O1 -g0 -w -std=gnu11 -Icore -DAOI_AOT -c "$W/aot_fuzz.c" -o "$W/aot_fuzz.o" || { echo "FAIL aot fuzz: compiling the translation"; exit 1; }
printf '#include "aot.h"\nextern const struct aoi_aot_lib aot_fuzz;\nconst struct aoi_aot_lib *const aoi_aot_libs[] = { &aot_fuzz, 0 };\n' > "$W/table.c"
cc -O2 -std=c11 -Icore -DAOI_AOT -o build/lxrun-aotfuzz tools/lxrun.c $CORE "$W/aot_fuzz.o" "$W/table.c" -lm -lpthread || exit 1

"$W/fuzzmain" "$CALLS" > "$W/native.txt" || { echo "FAIL aot fuzz: native run"; exit 1; }
LXRUN_QUIET=1 build/lxrun / "$W/fuzzmain" "$CALLS" > "$W/interp.txt" 2> "$W/interp.err" || { echo "FAIL aot fuzz: interpreted run"; tail -n 5 "$W/interp.err"; exit 1; }
LXRUN_QUIET=1 LXRUN_STATS=1 build/lxrun-aotfuzz / "$W/fuzzmain" "$CALLS" > "$W/aot.txt" 2> "$W/aot.err" || { echo "FAIL aot fuzz: translated run"; tail -n 5 "$W/aot.err"; exit 1; }

fail=0
n=$(wc -l < "$W/native.txt")
runs=$(sed -n 's/.*translated ahead of time: \([0-9]*\) runs.*/\1/p' "$W/aot.err")
echo "aot fuzz: seed $SEED, $n functions x $NI instructions x $CALLS calls; translated runs: ${runs:-0}"
[ "${runs:-0}" -gt 0 ] || { echo "FAIL aot fuzz: no translated code ran"; fail=1; }
for pair in "native interp" "native aot"; do
    set -- $pair
    if cmp -s "$W/$1.txt" "$W/$2.txt"; then echo "OK   aot fuzz: $1 = $2"
    else
        echo "FAIL aot fuzz: $1 != $2"; fail=1
        diff "$W/$1.txt" "$W/$2.txt" | sed -n 's/^> \(fuzz_[0-9]*\).*/\1/p' | head -n 5 | while read -r fn; do
            echo "  $fn:"; sed -n "/^$fn:/,/^\.size $fn,/p" "$W/fuzz.S" | grep -v "^ *st[pr] x\|^ *ld[pr] x[0-9]*, \(x[0-9]*, \)\?\[x16\|^ *ld[pr] x[0-9]*, \(x[0-9]*, \)\?\[sp\|^ *mov x2[89]\|^\.size" | head -n 70 | sed 's/^/     /'
        done
    fi
done
[ $fail = 0 ] && rm -rf "$W"
exit $fail
