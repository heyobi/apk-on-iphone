#!/bin/sh
# Exploration: does the host process (lxrun) keep file descriptors that guest processes have
# let go? A guest Python starts many subprocesses the way Python's test runner does (Popen with
# start_new_session, stdout to a file, close_fds) and other patterns; the host's open fds are
# counted from outside (/proc/PID/fd) as it goes.
# usage: sudo sh tools/explore/fdleak.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cat > "$R/tmp/leak.py" <<'PY'
import os, subprocess, sys, tempfile, socket, selectors, threading
kind = sys.argv[1]; n = int(sys.argv[2])
def guest_fds(): return len(os.listdir("/proc/self/fd"))
for i in range(n):
    if kind == "popen":
        with tempfile.TemporaryFile() as out:
            p = subprocess.Popen(["/bin/true"], stdout=out, stderr=out, start_new_session=True, close_fds=True)
            p.wait()
    elif kind == "popen-pipe":
        p = subprocess.run(["/bin/echo", "x"], capture_output=True)
    elif kind == "python-child":
        subprocess.run([sys.executable, "-c", "import os"], capture_output=True)
    elif kind == "socketpair":
        a, b = socket.socketpair(); a.close(); b.close()
    elif kind == "tcp":
        s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(); c = socket.create_connection(s.getsockname()); a, _ = s.accept(); a.close(); c.close(); s.close()
    elif kind == "selector":
        sel = selectors.DefaultSelector(); r, w = os.pipe(); sel.register(r, selectors.EVENT_READ); sel.close(); os.close(r); os.close(w)
    elif kind == "thread":
        t = threading.Thread(target=lambda: None); t.start(); t.join()
    elif kind == "unix":
        p = f"/tmp/u{i}.sock"; s = socket.socket(socket.AF_UNIX); s.bind(p); s.listen(); c = socket.socket(socket.AF_UNIX); c.connect(p); a, _ = s.accept(); a.close(); c.close(); s.close(); os.unlink(p)
    if i % (n // 4) == 0 or i == n - 1:
        print(f"{kind} {i + 1}: guest fds {guest_fds()}", flush=True)
PY
for kind in popen popen-pipe python-child socketpair tcp selector thread unix; do
    n=200; [ $kind = python-child ] && n=40
    LXRUN_QUIET=1 build/lxrun "$R" /usr/bin/python3 /tmp/leak.py $kind $n > /tmp/leak.out 2>&1 &
    pid=$!; seq=""
    while kill -0 $pid 2>/dev/null; do c=$(ls /proc/$pid/fd 2>/dev/null | wc -l); seq="$seq $c"; sleep 0.5; done
    set -- $seq
    printf '== %-13s host fds over time: %s ... %s   ' $kind "$(echo $seq | cut -d' ' -f1-4)" "$(echo $seq | tr ' ' '
' | tail -n 4 | tr '
' ' ')"; tail -n 1 /tmp/leak.out
done
