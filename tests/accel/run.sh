#!/bin/sh
# Accelerator prototype measurement (arm64 Linux host): the same freestanding program
# natively, in the interpreter (build/lxrun) and translated to WebAssembly (V8).
# usage: tests/accel/run.sh OUTDIR
set -eu
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
O=$1
mkdir -p "$O/root"
gcc -O2 -static -nostdlib -fno-builtin -mgeneral-regs-only -fno-pic -no-pie -mbranch-protection=none \
    -o "$O/root/bench" "$DIR/tests/accel/bench.c"
objdump -d "$O/root/bench" | awk -F'\t' 'NF>=3{split($3,a," "); print a[1]}' | sort | uniq -c | sort -rn > "$O/mnemonics.txt"
echo "instructions used:"; head -n 60 "$O/mnemonics.txt" | tr '\n' ' '; echo
t() { s=$(date +%s.%N); "$@"; echo "time: $(echo "$(date +%s.%N) - $s" | bc) s"; }
echo "== native"; t "$O/root/bench"
echo "== interpreter (lxrun)"; t "$DIR/build/lxrun" "$O/root" /bench
python3 "$DIR/tests/accel/a64wasm.py" "$O/root/bench" "$O/bench.wat" "$O/bench.json" "$O/bench.mem"
wat2wasm "$O/bench.wat" -o "$O/bench.wasm"
ls -la "$O/bench.wasm"
cp "$DIR/tests/accel/run.js" "$O/"
echo "== wasm, V8 (node, default tiering)"; node "$O/run.js" "$O"
echo "== wasm, V8 (node --no-liftoff: optimizing compiler only)"; node --no-liftoff "$O/run.js" "$O"
