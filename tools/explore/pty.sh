#!/bin/sh
# Exploration: os.openpty under lxrun: where it fails (system call trace).
# usage: sudo sh tools/explore/pty.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3' > /dev/null 2>&1
cat > "$R/tmp/pty.py" <<'PY'
import os, termios
m, s = os.openpty()
print("openpty", m, s, os.ttyname(s))
print("tcgetattr", termios.tcgetattr(s)[:4])
os.write(m, b"hi\n"); print("read", os.read(s, 10))
PY
echo "== plain"; LXRUN_QUIET=1 timeout 60 build/lxrun "$R" /usr/bin/python3 /tmp/pty.py 2>&1 | tail -n 5
echo "== trace"; LXRUN_QUIET=1 timeout 60 build/lxrun -t "$R" /usr/bin/python3 /tmp/pty.py 2>&1 | grep -E "ptmx|pts|ioctl|\[tty\]|= -22" | tail -n 40
echo "== interactive (-i)"; LXRUN_QUIET=1 LXRUN_KEYS='\w' timeout 60 build/lxrun -i "$R" /usr/bin/python3 /tmp/pty.py 2>&1 | tail -n 5
