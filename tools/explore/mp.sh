#!/bin/sh
# Exploration: Python's multiprocessing under lxrun with each start method (fork, forkserver,
# spawn): a Pool and a Queue fed by four processes.
# usage: sudo sh tools/explore/mp.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'command -v python3 || dnf -y -q install --setopt=install_weak_deps=False python3' > /dev/null 2>&1
cat > "$R/tmp/mp2.py" <<'PY'
import multiprocessing as m, sys
def f(x): return x * x
if __name__ == "__main__":
    print(sys.version.split()[0], "default:", m.get_start_method(allow_none=True) or "(unset)")
    m.set_start_method(sys.argv[1])
    with m.Pool(2) as p:
        print("pool", p.map(f, range(4)))
    q = m.Queue()
    ps = [m.Process(target=q.put, args=(i,)) for i in range(4)]
    for x in ps: x.start()
    print("queue", sorted(q.get() for _ in ps))
    for x in ps: x.join()
PY
ls -ld "$R/dev/shm" 2>&1
for meth in fork forkserver spawn; do
    echo "== start method $meth"
    t0=$(date +%s.%N)
    LXRUN_QUIET=1 timeout 120 build/lxrun "$R" /usr/bin/python3 /tmp/mp2.py $meth > /tmp/mp.out 2>&1
    rc=$?
    tail -n 8 /tmp/mp.out
    echo "rc=$rc $(echo "$(date +%s.%N) - $t0" | bc) s"
done
