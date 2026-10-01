#!/usr/bin/env python3
"""aidlcodes.py LIB.so: the transaction code of each C++ AIDL client method in LIB
(e.g. libgui.so: ISurfaceComposer, IDisplayEventConnection, ...). C++ AIDL numbers a
method by its place in the .aidl file, which changes between builds; the generated
client loads a trace string "AIDL::cpp::IFoo::method::cppClient" and then calls
transact(code, ...), so the first `mov w1, #code` after the string's adrp/add is it.
Methods whose code is 1 (FIRST_CALL_TRANSACTION) may show as None (set differently)."""
import re
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[0])
    lib = sys.argv[1]
    data = open(lib, "rb").read()
    secs = []
    for l in subprocess.run(["llvm-readelf", "-SW", lib], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", l)
        if m:
            secs.append((int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)))

    def va(off):
        for v, o, n in secs:
            if v and o <= off < o + n:
                return v + off - o

    strs = {}
    for m in re.finditer(rb"AIDL::cpp::(\w+)::(\w+)::cppClient\0", data):
        strs[va(m.start())] = (m.group(1).decode(), m.group(2).decode())
    lines = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", lib], capture_output=True,
                           text=True).stdout.splitlines()
    adrp, res = {}, {}
    for i, l in enumerate(lines):
        m = re.match(r"\s*[0-9a-f]+:\s+adrp\s+(x\d+), 0x([0-9a-f]+)", l)
        if m:
            adrp[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r"\s*[0-9a-f]+:\s+add\s+(x\d+), (x\d+), #0x([0-9a-f]+)", l)
        if m and m.group(2) in adrp:
            key = strs.get(adrp[m.group(2)] + int(m.group(3), 16))
            if key and key not in res:
                code = None
                for k in range(i + 1, min(i + 120, len(lines))):
                    mm = re.search(r"mov\s+w1, #(0x[0-9a-f]+|\d+)\b", lines[k])
                    if mm:
                        code = int(mm.group(1), 0)
                        break
                res[key] = code
    for (iface, name), code in sorted(res.items(), key=lambda x: (x[0][0], x[1] or 1)):
        print("%s %s %s" % (iface, code if code is not None else 1, name))


if __name__ == "__main__":
    main()
