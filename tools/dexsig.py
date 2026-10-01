#!/usr/bin/env python3
"""dexsig.py FILE.{dex,jar,apk} CLASS...: the methods and fields dex code refers to
on each CLASS (e.g. android.app.IApplicationThread), with full types, from every
classes*.dex inside. Used to write java/stubs against the exact framework of the
guest root (hidden API signatures change between Android versions)."""
import struct
import sys
import zipfile


def uleb(b, i):
    v = s = 0
    while True:
        x = b[i]; i += 1
        v |= (x & 0x7F) << s; s += 7
        if not x & 0x80:
            return v, i


def methods(dex, want):
    def u32(o): return struct.unpack_from("<I", dex, o)[0]
    n_str, o_str = u32(0x38), u32(0x3C)
    n_typ, o_typ = u32(0x40), u32(0x44)
    o_pro = u32(0x4C)
    n_fld, o_fld = u32(0x50), u32(0x54)
    n_met, o_met = u32(0x58), u32(0x5C)

    def string(i):
        off = u32(o_str + 4 * i)
        _, off = uleb(dex, off)
        end = dex.index(b"\0", off)
        return dex[off:end].decode("utf-8", "replace")

    def typ(i): return string(u32(o_typ + 4 * i))

    def pretty(t):
        dims = len(t) - len(t.lstrip("["))
        t = t[dims:]
        base = {"V": "void", "Z": "boolean", "B": "byte", "S": "short", "C": "char", "I": "int",
                "J": "long", "F": "float", "D": "double"}.get(t, t[1:-1].replace("/", ".") if t.startswith("L") else t)
        return base + "[]" * dims

    for k in range(n_fld):
        cls, ft, name = struct.unpack_from("<HHI", dex, o_fld + 8 * k)
        c = pretty(typ(cls))
        if c in want:
            yield c, "field %s %s" % (pretty(typ(ft)), string(name))
    for k in range(n_met):
        cls, pro, name = struct.unpack_from("<HHI", dex, o_met + 8 * k)
        c = pretty(typ(cls))
        if c not in want:
            continue
        ret, poff = struct.unpack_from("<II", dex, o_pro + 12 * pro + 4)
        params = []
        if poff:
            n = u32(poff)
            params = [pretty(typ(struct.unpack_from("<H", dex, poff + 4 + 2 * j)[0])) for j in range(n)]
        yield c, "%s %s(%s)" % (pretty(typ(ret)), string(name), ", ".join(params))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.split("\n\n")[0])
    path, want = sys.argv[1], set(sys.argv[2:])
    dexes = []
    if path.endswith(".dex"):
        dexes.append(open(path, "rb").read())
    else:
        with zipfile.ZipFile(path) as z:
            dexes = [z.read(n) for n in z.namelist() if n.startswith("classes") and n.endswith(".dex")]
    seen = set()
    for d in dexes:
        for c, m in methods(d, want):
            if (c, m) not in seen:
                seen.add((c, m))
                print("%s: %s" % (c, m))


if __name__ == "__main__":
    main()
