#!/bin/sh
# Exploration: the accelerated engine (wasm32 core + guest code translated to WebAssembly,
# core/jit.c) on AlmaLinux's dnf, beside the native interpreter. Node's V8 stands in for the
# phone's JavaScriptCore: the ratios matter, not the seconds. (AOT's ratios on the same
# commands: tools/explore/aot.sh.)
# usage: sudo sh tools/explore/accel_alma.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
if ! command -v emcc > /dev/null; then
    git clone -q --depth 1 https://github.com/emscripten-core/emsdk /tmp/emsdk
    (cd /tmp/emsdk && ./emsdk install latest > /dev/null 2>&1 && ./emsdk activate latest > /dev/null 2>&1)
    . /tmp/emsdk/emsdk_env.sh > /dev/null 2>&1
fi
sh tests/wasm/build.sh > /dev/null || { echo "wasm build failed"; exit 1; }
node --version
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y makecache' > /dev/null 2>&1   # (metadata for -C runs)

t() {   # t LABEL ENV COMMAND...  → seconds, output tail
    label=$1; envs=$2; shift 2
    t0=$(date +%s.%N)
    env $envs LXRUN_QUIET=1 LXRUN_STATS=1 timeout 1800 "$@" > /tmp/o 2> /tmp/e; rc=$?
    printf '   %-26s %7.2f s  rc=%s  %s\n' "$label" "$(echo "$(date +%s.%N) - $t0" | bc)" "$rc" "$(grep -o '[0-9]* instructions' /tmp/e | head -n 1)"
}
run() {   # run NAME COMMAND
    echo "== $1"
    t "native interpreter" "" build/lxrun "$R" /bin/bash -c "$2"
    t "wasm, interpreter only" "AOI_JIT=0" node build/lxrun.js "$R" /bin/bash -c "$2"
    t "wasm, accelerator" "" node build/lxrun.js "$R" /bin/bash -c "$2"
    t "wasm, accelerator hot 20k" "AOI_JIT_HOT=20000" node build/lxrun.js "$R" /bin/bash -c "$2"
}
run "dnf --version"         'dnf --version'
run "dnf -C search nginx"   'dnf -C search nginx'
run "rpm -qa"               'rpm -qa'
run "python3 json + re"     'python3 -c "import json, re; d=[{\"k\": i, \"v\": str(i)*5} for i in range(40000)]; s=json.dumps(d); print(len(s), len(re.findall(r\"[0-9]{5}\", s)), len(json.loads(s)))"'
run "python3 1M loop"       'python3 -c "print(sum(i for i in range(1000000)))"'
