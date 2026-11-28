#!/bin/sh
# Exploration: a socketserver in a thread, shut down from the main thread (test_logging,
# test_pydoc, test_ssl hang so): does it stop; if not, the full trace's last lines.
# usage: sudo sh tools/explore/srvstop.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3' > /dev/null 2>&1
cat > "$R/tmp/srv.py" <<'PY'
import socketserver, threading, time, select, socket, sys
kind = sys.argv[1]
if kind == "socketserver":
    s = socketserver.TCPServer(("127.0.0.1", 0), socketserver.BaseRequestHandler)
    t = threading.Thread(target=s.serve_forever, kwargs={"poll_interval": 0.5}); t.start()
    time.sleep(1); s.shutdown(); t.join(); print("stopped socketserver")
elif kind == "select-flag":
    srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(); finish = False
    def serve():
        while not finish: select.select([srv], [], [], 0.1)
    t = threading.Thread(target=serve); t.start(); time.sleep(1); finish = True; t.join(); print("stopped select-flag")
elif kind == "event":
    e = threading.Event()
    t = threading.Thread(target=lambda: e.wait()); t.start(); time.sleep(0.5); e.set(); t.join(); print("stopped event")
PY
for k in event select-flag socketserver; do
    echo "== $k"
    LXRUN_QUIET=1 timeout 30 build/lxrun "$R" /usr/bin/python3 /tmp/srv.py $k 2>&1 | tail -n 3; echo "rc=$?"
    LXRUN_QUIET=1 timeout 20 build/lxrun -t "$R" /usr/bin/python3 /tmp/srv.py $k 2>&1 | grep -E "^\[(sys|sig|sched)\]" | tail -n 45 | cut -c1-180 | sed 's/^/  > /'
done
