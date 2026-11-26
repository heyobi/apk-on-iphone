#!/bin/sh
# The wasm32 core for a web worker (WKWebView): build/web/lxweb.js + lxweb.wasm, a module
# factory createLx(); the page's worker fills its in-memory file system (MEMFS) with a root
# file system and calls main with lxrun's arguments (tests/wasm/web/lxworker.js).
set -eu
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$DIR"
mkdir -p build/web
CORE="core/jit.c core/elf.c core/scan.c core/vm.c core/cpu.c core/simd.c core/linux.c core/load.c core/dl.c core/bionic.c core/proc.c core/binder.c core/sf.c core/af.c core/gralloc.c core/snap.c core/hle.c core/lxfast.c core/tty.c"
emcc -O2 -std=gnu11 -w \
    -sENVIRONMENT=worker -sMODULARIZE=1 -sEXPORT_NAME=createLx -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 \
    -sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB \
    -sEXIT_RUNTIME=1 --post-js tests/wasm/web/netbridge.js --post-js tests/wasm/web/filesbridge.js -o build/web/lxweb.js tools/lxrun.c $CORE
ls -la build/web
