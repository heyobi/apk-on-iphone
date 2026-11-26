#!/bin/sh
# Exploration: why the first `dnf --version` in a freshly unpacked AlmaLinux root is slow
# (13 s on a phone, 2 s afterwards): what it writes, and the second run beside it.
# usage: sudo sh tools/explore/firstrun.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
SP=$R/usr/lib/python3.12/site-packages
echo "== bytecode in the image"
for d in dnf libdnf hawkey rpm dnfpy; do
    [ -d "$SP/$d" ] && echo "   $d: $(find "$SP/$d" -name '*.py' | wc -l) .py, $(find "$SP/$d" -name '*.pyc' | wc -l) .pyc"
done
echo "   stdlib: $(find "$R/usr/lib64/python3.12" -maxdepth 1 -name '*.py' | wc -l) .py, $(find "$R/usr/lib64/python3.12/__pycache__" -name '*.pyc' 2>/dev/null | wc -l) .pyc"
touch /tmp/marker; sleep 1
for k in 1 2 3; do
    t0=$(date +%s.%N)
    LXRUN_STATS=1 LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf --version' > /dev/null 2> /tmp/err.$k
    echo "== run $k: $(echo "$(date +%s.%N) - $t0" | bc) s"
    grep -E "instructions|syscalls" /tmp/err.$k | head -n 3 | sed 's/^/   /'
    if [ $k = 1 ]; then
        echo "   written by run 1:"
        find "$R" -xdev -newer /tmp/marker -type f 2>/dev/null | sed "s#^$R##" | awk -F/ '{print "/"$2"/"$3"/"$4"/"$5}' | sort | uniq -c | sort -rn | head -n 25 | sed 's/^/     /'
    fi
done
echo "== python3 -c pass, twice"
for k in 1 2; do t0=$(date +%s.%N); build/lxrun "$R" /usr/bin/python3 -c pass; echo "   $(echo "$(date +%s.%N) - $t0" | bc) s"; done
