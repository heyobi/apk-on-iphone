#!/bin/sh
# Exploration: Python's multiprocessing.Pool under lxrun (hangs on Fedora and Rocky Linux,
# works on AlmaLinux): the Python version, its start method, and where the processes wait.
# usage: sudo sh tools/explore/mp.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'command -v python3 || dnf -y -q install --setopt=install_weak_deps=False python3' > /dev/null 2>&1
printf 'import multiprocessing as m, sys\nprint(sys.version.split()[0], m.get_start_method())\ndef f(x): return x * x\nif __name__ == "__main__":\n    with m.Pool(2) as p: print("pool", p.map(f, range(4)))\n' > "$R/tmp/mp.py"
ls -ld "$R/dev/shm" 2>&1
rm -f /tmp/ct.*
AOI_CHILD_TRACE=/tmp/ct LXRUN_QUIET=1 timeout 60 build/lxrun -t "$R" /usr/bin/python3 /tmp/mp.py > /tmp/mp.out 2> /tmp/mp.trace
echo "rc=$?"; cat /tmp/mp.out
echo "== main process: last system calls"; tail -n 25 /tmp/mp.trace
for f in /tmp/ct*; do [ -f "$f" ] || continue; echo "== $f: last system calls"; tail -n 25 "$f"; done
echo "== with the fork start method"
printf 'import multiprocessing as m\ndef f(x): return x * x\nif __name__ == "__main__":\n    m.set_start_method("fork")\n    with m.Pool(2) as p: print("pool", p.map(f, range(4)))\n' > "$R/tmp/mp2.py"
LXRUN_QUIET=1 timeout 60 build/lxrun "$R" /usr/bin/python3 /tmp/mp2.py; echo "rc=$?"
