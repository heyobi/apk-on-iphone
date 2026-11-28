#!/bin/sh
# Linux userland mode on an RPM distribution's official arm64 root (Fedora, AlmaLinux):
# glibc, bash, coreutils, rpm, dnf. Programs run through bash: AlmaLinux's coreutils-single
# commands are #! scripts, which lxrun's first exec does not follow (the guest's execve does). ROOT from tests/linux/fetch-image.sh.
# usage: tests/linux/run_rpm.sh ROOT NAME [net]   (net: also dnf install)
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1; N=$2
L="$DIR/build/lxrun"
fail=0
check() {   # check NAME EXPECTED-SUBSTRING TIMEOUT PROGRAM ARGS...
    name=$1; exp=$2; to=$3; shift 3
    t0=$(date +%s.%N)
    out="$(LXRUN_QUIET=1 timeout "$to" "$L" "$R" "$@" 2>&1)"; rc=$?
    secs=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF -- "$exp"; then
        echo "OK   $N: $name (${secs}s)"
    else
        echo "FAIL $N: $name (rc=$rc, ${secs}s)"; fail=1
        printf '%s\n' "$out" | tail -n 20 | sed 's/^/     | /'
    fi
    printf '%s\n' "$out" | head -n 8 | sed 's/^/     > /'
}
check "os-release"       "NAME="          30  /bin/bash -c 'cat /etc/os-release'
check "ld.so --version"  "libc"          30  /lib/ld-linux-aarch64.so.1 --version
check "bash"             "bash 5"         30  /bin/bash -c 'echo bash ${BASH_VERSINFO[0]}'
check "ls -la /"         "etc"            30  /bin/bash -c 'ls -la /'
check "uname -m"         "aarch64"        30  /bin/bash -c 'uname -m'
check "id"               "uid=0(root)"    30  /bin/bash -c id
check "pipeline"         "3"              30  /bin/bash -c 'ls /etc | head -n 3 | wc -l'
check "login shell"      "login-ok"       60  /bin/bash -l -c 'echo login-ok'
# a tab: an interactive login shell on a terminal
out="$(LXRUN_QUIET=1 LXRUN_KEYS='echo tty-$((40+2))\r\wexit\r' timeout 60 "$L" -i "$R" /bin/bash -l 2>&1)"; rc=$?
if printf '%s\n' "$out" | grep -q "tty-42"; then echo "OK   $N: interactive login shell on a terminal"
else
    echo "FAIL $N: interactive login shell on a terminal (rc=$rc)"; fail=1
    printf '%s\n' "$out" | tail -n 8 | sed 's/^/     | /'
    echo "== where it waits"
    LXRUN_QUIET=1 LXRUN_KEYS='echo tty-$((40+2))\r\wexit\r' timeout 40 "$L" -t -i "$R" /bin/bash -l 2>&1 | grep -E "^\[sys\]|exec|open |\[proc\]|\[tty\]" | tail -n 50
