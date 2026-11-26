#!/usr/bin/env python3
"""The hot code pages of one library, from LXRUN_PROF profiles: the fewest 4 KiB pages
that hold COVER of its samples, as hex page addresses (file offsets = virtual addresses
for the libraries tools/aotgen.c takes), one per line - aotgen's PAGES file.
usage: prof_pages.py LIBRARY-BASENAME COVER PROFILE...   (COVER: 0.99 = 99%)"""
import collections, os, sys

lib, cover, files = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
pages = collections.Counter()
for name in files:
    for line in open(name):
        if line.startswith("#"):
            continue
        c, path, off = line.split()
        if os.path.basename(path) == lib:
            pages[int(off, 16) & ~0xfff] += int(c)
total = sum(pages.values())
acc, out = 0, []
for pg, c in pages.most_common():
    if total and acc / total >= cover:
        break
    out.append(pg)
    acc += c
for pg in sorted(out):
    print(f"{pg:x}")
print(f"{lib}: {len(out)} of {len(pages)} sampled pages hold {100 * acc / max(total, 1):.1f}% of {total} samples", file=sys.stderr)
