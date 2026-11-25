#!/bin/sh
# Jupyter in Debian (root from tests/linux/fetch-debian.sh): Debian's own jupyter-notebook
# package installed with apt, the server started, its page fetched over localhost, as
# LxTerm's Ports list and browser would. usage: tests/linux/run_jupyter.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
t0=$(date +%s)
LXRUN_QUIET=1 timeout 3000 "$L" "$R" /bin/sh -c 'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends jupyter-notebook python3-ipykernel curl > /tmp/apt.log 2>&1; tail -n 3 /tmp/apt.log; command -v jupyter' 2>&1 | tail -n 5
echo "install: $(( $(date +%s) - t0 ))s"
t0=$(date +%s)
out="$(LXRUN_QUIET=1 timeout 900 "$L" "$R" /bin/bash -c '
    jupyter notebook --no-browser --allow-root --ip=127.0.0.1 --port=8888 --IdentityProvider.token=lx > /tmp/jup.log 2>&1 &
    for i in $(seq 1 120); do c=$(curl -s -o /tmp/page -w "%{http_code}" "http://127.0.0.1:8888/tree?token=lx"); [ "$c" = 200 ] && break; sleep 3; done
    echo "http $c"; grep -o "<title>[^<]*</title>" /tmp/page | head -n 1
    k=$(curl -s -X POST -H "Authorization: token lx" http://127.0.0.1:8888/api/kernels); echo "kernel: $(echo "$k" | head -c 120)"
    head -n 60 /tmp/jup.log; kill %1' 2>&1)"
echo "$out" | sed 's/^/     | /'
echo "server: $(( $(date +%s) - t0 ))s"
echo "$out" | grep -q "http 200" && echo "OK   jupyter: notebook page over localhost" && exit 0
echo "FAIL jupyter"
echo "== failing syscalls of a jupyter start"
LXRUN_QUIET=1 timeout 300 "$L" -t "$R" /bin/bash -c 'timeout 120 jupyter notebook --no-browser --allow-root --ip=127.0.0.1 --port=8889 > /dev/null 2>&1' 2>&1 |
    grep -E "= -[0-9]+$|not impl" | grep -vE "= -(11|2|25|115)$" | sed 's/(0x[0-9a-f]*,.*) =/ =/' | sort | uniq -c | sort -rn | head -n 40
exit 1
