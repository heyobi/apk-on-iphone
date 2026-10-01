#!/usr/bin/env python3
"""tracefiles.py ROOT TRACE... : the guest files an `aoiproc -t` run opened, as a list
for tools/mini-root.sh (one guest path per line, sorted). Symlinks on the way are
listed too (mini-root.sh keeps them as links), with what they point to. Left out:
what the app or our build writes at run time (/data/app, /data/data, /data/user*,
aoi.dex, mapper.aoi.so), /dev and /proc. A file only ever opened with O_PATH (looked
at, never read: the .apex packages) is listed as "~path": mini-root.sh makes it empty. The program aoiproc execs (and its
interpreter) is loaded without an open: add it by hand."""
import os
import re
import sys

SKIP = re.compile(r"^/(dev|proc|sys)/|^/data/(app|data|user|user_de|misc)/|^/data/local/tmp/aoi\.dex$|"
                  r"^/vendor/lib64/hw/mapper\.aoi\.so$|^/data/dalvik-cache/")


def chain(root, g, out, depth=0):
    """g and every symlink on the way to the file it names, inside root."""
    if depth > 20:
        return
    parts = [p for p in g.split("/") if p]
    cur = ""
    for i, p in enumerate(parts):
        cur += "/" + p
        h = root + cur
        if os.path.islink(h):
            out.add(cur)
            t = os.readlink(h)
            t = t if t.startswith("/") else os.path.normpath(os.path.join(os.path.dirname(cur), t))
            chain(root, t + "".join("/" + q for q in parts[i + 1:]), out, depth + 1)
            return
    if os.path.isfile(root + cur):
        out.add(cur)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.split("\n\n")[0])
    root, out, read = sys.argv[1].rstrip("/"), set(), set()
    for t in sys.argv[2:]:
        pending = None
        for line in open(t, errors="replace"):
            m = re.match(r"\[sys\] open (\S+) -> \d", line)
            if m and not SKIP.search(m.group(1)):
                pending = m.group(1)
                chain(root, pending, out)
                continue
            m = re.match(r"\[sys\]\s+56\(0x[0-9a-f]+, 0x[0-9a-f]+, (0x[0-9a-f]+|0)", line)
            if m and pending:
                if not int(m.group(1), 16) & 0x200000:      # O_PATH
                    s2 = set()
                    chain(root, pending, s2)
                    read |= s2
                pending = None
    for g in sorted(out):
        if not SKIP.search(g):
            print(g if g in read or os.path.islink(root + g) else "~" + g)


if __name__ == "__main__":
    main()
