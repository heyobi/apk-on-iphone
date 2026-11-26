#!/usr/bin/env python3
"""Turns an LXRUN_PROF file (count, guest path, file offset per sample site) into the
files and functions the guest spent its time in. Symbols come from the separate debug
files in ROOT/usr/lib/debug (dnf debuginfo-install) when present, else from the
library's own .symtab/.dynsym (then a static function shows as the exported one before
it, marked ~).
usage: prof_resolve.py PROFILE ROOT [TOP]"""
import bisect, collections, glob, os, struct, sys


def elf(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        raise ValueError("not ELF64")
    phoff, shoff = struct.unpack_from("<QQ", d, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", d, 0x36)
    loads = []
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", d, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_offset, p_vaddr, p_filesz))
    secs = []
    for i in range(shnum):
        secs.append(struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize))
    return d, loads, secs


def build_id(d, secs):
    for s in secs:
        if s[1] == 7:                                        # SHT_NOTE
            off, size = s[4], s[5]
            p = off
            while p + 12 <= off + size:
                namesz, descsz, typ = struct.unpack_from("<III", d, p)
                name_end = p + 12 + ((namesz + 3) & ~3)
                if typ == 3:                                 # NT_GNU_BUILD_ID
                    return d[name_end:name_end + descsz].hex()
                p = name_end + ((descsz + 3) & ~3)
    return None


def functions(d, secs):
    out = []
    for want in (2, 11):                                     # .symtab, else .dynsym
        for s in secs:
            if s[1] != want:
                continue
            strtab = secs[s[6]]
            for k in range(s[5] // 24):
                st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", d, s[4] + k * 24)
                if st_info & 0xf != 2 or not st_value:       # STT_FUNC
                    continue
                e = d.index(b"\0", strtab[4] + st_name)
                out.append((st_value, st_size, d[strtab[4] + st_name:e].decode(errors="replace")))
        if out:
            return sorted(out), want == 2
    return [], False


class Lib:
    def __init__(self, root, path):
        self.name = os.path.basename(path)
        host = root + path
        self.loads, self.syms, self.full, self.starts = [], [], False, []   # (not an ELF file, e.g. [lxfast]: no names)
        try:
            d, self.loads, secs = elf(host)
        except Exception:
            return
        dbg = None
        bid = build_id(d, secs)
        if bid:
            link = os.path.join(root, "usr/lib/debug/.build-id", bid[:2], bid[2:] + ".debug")
            if os.path.islink(link):
                t = os.readlink(link)
                dbg = os.path.normpath(os.path.join(os.path.dirname(link), t)) if not t.startswith("/") else root + t
        if not dbg:
            real = os.path.realpath(host)[len(os.path.realpath(root)):]
            g = glob.glob(os.path.join(root, "usr/lib/debug" + real + "*.debug"))
            dbg = g[0] if g else None
        if dbg and os.path.exists(dbg):
            try:
                dd, _, dsecs = elf(dbg)
                self.syms, self.full = functions(dd, dsecs)
            except Exception:
                pass
        if not self.syms:
            self.syms, self.full = functions(d, secs)
        self.starts = [s[0] for s in self.syms]

    def name_of(self, off):
        va = off
        for p_offset, p_vaddr, p_filesz in self.loads:
            if p_offset <= off < p_offset + p_filesz:
                va = off - p_offset + p_vaddr
                break
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0:
            return "?"
        start, size, name = self.syms[i]
        if size and va >= start + size:
            return "~" + name if not self.full else "?"
        return name if (self.full or size) else "~" + name


def main():
    prof, root = sys.argv[1], sys.argv[2].rstrip("/")
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    files, funcs, total, head = collections.Counter(), collections.Counter(), 0, ""
    libs = {}
    for line in open(prof):
        if line.startswith("#"):
            head = line.strip()
            continue
        c, path, off = line.split()
        c, off = int(c), int(off, 16)
        total += c
        files[path] += c
        if path == "?":
            funcs["?"] += c
            continue
        if path not in libs:
            libs[path] = Lib(root, path)
        funcs[f"{libs[path].name}:{libs[path].name_of(off)}"] += c
    print(head)
    print(f"-- files ({total} samples)")
    for p, c in files.most_common(15):
        print(f"{100 * c / total:6.1f}%  {p}")
    print("-- functions")
    for p, c in funcs.most_common(top):
        print(f"{100 * c / total:6.1f}%  {p}")
    full = sorted(os.path.basename(p) for p, l in libs.items() if l.full)
    print("-- full symbols (debug files) for: " + (", ".join(full) or "none"))


if __name__ == "__main__":
    main()
