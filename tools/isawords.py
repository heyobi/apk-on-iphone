#!/usr/bin/env python3
"""isawords: list every distinct instruction word in AArch64 .so files.

usage: isawords.py lib.so [...] > words.txt      (needs llvm-objdump)
Output: one line per distinct word, "hexword mnemonic count", most frequent first.
Feed it to build/isacheck to see which ones our CPU is missing or gets wrong."""
import collections, re, subprocess, sys

line = re.compile(r"^\s+[0-9a-f]+:\s+([0-9a-f]{8})\s+(\S+)")
count, mnem = collections.Counter(), {}
for path in sys.argv[1:]:
    out = subprocess.run(["llvm-objdump", "-d", path], capture_output=True, text=True, check=True).stdout
    for l in out.splitlines():
        m = line.match(l)
        if m and not m.group(2).startswith("<"):
            count[m.group(1)] += 1
            mnem[m.group(1)] = m.group(2)
for w, n in count.most_common():
    print(w, mnem[w], n)
