#!/bin/sh
# Debian 13 in the wasm32 core under node (build/lxrun.js, NODERAWFS): glibc's loader,
# bash, coreutils, perl, dpkg, apt.
# usage: tests/wasm/run-debian.sh ROOT   (from tests/linux/fetch-debian.sh)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
fail=0
check() {   # check NAME EXPECTED TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s.%N)
    out="$(LXRUN_QUIET=1 LXRUN_STATS=1 timeout "$to" node "$DIR/build/lxrun.js" "$R" "$@" 2>&1)"; rc=$?
    secs=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then echo "OK   wasm debian: $name (${secs}s)"
    else echo "FAIL wasm debian: $name (rc=$rc, ${secs}s)"; fail=1; fi
    printf '%s\n' "$out" | tail -n 5 | sed 's/^/     > /'
}
check "ld.so --version"  "GLIBC"        60  /lib/ld-linux-aarch64.so.1 --version
check "bash"             "bash 5"       60  /bin/bash -c 'echo bash ${BASH_VERSINFO[0]}'
check "pipeline"         "3"            60  /bin/bash -c 'ls /etc | head -n 3 | wc -l'
check "subst + sort"     "2 b"          60  /bin/bash -c 'printf "a\nb\nb\n" | sort | uniq -c | sed "s/^ *//" | grep b'
check "perl"             "perl ok"      60  /usr/bin/perl -e 'print "perl ok\n"'
check "dpkg -l bash"     "ii  bash"     120 /usr/bin/dpkg -l bash
check "apt-get --version" "apt"         60  /usr/bin/apt-get --version
check "id"               "uid=0(root)"  60  /usr/bin/id
check "Ctrl-less job"    "jobs-ok"      60  /bin/bash -c 'sleep 0.2 & wait $!; echo jobs-ok'
if [ -x "$R/usr/bin/python3" ]; then
    check "python3 -c pass (accelerator on)"   "ok"            120 /usr/bin/python3 -c 'print("ok")'
    check "python3 1M loop (accelerator on)"   "499999500000"  600 /usr/bin/python3 -c 'print(sum(i for i in range(1000000)))'
    check "python3 json+re (accelerator on)"   "done"          600 /usr/bin/python3 -c 'import json,re
d=[{"k":i,"v":str(i)*3} for i in range(20000)]
s=json.dumps(d); x=json.loads(s); n=len(re.findall(r"[0-9]+",s)); print("done",n)'
    export AOI_JIT=0
    check "python3 1M loop (interpreter only)" "499999500000"  600 /usr/bin/python3 -c 'print(sum(i for i in range(1000000)))'
    check "python3 json+re (interpreter only)" "done"          600 /usr/bin/python3 -c 'import json,re
d=[{"k":i,"v":str(i)*3} for i in range(20000)]
s=json.dumps(d); x=json.loads(s); n=len(re.findall(r"[0-9]+",s)); print("done",n)'
    unset AOI_JIT
fi
exit $fail
