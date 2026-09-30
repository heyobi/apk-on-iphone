#!/bin/sh
# Milestone 1 check: a real AArch64 Linux ELF runs in the interpreter and
# produces the right output and exit code, at -O0 (loops/branches actually run)
# and -O1 (constant-folded). No JIT, no device.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
AOIRUN="$DIR/build/aoirun"
fail=0
for opt in 0 1; do
    elf="$DIR/build/hello_O$opt.elf"
    clang --target=aarch64-linux-gnu -nostdlib -static -mgeneral-regs-only -fuse-ld=lld \
          -O$opt -o "$elf" "$DIR/tests/hello.c"
    out="$("$AOIRUN" "$elf" 2>/dev/null)" ; rc=$?
    exp="hello from guest aarch64 (interpreted, no JIT)"
    if [ "$out" = "$exp" ] && [ "$rc" -eq 0 ]; then
        echo "OK  -O$opt: correct output, exit 0"
    else
        echo "FAIL -O$opt: rc=$rc out=[$out]" ; fail=1
    fi
done
clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld -o "$DIR/build/tpidr.elf" "$DIR/tests/tpidr.S"
if "$AOIRUN" "$DIR/build/tpidr.elf" 2>/dev/null; then echo "OK  tpidr_el0 write/read round-trip"; else echo "FAIL tpidr_el0"; fail=1; fi
exit $fail
