#!/usr/bin/env python3
"""swcodec-ns.py ROOT: Android's software codecs in the app's process (guest/media.c).
On a phone they run in the media.swcodec process, with the swcodec APEX's own
libraries. Here the app process loads libmedia_codecserviceregistrant.so: each
library of that APEX that /system/lib64 does not have gets a symlink there (the
system namespace then finds them, and the ones both have are the system's: one
libhidlbase, one hwbinder state in the process), and public.libraries.txt names the
registrant, so an app's class-loader namespace may load it. The linker checks a
library's real path: linkerconfig's [system] section permits the APEX's directory
to the default namespace. Idempotent."""
import os
import sys

APEX = "apex/com.android.media.swcodec/lib64"
LIB = "libmedia_codecserviceregistrant.so"


def main():
    root = sys.argv[1]
    sysdir = os.path.join(root, "system/lib64")
    n = 0
    for name in sorted(os.listdir(os.path.join(root, APEX))):
        if not name.endswith(".so"):
            continue
        dst = os.path.join(sysdir, name)
        if os.path.lexists(dst):
            continue
        os.symlink(os.path.join("../..", APEX, name), dst)
        n += 1
    cfg = os.path.join(root, "linkerconfig/ld.config.txt")
    lines, out, section, done = open(cfg).read().split("\n"), [], None, False
    permit = "namespace.default.permitted.paths += /apex/com.android.media.swcodec/${LIB}"
    if permit not in lines:
        for l in lines:
            if l.startswith("["):
                section = l.strip()
            out.append(l)
            if section == "[system]" and not done and l.startswith("namespace.default.permitted.paths = "):
                out.append(permit)
                done = True
        open(cfg, "w").write("\n".join(out))
    pl = os.path.join(root, "system/etc/public.libraries.txt")
    text = open(pl).read()
    if LIB not in text:
        open(pl, "a").write(f"{LIB} nopreload\n")
    print(f"swcodec: {n} libraries linked into /system/lib64", file=sys.stderr)


if __name__ == "__main__":
    main()
