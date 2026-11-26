#!/bin/sh
# Exploration: server programs (httpd, nginx, MariaDB) run directly under lxrun with every
# system call traced (child processes too); prints which calls fail and how, and how far
# each program got. The packages are installed natively in a copy, then copied over.
# usage: sudo sh tools/explore/daemons.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
cp /etc/resolv.conf "$R/etc/resolv.conf"
mount -t proc proc "$R/proc"; mount --rbind /dev "$R/dev"
chroot "$R" dnf -y -q --setopt=install_weak_deps=False install httpd nginx mariadb-server procps-ng > /dev/null 2>&1
umount -R "$R/dev" "$R/proc"
echo "127.0.0.1 iphone" >> "$R/etc/hosts"
sed -i 's/^Listen 80$/Listen 8080/' "$R/etc/httpd/conf/httpd.conf"
sed -i 's/listen       80;/listen       8081;/; s/listen       \[::\]:80;/listen       [::]:8081;/' "$R/etc/nginx/nginx.conf"

names() {   # syscall numbers -> names (aarch64), for the summary
    python3 - "$1" <<'EOF'
import re, sys, collections
nr = {17:"getcwd",19:"eventfd2",20:"epoll_create1",21:"epoll_ctl",22:"epoll_pwait",23:"dup",24:"dup3",25:"fcntl",29:"ioctl",
      32:"flock",33:"mknodat",34:"mkdirat",35:"unlinkat",37:"linkat",38:"renameat",43:"statfs",46:"ftruncate",47:"fallocate",
      48:"faccessat",49:"chdir",52:"fchmod",53:"fchmodat",54:"fchownat",55:"fchown",56:"openat",57:"close",59:"pipe2",
      61:"getdents64",62:"lseek",63:"read",64:"write",66:"writev",67:"pread64",68:"pwrite64",71:"sendfile",72:"pselect6",
      73:"ppoll",74:"signalfd4",78:"readlinkat",79:"newfstatat",80:"fstat",82:"fsync",83:"fdatasync",85:"timerfd_create",
      86:"timerfd_settime",88:"utimensat",90:"capget",91:"capset",93:"exit",94:"exit_group",96:"set_tid_address",98:"futex",
      99:"set_robust_list",101:"nanosleep",113:"clock_gettime",115:"clock_nanosleep",123:"sched_getaffinity",
      124:"sched_yield",129:"kill",130:"tkill",131:"tgkill",134:"rt_sigaction",135:"rt_sigprocmask",139:"rt_sigreturn",
      140:"setpriority",141:"getpriority",143:"setregid",144:"setgid",146:"setuid",147:"setresuid",149:"setresgid",
      153:"times",154:"setpgid",157:"setsid",160:"uname",163:"getrlimit",164:"setrlimit",165:"getrusage",166:"umask",
      167:"prctl",172:"getpid",173:"getppid",174:"getuid",175:"geteuid",178:"gettid",179:"sysinfo",186:"semget",
      187:"semctl",188:"semtimedop",189:"semop",190:"shmget",191:"shmctl",192:"shmat",193:"shmdt",194:"shmget",
      198:"socket",199:"socketpair",200:"bind",201:"listen",202:"accept",203:"connect",204:"getsockname",
      205:"getpeername",206:"sendto",207:"recvfrom",208:"setsockopt",209:"getsockopt",210:"shutdown",211:"sendmsg",
      212:"recvmsg",214:"brk",215:"munmap",216:"mremap",220:"clone",221:"execve",222:"mmap",226:"mprotect",
      227:"msync",232:"mincore",233:"madvise",242:"accept4",260:"wait4",261:"prlimit64",267:"syncfs",271:"process_vm_readv",
      276:"renameat2",277:"seccomp",278:"getrandom",279:"memfd_create",281:"execveat",291:"statx",293:"rseq",
      425:"io_uring_setup",435:"clone3",439:"faccessat2",441:"epoll_pwait2"}
c = collections.Counter()
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[sys\] +(\d+)\(.*\) = (-\d+)$", line.strip())
    if m and m.group(2) not in ("-11", "-115"):                    # (EAGAIN, EINPROGRESS: normal)
        c[(nr.get(int(m.group(1)), m.group(1)), int(m.group(2)))] += 1
for (n, e), k in c.most_common(30):
    print(f"   {k:6d}  {n} -> {e}")
EOF
}
run() {   # run NAME SECONDS COMMAND
    echo "== $1"
    rm -f /tmp/ct.log
    AOI_CHILD_TRACE=/tmp/ct.log LXRUN_QUIET=1 timeout "$2" "$L" -t "$R" /bin/bash -c "$3" > /tmp/out.log 2> /tmp/tr.log
    echo "   exit $? ; last output:"
    grep -v '^\[' /tmp/tr.log | tail -n 12 | sed 's/^/   | /'
    tail -n 12 /tmp/out.log | sed 's/^/   > /'
    cat /tmp/tr.log /tmp/ct.log 2>/dev/null > /tmp/all.log
    echo "   failing system calls (count, call -> -errno):"
    names /tmp/all.log
}
run httpd 40 '/usr/sbin/httpd -DFOREGROUND & sleep 12; curl -s -o /dev/null -w "httpd http %{http_code}\n" --max-time 10 http://127.0.0.1:8080/; kill %1; tail -n 8 /var/log/httpd/error_log'
run nginx 40 '/usr/sbin/nginx -g "daemon off;" & sleep 10; curl -s -o /dev/null -w "nginx http %{http_code}\n" --max-time 10 http://127.0.0.1:8081/; kill %1; tail -n 8 /var/log/nginx/error.log'
run mariadb-install-db 400 'mariadb-install-db --user=mysql --datadir=/var/lib/mysql > /tmp/mi.log 2>&1; echo rc=$?; grep -iE "error|fail|warn|\[" /tmp/mi.log | head -n 25'
run mariadbd 120 '/usr/libexec/mariadbd --user=mysql --basedir=/usr --datadir=/var/lib/mysql > /tmp/md.log 2>&1 & sleep 40; mariadb -e "select 6*7" 2>&1; head -n 30 /tmp/md.log'