fi
check "rpm -q bash"      "bash-"          60  /usr/bin/rpm -q bash
check "rpm -qa count"    "pkgs"           120 /bin/bash -c 'echo "$(rpm -qa | wc -l) pkgs"'
if [ -x "$R/usr/bin/zypper" ]; then                               # openSUSE
check "zypper --version" "zypper"         120 /bin/bash -c 'zypper --version 2>&1 | head -n 2; echo zypper'
if [ "${3:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "zypper refresh"       "repositor"      2400 /bin/bash -c 'zypper --non-interactive refresh 2>&1 | tail -n 6'
    check "zypper install tmux"  "tmux "          2400 /bin/bash -c 'zypper --non-interactive install --no-recommends tmux 2>&1 | tail -n 6; tmux -V'
    check "zypper install python3" "499999500000" 2400 /bin/bash -c 'zypper --non-interactive install --no-recommends python3 2>&1 | tail -n 4; python3 -c "print(sum(range(10**6)))"'
fi
exit $fail
fi
check "dnf --version"    "dnf"            120 /bin/bash -c 'dnf --version 2>&1 | head -n 3; echo dnf'
if [ "${3:-}" = net ]; then
    cp /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null || echo "nameserver 1.1.1.1" > "$R/etc/resolv.conf"
    check "getent hosts"         "."              60   /usr/bin/getent hosts mirrors.fedoraproject.org
    check "dnf makecache"        "etadata"        2400 /bin/bash -c 'dnf -y makecache 2>&1 | tail -n 8'
    check "dnf install python3"  "Complete"       2400 /bin/bash -c 'dnf -y install --setopt=install_weak_deps=False python3 2>&1 | tail -n 12; command -v python3 && echo Complete'
    check "dnf install tmux"     "tmux "         2400 /bin/bash -c 'dnf -y install --setopt=install_weak_deps=False tmux 2>&1 | tail -n 6; tmux -V'
    check "python3"              "499999500000"   120  /bin/bash -c 'python3 -c "print(sum(range(10**6)))"'
    printf 'from multiprocessing import Pool\ndef f(x): return x * x\nif __name__ == "__main__":\n    with Pool(2) as p: print("pool", p.map(f, range(4)))\n' > "$R/tmp/mp.py"
    check "python3 multiprocessing" "pool [0, 1, 4, 9]" 300 /bin/bash -c 'python3 /tmp/mp.py'   # (semaphores in /dev/shm)
    printf 'import subprocess\np = subprocess.run(["python3", "-c", "import os; print(os.getsid(0) == os.getpid())"], start_new_session=True, capture_output=True, text=True)\nprint("new session", p.stdout.strip(), p.returncode)\n' > "$R/tmp/sess.py"
    check "subprocess start_new_session" "new session True 0" 120 /bin/bash -c 'python3 /tmp/sess.py'   # (setsid in a vfork child)
    printf 'import asyncio, os, stat\nP = "/tmp/t.sock"\nasync def main():\n    for _ in range(2):\n        s = await asyncio.start_unix_server(lambda r, w: w.close(), P)\n        print(stat.S_ISSOCK(os.stat(P).st_mode), end=" ")\n        s.close(); await s.wait_closed()\n    print("rebound")\nasyncio.run(main())\n' > "$R/tmp/usock.py"
    check "unix socket name is a socket" "True True rebound" 120 /bin/bash -c 'rm -f /tmp/t.sock; python3 /tmp/usock.py'   # (asyncio removes stale socket names)
    check "/proc/self/fd lists the fds" "fds ok" 60 /bin/bash -c 'python3 -c "import os; f = open(\"/etc/os-release\"); n = os.listdir(\"/proc/self/fd\"); print(\"fds ok\" if str(f.fileno()) in n and len(n) >= 4 else n)"; ls /proc/$$ > /dev/null && echo pid dir'
    cat > "$R/tmp/meta.py" <<'PY'
import os, stat
d = "/tmp/meta-t"; os.makedirs(d, exist_ok=True); f = d + "/f"
open(f, "w").close()
os.chmod(f, 0o444); m1 = stat.S_IMODE(os.stat(f).st_mode)
open(f, "a").write("root writes")                                  # root still writes a 0444 file
os.chown(f, 1234, 5678); st = os.stat(f)
os.chmod(f, 0o640); m2 = stat.S_IMODE(os.stat(f).st_mode); u2 = os.stat(f).st_uid
os.chown(f, 0, 0); os.chmod(f, 0o644)
old = os.umask(0); g = d + "/g"
if os.path.exists(g): os.unlink(g)
open(g, "w").close(); m3 = stat.S_IMODE(os.stat(g).st_mode)
os.mkdir(d + "/x", 0o555) if not os.path.exists(d + "/x") else os.chmod(d + "/x", 0o555); m4 = stat.S_IMODE(os.stat(d + "/x").st_mode)
os.umask(old)
os.utime(f, (1.5, 1000000.25)); mt = os.stat(f).st_mtime
os.setxattr(f, "user.lx-test", b"v1"); xa = os.getxattr(f, "user.lx-test"); xl = os.listxattr(f)
print("meta", oct(m1), st.st_uid, st.st_gid, oct(m2), u2, oct(m3), oct(m4), mt, xa, xl)
PY
    check "file modes, owners, times, xattrs" "meta 0o444 1234 5678 0o640 1234 0o666 0o555 1000000.25 b'v1' ['user.lx-test']" 60 /bin/bash -c 'python3 /tmp/meta.py; ls -l /tmp/meta-t/f'
    cat > "$R/tmp/alarm.py" <<'PY'
import os, select, signal, threading, time
class Boom(Exception): pass
def h(*a): raise Boom()
signal.signal(signal.SIGALRM, h)
r, w = os.pipe(); t0 = time.monotonic(); out = []
signal.alarm(1)
try: select.select([r], [], [], 20); out.append("no")
except Boom: out.append("select %.0f" % (time.monotonic() - t0))
lock = threading.Lock(); lock.acquire(); t0 = time.monotonic()
signal.setitimer(signal.ITIMER_REAL, 0.5)
try: lock.acquire(timeout=20); out.append("no")
except Boom: out.append("lock %.0f" % (time.monotonic() - t0))
print("alarm", *out)
PY
    check "alarm interrupts select and a lock" "alarm select 1 lock 1" 60 /bin/bash -c 'python3 /tmp/alarm.py'
    cat > "$R/tmp/pty.py" <<'PY'
import os, termios
m, s = os.openpty()
os.write(m, b"hi" + bytes([10]))
print("pty", os.ttyname(s).startswith("/dev/pts/"), os.read(s, 10) == b"hi" + bytes([10]), len(termios.tcgetattr(s)))
PY
    check "os.openpty and termios" "pty True True 7" 60 /bin/bash -c 'python3 /tmp/pty.py'
    cat > "$R/tmp/sigwait.py" <<'PY'
import os, signal, threading, time
S = signal.SIGUSR1
signal.pthread_sigmask(signal.SIG_BLOCK, [S])
os.kill(os.getpid(), S); time.sleep(0.2)
pend = S in signal.sigpending()
got = signal.sigwait([S])                                          # the pending one
t = threading.Thread(target=lambda: (time.sleep(0.5), os.kill(os.getpid(), S))); t.start()
got2 = signal.sigwait([S]); t.join()                               # sent by another thread
t0 = time.monotonic(); none = signal.sigtimedwait([S], 0.3); dt = time.monotonic() - t0
print("sigwait", pend, got == S, got2 == S, none, dt >= 0.25)
PY
    check "sigpending, sigwait, sigtimedwait" "sigwait True True True None True" 60 /bin/bash -c 'timeout 30 python3 /tmp/sigwait.py'
    if [ -x "$R/usr/bin/systemd-sysusers" ]; then                  # rpm's %sysusers scriptlets
        check "systemd-sysusers" "sysusers-ok" 120 /bin/bash -c 'printf "u lxtest - \"test\" /tmp\n" | systemd-sysusers - 2>&1; getent passwd lxtest && echo sysusers-ok'
        echo "== failing syscalls of systemd-sysusers"
        LXRUN_QUIET=1 timeout 120 "$L" -t "$R" /bin/bash -c 'printf "u lxtest2 - \"t\" /tmp\n" | systemd-sysusers -' 2>&1 |
            grep -E "= -[0-9]+|not impl" | grep -v "= -11$" | tail -n 50
    fi
fi
exit $fail
