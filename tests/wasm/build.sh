#!/bin/sh
# Builds tools/lxrun with the core for wasm32 with Emscripten: build/lxrun.js + .wasm, run with
# node; NODERAWFS gives the program the host's files directly (stage 1: no web view yet).
set -eu
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$DIR"
mkdir -p build
CORE="core/elf.c core/scan.c core/vm.c core/cpu.c core/simd.c core/linux.c core/load.c core/dl.c core/bionic.c core/proc.c core/binder.c core/sf.c core/af.c core/gralloc.c core/snap.c core/hle.c core/tty.c"
emcc -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-sign-compare \
    -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sEXIT_RUNTIME=1 \
    -o build/lxrun.js tools/lxrun.c $CORE 2>&1 | grep -E "error|warning: .*(pointer|int-to|incompatible)" | head -n 80 || true
ls -la build/lxrun.js build/lxrun.wasm
