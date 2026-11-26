#!/usr/bin/env python3
"""The hot functions of the libraries in LXRUN_PROF profiles, for tools/aotgen.c: the fewest
functions (by samples, across all the libraries) that hold COVER of the samples in them.
Function bounds come from the libraries' symbols (the debuginfo files in ROOT/usr/lib/debug
when installed, as prof_resolve.py finds them); a sample outside any known function brings
the 512 bytes around it. Writes, per library, OUTDIR/<basename>.ranges: "start end" in hex
(file offsets = virtual addresses), one range per line.
usage: prof_funcs.py ROOT COVER OUTDIR PROFILE...   (COVER: 0.97 = 97%)"""
import bisect, collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof_resolve import Lib   # noqa: E402

root, cover, outdir, files = sys.argv[1].rstrip("/"), float(sys.argv[2]), sys.argv[3], sys.argv[4:]
samples = collections.Counter()           # (path, offset) -> count
for name in files:
    for line in open(name):
        if line.startswith("#"):
            continue
        c, path, off = line.split()
        if path != "?":
            samples[(path, int(off, 16))] += int(c)

libs, funcs = {}, collections.Counter()   # (path, start, end) -> samples
for (path, off), c in samples.items():
    if path not in libs:
        libs[path] = Lib(root, path)
    L = libs[path]
    i = bisect.bisect_right(L.starts, off) - 1     # (vaddr = offset for the libraries aotgen takes)
    if i >= 0 and L.syms[i][1] and off < L.syms[i][0] + L.syms[i][1]:
        start, size, _ = L.syms[i]
        funcs[(path, start, start + size)] += c
    else:
        funcs[(path, max(0, off - 256) & ~3, (off + 256) & ~3)] += c

total = sum(funcs.values())
acc, chosen = 0, collections.defaultdict(list)
for (path, s, e), c in funcs.most_common():
    if total and acc / total >= cover:
        break
    chosen[path].append((s, e))
    acc += c
os.makedirs(outdir, exist_ok=True)
for path, rs in chosen.items():
    with open(os.path.join(outdir, os.path.basename(path) + ".ranges"), "w") as f:
        for s, e in sorted(rs):
            f.write(f"{s:x} {e:x}\n")
    print(f"{os.path.basename(path)}: {len(rs)} functions, {sum(e - s for s, e in rs) // 4} instructions", file=sys.stderr)
print(f"chosen: {sum(len(v) for v in chosen.values())} functions holding {100 * acc / max(total, 1):.1f}% of {total} samples", file=sys.stderr)
