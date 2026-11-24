#!/bin/sh
# Accelerator planning: how many syscalls real Debian workloads make, next to their run
# time in the interpreter. With the bridge's round trip (tests/accel/bridge) this gives
# the time a WebAssembly engine would spend crossing to the app.
# usage: tests/accel/workloads.sh ROOT   (a Debian root from tests/linux/fetch-debian.sh)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
LXRUN_QUIET=1 "$L" "$R" /bin/sh -c 'apt-get update -qq && apt-get install -y -qq --no-install-recommends python3 gcc libc6-dev git >/dev/null 2>&1; echo installed: $(command -v python3 gcc git)'
cp "$DIR/tests/accel/bench.c" "$R/tmp/bench.c"
run() {   # run NAME COMMAND
    name=$1; shift
    t0=$(date +%s.%N)
    s=$(LXRUN_STATS=1 LXRUN_QUIET=1 "$L" "$R" /bin/sh -c "$1" 2>&1 >/dev/null | grep '^\[lxrun\] exit' | tail -n 1)
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    sc=$(echo "$s" | sed -n 's/.*, \([0-9]*\) syscalls.*/\1/p')
    rate=$(echo "${sc:-0} / $t" | bc)
    printf 'WORK %-28s %8.2f s %9s syscalls %7s /s\n' "$name" "$t" "${sc:-?}" "$rate"
}
run "sh -c true"              'true'
run "python3 -c pass"         'python3 -c pass'
run "python3 imports"         'python3 -c "import json, re, collections, email, http.client, sqlite3, decimal"'
run "python3 1M loop"         'python3 -c "s=0
for i in range(1000000): s+=i"'
run "gcc hello.c"             'printf "#include <stdio.h>\nint main(){puts(\"hi\");}\n" > /tmp/h.c && gcc -O2 -o /tmp/h /tmp/h.c && /tmp/h'
run "gcc -O2 -c bench.c"      'gcc -O2 -c -o /tmp/bench.o /tmp/bench.c'
run "git init+add+commit"     'rm -rf /tmp/r && cp -r /etc /tmp/r && cd /tmp/r && git init -q && git add -A && git -c user.email=a@b -c user.name=a commit -qm x'
run "dpkg -l | wc"            'dpkg -l | wc -l'
run "apt-cache search"        'apt-cache search python | wc -l'
run "tar czf python lib"      'tar czf /tmp/py.tgz /usr/lib/python3.13'
run "find / | wc"             'find / -xdev | wc -l'
