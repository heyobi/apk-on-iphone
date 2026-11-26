#!/bin/sh
# Exploration (.github/workflows/explore.yml): what AlmaLinux's dnf downloads and keeps
# as metadata, which settings change that, and what each costs under lxrun.
# usage: sudo sh tools/explore/meta_dnf.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
L="$DIR/build/lxrun"
NR="$R.native"
rm -rf "$NR"; cp -a "$R" "$NR"
for r in "$R" "$NR"; do cp /etc/resolv.conf "$r/etc/resolv.conf"; done
mount -t proc proc "$NR/proc"; mount --rbind /dev "$NR/dev"; mount --rbind /sys "$NR/sys"
trap 'umount -R "$NR/dev" "$NR/sys" "$NR/proc" 2>/dev/null' EXIT
n() { chroot "$NR" /bin/bash -c "$1"; }
t() {   # t LABEL CMD: wall time under lxrun
    t0=$(date +%s.%N)
    LXRUN_QUIET=1 timeout 2400 "$L" "$R" /bin/bash -c "$2" > /tmp/t.out 2>&1; rc=$?
    printf 'TIME %-60s %7.1f s (rc %d)\n' "$1" "$(echo "$(date +%s.%N) - $t0" | bc)" $rc
    tail -n 3 /tmp/t.out | sed 's/^/     | /'
}

echo "== dnf configuration that matters"
n 'python3 - <<EOF
import dnf
b = dnf.Base()
c = b.conf
for k in ("optional_metadata_types", "metadata_expire", "metadata_timer_sync", "fastestmirror", "max_parallel_downloads", "zchunk", "skip_if_unavailable", "keepcache", "install_weak_deps", "check_config_file_age"):
    try: print(k, "=", getattr(c, k))
    except Exception as e: print(k, "?", e)
b.read_all_repos()
for r in b.repos.iter_enabled():
    print("enabled repo:", r.id, "metadata_expire", r.metadata_expire, "baseurl", list(r.baseurl)[:1], "mirrorlist", r.mirrorlist, "metalink", r.metalink)
EOF'
echo "== strings in libdnf about optional metadata"
strings "$NR"/usr/lib64/libdnf.so.2 | grep -iE "optional_metadata|filelists|^other$" | sort -u | head -20
cat "$NR"/etc/dnf/dnf.conf

echo "== native makecache, default: what lands in the cache"
n 'dnf -y -q makecache >/dev/null 2>&1; cd /var/cache/dnf && du -sh . && find . -type f -size +100k -printf "%10s %p\n" | sort -n'
echo "== native, optional_metadata_types without filelists"
n 'dnf clean all >/dev/null; dnf -y -q --setopt=optional_metadata_types=comps,updateinfo makecache >/dev/null 2>&1; cd /var/cache/dnf && du -sh . && find . -type f -size +100k -printf "%10s %p\n" | sort -n'
echo "== native, filelists asked for explicitly"
n 'dnf clean all >/dev/null; dnf -y -q --setopt=optional_metadata_types=filelists,comps,updateinfo makecache >/dev/null 2>&1; cd /var/cache/dnf && du -sh . && find . -type f -size +100k -printf "%10s %p\n" | sort -n'
echo "== does a file dependency still resolve without filelists? (native)"
n 'dnf clean all >/dev/null; dnf -y -q --setopt=optional_metadata_types=comps,updateinfo --assumeno install /usr/bin/tmux 2>&1 | tail -n 4'

echo "== under lxrun"
t "cold makecache, default"                         'dnf clean all >/dev/null; dnf -y makecache'
t "warm makecache, default"                         'dnf -y makecache'
t "cold makecache, no filelists"                    'dnf clean all >/dev/null; dnf -y --setopt=optional_metadata_types=comps,updateinfo makecache'
t "warm makecache, no filelists"                    'dnf -y --setopt=optional_metadata_types=comps,updateinfo makecache'
t "cold makecache, no filelists/comps/updateinfo"   'dnf clean all >/dev/null; dnf -y --setopt=optional_metadata_types= makecache'
t "install tmux on cached metadata (-C)"            'dnf -y -C --setopt=install_weak_deps=False install tmux'
t "remove tmux (-C)"                                'dnf -y -C remove tmux'
t "install tmux, metadata_expire=never"             'dnf -y --setopt=metadata_expire=-1 --setopt=install_weak_deps=False install tmux'
