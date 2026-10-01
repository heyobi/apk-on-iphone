#!/usr/bin/env python3
"""mkdex.py OUT.dex [LOOPS]: write a minimal dex (no Android SDK needed) holding

    public class Hello {
        public static void main(String[] args) {
            int x = 0;
            for (int i = 0; i < LOOPS; i++) x = x + i;      // only with LOOPS
            System.out.println("Merhaba from ART");
            if (LOOPS) System.out.println(String.valueOf(x));
        }
    }

The bytecode is written by hand (no d8 in the cloud sessions: dl.google.com is
blocked), following the dex format: header, id tables, class_def, code_item,
type_lists, string_data, class_data, map_list, then adler32 and sha1.
"""
import hashlib
import struct
import sys
import zlib

MSG = "Merhaba from ART"


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def align(buf, n):
    while len(buf) % n:
        buf.append(0)


def build(loops):
    strings = ["LHello;", "Ljava/io/PrintStream;", "Ljava/lang/Object;", "Ljava/lang/String;",
               "Ljava/lang/System;", MSG, "V", "VL", "VI", "LI", "I", "[Ljava/lang/String;", "main",
               "out", "println", "valueOf"]
    strings = sorted(set(strings), key=lambda s: s.encode())
    S = {s: i for i, s in enumerate(strings)}
    types = sorted(["LHello;", "Ljava/io/PrintStream;", "Ljava/lang/Object;", "Ljava/lang/String;",
                    "Ljava/lang/System;", "V", "I", "[Ljava/lang/String;"], key=lambda t: S[t])
    T = {t: i for i, t in enumerate(types)}
    # protos: (shorty, return, params), sorted by return type, then params
    protos = [("VL", "V", ["Ljava/lang/String;"]), ("VL", "V", ["[Ljava/lang/String;"]),
              ("LI", "Ljava/lang/String;", ["I"])]
    protos.sort(key=lambda p: (T[p[1]], [T[x] for x in p[2]]))
    P = {p[2][0] + "->" + p[1]: i for i, p in enumerate(protos)}
    fields = [("Ljava/lang/System;", "Ljava/io/PrintStream;", "out")]
    methods = [("LHello;", "main", P["[Ljava/lang/String;->V"]),
               ("Ljava/io/PrintStream;", "println", P["Ljava/lang/String;->V"]),
               ("Ljava/lang/String;", "valueOf", P["I->Ljava/lang/String;"])]
    methods.sort(key=lambda m: (T[m[0]], S[m[1]], m[2]))
    M = {m[0] + "->" + m[1]: i for i, m in enumerate(methods)}

    # main's code. Registers: v0 out, v1 string, v2 x, v3 i, v4 limit, v5 = p0 (args)
    out, cs, vo, pl = 0, S[MSG], M["Ljava/lang/String;->valueOf"], M["Ljava/io/PrintStream;->println"]
    code = []
    if loops:
        code += [0x0212, 0x0312]                                   # const/4 v2,0 ; const/4 v3,0
        code += [0x0414, loops & 0xFFFF, loops >> 16]              # const v4, #LOOPS
        # loop: if-ge v3, v4, +6 ; add-int/2addr v2, v3 ; add-int/lit8 v3, v3, #1 ; goto -5
        code += [0x4335, 0x0006, 0x32B0, 0x03D8, 0x0103, 0xFB28]
    code += [0x0062, out, 0x011A, cs, 0x206E, pl, 0x0010]         # sget-object; const-string; invoke-virtual
    if loops:
        code += [0x1071, vo, 0x0002, 0x010C, 0x206E, pl, 0x0010]   # invoke-static valueOf(v2); move-result-object v1; println
    code += [0x000E]                                               # return-void
    regs, ins, outs = 6, 1, 2

    n_hdr = 0x70
    off = n_hdr
    off_strings = off; off += 4 * len(strings)
    off_types = off; off += 4 * len(types)
    off_protos = off; off += 12 * len(protos)
    off_fields = off; off += 8 * len(fields)
    off_methods = off; off += 8 * len(methods)
    off_classes = off; off += 32
    data = bytearray()
    data_start = off

    def here():
        return data_start + len(data)

    align(data, 4)
    off_code = here()
    data += struct.pack("<4H2I", regs, ins, outs, 0, 0, len(code)) + struct.pack("<%dH" % len(code), *code)
    align(data, 4)
    tl_off = {}
    off_typelists = here()
    for p in protos:
        key = tuple(p[2])
        if key in tl_off:
            continue
        align(data, 4)
        tl_off[key] = here()
        data += struct.pack("<I", len(key)) + struct.pack("<%dH" % len(key), *[T[x] for x in key])
    n_typelists = len(tl_off)
    off_strdata = here()
    str_off = []
    for s in strings:
        str_off.append(here())
        data += uleb(len(s)) + s.encode() + b"\0"
    off_classdata = here()
    data += uleb(0) + uleb(0) + uleb(1) + uleb(0) + uleb(M["LHello;->main"]) + uleb(0x9) + uleb(off_code)
    align(data, 4)
    off_map = here()
    maps = [(0x0000, 1, 0), (0x0001, len(strings), off_strings), (0x0002, len(types), off_types),
            (0x0003, len(protos), off_protos), (0x0004, len(fields), off_fields),
            (0x0005, len(methods), off_methods), (0x0006, 1, off_classes), (0x2001, 1, off_code),
            (0x1001, n_typelists, off_typelists), (0x2002, len(strings), off_strdata),
            (0x2000, 1, off_classdata), (0x1000, 1, off_map)]
    data += struct.pack("<I", len(maps)) + b"".join(struct.pack("<HHII", t, 0, n, o) for t, n, o in maps)

    ids = bytearray()
    ids += b"".join(struct.pack("<I", o) for o in str_off)
    ids += b"".join(struct.pack("<I", S[t]) for t in types)
    ids += b"".join(struct.pack("<III", S[p[0]], T[p[1]], tl_off[tuple(p[2])]) for p in protos)
    ids += b"".join(struct.pack("<HHI", T[c], T[t], S[n]) for c, t, n in fields)
    ids += b"".join(struct.pack("<HHI", T[c], pi, S[n]) for c, n, pi in methods)
    ids += struct.pack("<8I", T["LHello;"], 0x1, T["Ljava/lang/Object;"], 0, 0xFFFFFFFF, 0, off_classdata, 0)
    assert len(ids) == data_start - n_hdr

    size = data_start + len(data)
    hdr = bytearray(b"dex\n035\0" + bytes(n_hdr - 8))
    struct.pack_into("<IIIIII", hdr, 32, size, n_hdr, 0x12345678, 0, 0, off_map)
    struct.pack_into("<14I", hdr, 56, len(strings), off_strings, len(types), off_types, len(protos), off_protos,
                     len(fields), off_fields, len(methods), off_methods, 1, off_classes, len(data), data_start)
    dex = hdr + ids + data
    dex[12:32] = hashlib.sha1(dex[32:]).digest()
    struct.pack_into("<I", dex, 8, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__.split("\n\n")[0])
    open(sys.argv[1], "wb").write(build(int(sys.argv[2]) if len(sys.argv) > 2 else 0))
