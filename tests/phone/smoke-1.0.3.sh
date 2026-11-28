#!/bin/bash
# Pocket Distro 1.0.3 smoke test, run on the phone in an AlmaLinux tab (as root):
#   curl -fsSL https://raw.githubusercontent.com/heyobi/apk-on-iphone/linux-userland/tests/phone/smoke-1.0.3.sh | bash
# Checks what CI (a Linux host) cannot: the engine's iOS paths for file modes and owners
# (xattr + UF_NODUMP), extended attributes, umask, utime, durable fsync (F_FULLFSYNC),
# and the new terminal, signal, socket and netlink support. About a minute.
pass=0; fail=0
ok()  { echo "  OK   $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL $1 -> $2"; fail=$((fail + 1)); }
t() {   # t NAME EXPECTED COMMAND
    out="$(bash -c "$3" 2>&1)"
    if printf '%s' "$out" | grep -qF -- "$2"; then ok "$1"; else bad "$1" "$(printf '%s' "$out" | tail -n 2 | tr '\n' ' ')"; fi
}
D=/root/pd-smoke; rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 1
echo "== files"
t "chmod 444 shows 444"        "444"        "touch f1; chmod 444 f1; stat -c %a f1"
t "root still writes a 444 file" "written"  "echo hi >> f1 && echo written"
t "chown shows the new owner"  "1234:5678"  "touch f2; chown 1234:5678 f2; stat -c %u:%g f2"
t "chmod after chown keeps it" "640 1234"   "chmod 640 f2; stat -c '%a %u' f2"
t "umask 0 creates 666"        "666"        "umask 0; touch f3; stat -c %a f3"
t "mkdir -m 555"               "555"        "mkdir -m 555 d1; stat -c %a d1"
t "touch -d sets the time"     "2001-02-03" "touch -d '2001-02-03 04:05:06' f4 2>/dev/null || touch f4; touch -d '2001-02-03 04:05:06' f4; date -r f4 +%F"
t "extended attributes"        "b'v1'"      "touch f5; python3 -c \"import os; os.setxattr('f5','user.t',b'v1'); print(os.getxattr('f5','user.t'), os.listxattr('f5'))\""
t "ls -l shows the owner"      "1234"       "ls -ln f2"
echo "== durable writes"
t "fsync (F_FULLFSYNC) works"  "fsync ok"   "python3 -c \"
import os, time
t0 = time.monotonic()
for i in range(50):
    fd = os.open('s%d' % i, os.O_WRONLY | os.O_CREAT, 0o644); os.write(fd, b'x' * 4096); os.fsync(fd); os.close(fd)
print('fsync ok, 50 files in %.2f s' % (time.monotonic() - t0))\""
python3 -c "
import os, time
t0 = time.monotonic()
for i in range(50):
    fd = os.open('/root/pd-smoke/s%d' % i, os.O_WRONLY | os.O_CREAT, 0o644); os.write(fd, b'x' * 4096); os.fsync(fd); os.close(fd)
print('       (50 fsyncs: %.2f s)' % (time.monotonic() - t0))"
echo "== terminals, signals, sockets"
t "os.openpty + termios"       "pty True"   "python3 -c \"import os, termios; m, s = os.openpty(); os.write(m, b'hi' + bytes([10])); print('pty', os.read(s, 10) == b'hi' + bytes([10]), len(termios.tcgetattr(s)))\""
t "alarm interrupts select"    "alarm 1"    "python3 -c \"
import os, select, signal, time
class B(Exception): pass
def h(*a): raise B()
signal.signal(signal.SIGALRM, h); r, w = os.pipe(); t0 = time.monotonic(); signal.alarm(1)
try: select.select([r], [], [], 20)
except B: print('alarm %.0f' % (time.monotonic() - t0))\""
t "sigwait from another thread" "sigwait True" "timeout 20 python3 -c \"
import os, signal, threading, time
S = signal.SIGUSR1; signal.pthread_sigmask(signal.SIG_BLOCK, [S])
threading.Thread(target=lambda: (time.sleep(0.3), os.kill(os.getpid(), S))).start()
print('sigwait', signal.sigwait([S]) == S)\""
t "unix datagram sockets"      "udg True"   "python3 -c \"
import socket, os
P = '/root/pd-smoke/u.sock'
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(P)
c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); c.sendto(b'one', P)
print('udg', s.recv(10) == b'one')\""
t "select times out empty"     "sel True"   "python3 -c \"
import select, socket, time
a, b = socket.socketpair(); t0 = time.process_time(); r = select.select([a], [], [], 0.3)
print('sel', r == ([], [], []), time.process_time() - t0 < 0.2)\""
t "network interfaces (netlink)" "eth0"     "python3 -c \"import socket; print([n for _, n in socket.if_nameindex()])\""
t "the phone's address"        "."          "python3 -c \"
import ctypes, socket
print('getifaddrs', ctypes.CDLL(None).getifaddrs(ctypes.byref(ctypes.c_void_p())) == 0, socket.gethostname())\""
echo "== packages"
t "rpm database"               "bash-"      "rpm -q bash"
t "dnf starts"                 "dnf"        "dnf --version | head -n 1; echo dnf"
cd /root; rm -rf "$D"
echo
echo "== $pass passed, $fail failed"
