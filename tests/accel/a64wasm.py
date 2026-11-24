#!/usr/bin/env python3
"""Accelerator prototype: translates a static AArch64 Linux program (integer subset)
into one WebAssembly function, to measure what the "WebKit JIT" route (docs/RESEARCH.md:
translate guest code to Wasm and let the JavaScript engine compile it) could gain over
the interpreter.

Layout: every guest basic block becomes a labelled section of a dispatch loop
(loop + br_table over instruction slots); x0-x30, sp and NZCV are Wasm locals; guest
memory is the Wasm memory, offset by the lowest loaded address. svc calls an import
(write) or returns (exit). Anything outside the subset stops the translation with the
instruction word, so the gap is visible.

usage: a64wasm.py PROGRAM.elf OUT.wat OUT.json OUT.mem
"""
import json
import struct
import sys


def bits(v, hi, lo):
    return (v >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def ones(n):
    return (1 << n) - 1


def ror(v, r, w):
    r %= w
    return ((v >> r) | (v << (w - r))) & ones(w) if r else v


def decode_bit_masks(n, imms, immr, w, immediate):
    """ARM ARM DecodeBitMasks: (wmask, tmask) or None."""
    x = (n << 6) | (~imms & 0x3f)
    if x == 0:
        return None
    ln = x.bit_length() - 1
    if ln < 1:
        return None
    levels = ones(ln)
    if immediate and (imms & levels) == levels:
        return None
    s = imms & levels
    r = immr & levels
    diff = (s - r) & ones(6)
    esize = 1 << ln
    d = diff & levels
    welem = ones(s + 1)
    telem = ones(d + 1)
    wm = ror(welem, r, esize)
    rep_w = 0
    rep_t = 0
    for i in range(0, w, esize):
        rep_w |= wm << i
        rep_t |= telem << i
    return rep_w & ones(w), rep_t & ones(w)


class Unsupported(Exception):
    pass


def load_elf(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'\x7fELF' and b[4] == 2, 'ELF64 only'
    e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', b, 24)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHHHH', b, 54)
    segs = []
    for i in range(e_phnum):
        p_type, p_flags, p_off, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', b, e_phoff + i * e_phentsize)
        if p_type == 1:
            segs.append((p_vaddr, b[p_off:p_off + p_filesz], p_memsz, p_flags))
    text = None
    shstr = struct.unpack_from('<IIQQQQIIQQ', b, e_shoff + e_shstrndx * e_shentsize)
    for i in range(e_shnum):
        sh = struct.unpack_from('<IIQQQQIIQQ', b, e_shoff + i * e_shentsize)
        name = b[shstr[4] + sh[0]:].split(b'\x00', 1)[0]
        if name == b'.text':
            text = (sh[3], sh[5])                           # sh_addr, sh_size
    return e_entry, segs, text


class T:
    """Translator state for one program."""

    def __init__(self, entry, segs, text):
        self.entry = entry
        self.base = min(s[0] for s in segs) & ~0xfff
        top = max(s[0] + s[2] for s in segs)
        self.stack_size = 1 << 20
        self.mem_size = ((top - self.base + 0xffff) & ~0xffff) + self.stack_size
        self.image = bytearray(top - self.base)
        self.text = None
        for va, data, memsz, flags in segs:
            self.image[va - self.base:va - self.base + len(data)] = data
            if flags & 1:                                   # PF_X
                self.text = (va, data)
        self.tbase, tsize = text
        off = self.tbase - self.base
        self.words = [struct.unpack_from('<I', self.image, off + i)[0] for i in range(0, tsize - 3, 4)]
        self.n = len(self.words)

    # ---------- expression helpers ----------
    def addr(self, e):
        """guest address (i64 expr) -> Wasm memory offset (i32 expr)"""
        return f'(i32.wrap_i64 (i64.sub {e} (i64.const {self.base})))'

    @staticmethod
    def R(r, w32, sp=False):
        if r == 31:
            if sp:
                e = '(local.get $sp)'
            else:
                return '(i32.const 0)' if w32 else '(i64.const 0)'
        else:
            e = f'(local.get $x{r})'
        return f'(i32.wrap_i64 {e})' if w32 else e

    @staticmethod
    def W(r, v, w32, sp=False):
        if w32:
            v = f'(i64.extend_i32_u {v})'
        if r == 31 and not sp:
            return f'(drop {v})'
        return f'(local.set {"$sp" if r == 31 else f"$x{r}"} {v})'

    @staticmethod
    def ty(w32):
        return 'i32' if w32 else 'i64'

    def cond(self, c):
        base = c >> 1
        e = ['(local.get $z)', '(local.get $c)', '(local.get $n)', '(local.get $v)',
             '(i32.and (local.get $c) (i32.eqz (local.get $z)))',
             '(i32.eq (local.get $n) (local.get $v))',
             '(i32.and (i32.eqz (local.get $z)) (i32.eq (local.get $n) (local.get $v)))',
             '(i32.const 1)'][base]
        if (c & 1) and c != 15:
            e = f'(i32.eqz {e})'
        return e

    def flags_nz(self, w32):
        t = self.ty(w32)
        tr = '$ur' if w32 else '$tr'
        return [f'(local.set $n ({t}.lt_s (local.get {tr}) ({t}.const 0)))',
                f'(local.set $z ({t}.eqz (local.get {tr})))']

    def addsub(self, a, b, w32, sub, setf, rd, rd_sp):
        """a, b: exprs of the right width; result written to rd."""
        t = self.ty(w32)
        ta, tb, tr = ('$ua', '$ub', '$ur') if w32 else ('$ta', '$tb', '$tr')
        out = [f'(local.set {ta} {a})', f'(local.set {tb} {b})',
               f'(local.set {tr} ({t}.{"sub" if sub else "add"} (local.get {ta}) (local.get {tb})))']
        if setf:
            out += self.flags_nz(w32)
            if sub:
                out.append(f'(local.set $c ({t}.ge_u (local.get {ta}) (local.get {tb})))')
                out.append(f'(local.set $v ({t}.lt_s ({t}.and ({t}.xor (local.get {ta}) (local.get {tb})) ({t}.xor (local.get {ta}) (local.get {tr}))) ({t}.const 0)))')
            else:
                out.append(f'(local.set $c ({t}.lt_u (local.get {tr}) (local.get {ta})))')
                out.append(f'(local.set $v ({t}.lt_s ({t}.and ({t}.xor ({t}.xor (local.get {ta}) (local.get {tb})) ({t}.const -1)) ({t}.xor (local.get {ta}) (local.get {tr}))) ({t}.const 0)))')
        out.append(self.W(rd, f'(local.get {tr})', w32, rd_sp))
        return out

    def shifted(self, r, w32, typ, amt):
        t = self.ty(w32)
        e = self.R(r, w32)
        if amt == 0:
            return e
        op = ['shl', 'shr_u', 'shr_s', 'rotr'][typ]
        return f'({t}.{op} {e} ({t}.const {amt}))'

    def extended(self, r, w32, option, sh):
        """extended register operand (add/sub extended), result width w32"""
        e = self.R(r, False)
        e = [f'(i64.and {e} (i64.const 255))', f'(i64.and {e} (i64.const 65535))',
             f'(i64.and {e} (i64.const 4294967295))', e,
             f'(i64.extend8_s {e})', f'(i64.extend16_s {e})', f'(i64.extend32_s {e})', e][option]
        if sh:
            e = f'(i64.shl {e} (i64.const {sh}))'
        return f'(i32.wrap_i64 {e})' if w32 else e

    def goto(self, target):
        if not (self.tbase <= target < self.tbase + 4 * self.n):
            raise Unsupported(f'branch outside text to {target:#x}')
        return [f'(local.set $idx (i32.const {(target - self.tbase) // 4}))', '(br $top)']

    def goto_dyn(self, e):
        return [f'(local.set $idx (i32.wrap_i64 (i64.shr_u (i64.sub {e} (i64.const {self.tbase})) (i64.const 2))))', '(br $top)']

    # ---------- loads and stores ----------
    def mem_op(self, load, size, signed, to32, rt, ea, fp=False):
        """ea: i64 expr for the address. Returns statements."""
        a = self.addr(ea)
        if load:
            if size == 3:
                v = f'(i64.load {a})'
            elif signed and to32:
                v = f'(i64.extend_i32_u (i32.load{[8, 16][size]}_s {a}))'
            else:
                v = f'(i64.load{[8, 16, 32][size]}_{"s" if signed else "u"} {a})'
            return [self.W(rt, v, False)]
        v = self.R(rt, size < 3)
        if size == 3:
            return [f'(i64.store {a} {v})']
        return [f'(i32.store{["8", "16", ""][size]} {a} {v})']

    # ---------- one instruction ----------
    def insn(self, i, w):
        pc = self.tbase + 4 * i
        sf = w >> 31
        w32 = not sf
        t = self.ty(w32)
        rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
        nxt = pc + 4

        # branches
        if w & 0x7c000000 == 0x14000000:                    # b / bl
            tgt = pc + (sext(w & 0x3ffffff, 26) << 2)
            out = [f'(local.set $x30 (i64.const {nxt}))'] if w >> 31 else []
            return out + self.goto(tgt), True
        if w & 0xff000010 == 0x54000000:                    # b.cond
            tgt = pc + (sext(bits(w, 23, 5), 19) << 2)
            return [f'(if {self.cond(w & 15)} (then {" ".join(self.goto(tgt))}))'], True
        if w & 0x7e000000 == 0x34000000:                    # cbz / cbnz
            tgt = pc + (sext(bits(w, 23, 5), 19) << 2)
            e = f'({t}.eqz {self.R(rd, w32)})'
            if w >> 24 & 1:
                e = f'(i32.eqz {e})'
            return [f'(if {e} (then {" ".join(self.goto(tgt))}))'], True
        if w & 0x7e000000 == 0x36000000:                    # tbz / tbnz
            b = (w >> 26 & 0x20) | bits(w, 23, 19)
            tgt = pc + (sext(bits(w, 18, 5), 14) << 2)
            e = f'(i64.eqz (i64.and {self.R(rd, False)} (i64.const {1 << b})))'
            if w >> 24 & 1:
                e = f'(i32.eqz {e})'
            return [f'(if {e} (then {" ".join(self.goto(tgt))}))'], True
        if w & 0xfffffc1f == 0xd65f0000 or w & 0xfffffc1f == 0xd61f0000:   # ret / br
            return self.goto_dyn(self.R(rn, False)), True
        if w & 0xfffffc1f == 0xd63f0000:                    # blr
            return ['(local.set $ta ' + self.R(rn, False) + ')', f'(local.set $x30 (i64.const {nxt}))'] + self.goto_dyn('(local.get $ta)'), True
        if w == 0xd4000001:                                 # svc #0
            return ['(if (i32.or (i64.eq (local.get $x8) (i64.const 93)) (i64.eq (local.get $x8) (i64.const 94))) (then (return (local.get $x0))))',
                    '(local.set $x0 (call $sys (local.get $x8) (local.get $x0) (local.get $x1) (local.get $x2)))'], False
        if w & 0xfffff01f == 0xd503201f:                    # hints: nop, bti, pac*
            return [], False

        # data processing, immediate
        if w & 0x1f000000 == 0x11000000 and not (w >> 23 & 1):   # add/sub (immediate)
            op, s = w >> 30 & 1, w >> 29 & 1
            imm = bits(w, 21, 10) << (12 if w >> 22 & 1 else 0)
            return self.addsub(self.R(rn, w32, True), f'({t}.const {imm})', w32, op, s, rd, not s), False
        if w & 0x1f800000 == 0x12000000:                    # logical (immediate)
            opc, nbit = w >> 29 & 3, w >> 22 & 1
            m = decode_bit_masks(nbit, bits(w, 15, 10), bits(w, 21, 16), 32 if w32 else 64, True)
            if m is None:
                raise Unsupported('bad bitmask')
            imm = m[0]
            if imm >= 1 << (31 if w32 else 63):
                imm -= 1 << (32 if w32 else 64)
            op = ['and', 'or', 'xor', 'and'][opc]
            e = f'({t}.{op} {self.R(rn, w32)} ({t}.const {imm}))'
            if opc == 3:
                tr = '$ur' if w32 else '$tr'
                return [f'(local.set {tr} {e})'] + self.flags_nz(w32) + ['(local.set $c (i32.const 0))', '(local.set $v (i32.const 0))',
                                                                          self.W(rd, f'(local.get {tr})', w32)], False
            return [self.W(rd, e, w32, True)], False
        if w & 0x1f800000 == 0x12800000:                    # movn / movz / movk
            opc, hw = w >> 29 & 3, bits(w, 22, 21)
            imm = bits(w, 20, 5) << (16 * hw)
            if opc == 0:
                v = ~imm & ones(32 if w32 else 64)
            elif opc == 2:
                v = imm
            elif opc == 3:
                mask = ~(0xffff << (16 * hw)) & ones(64)
                if mask >= 1 << 63:
                    mask -= 1 << 64
                return [self.W(rd, f'(i64.or (i64.and {self.R(rd, False)} (i64.const {mask})) (i64.const {imm}))' if not w32 else
                               f'(i32.wrap_i64 (i64.or (i64.and {self.R(rd, False)} (i64.const {mask & 0xffffffff})) (i64.const {imm})))', w32)], False
            else:
                raise Unsupported('mov opc 1')
            if v >= 1 << (31 if w32 else 63):
                v -= 1 << (32 if w32 else 64)
            return [self.W(rd, f'({t}.const {v})', w32)], False
        if w & 0x1f800000 == 0x13000000:                    # sbfm / bfm / ubfm
            opc, nbit, immr, imms = w >> 29 & 3, w >> 22 & 1, bits(w, 21, 16), bits(w, 15, 10)
            width = 32 if w32 else 64
            m = decode_bit_masks(nbit, imms, immr, width, False)
            if m is None or opc == 3:
                raise Unsupported('bitfield')
            wm, tm = m

            def c(v):
                return v - (1 << width) if v >= 1 << (width - 1) else v
            src = self.R(rn, w32)
            bot = f'({t}.and ({t}.rotr {src} ({t}.const {immr})) ({t}.const {c(wm)}))'
            if opc == 2:                                    # ubfm
                return [self.W(rd, f'({t}.and {bot} ({t}.const {c(tm)}))', w32)], False
            if opc == 0:                                    # sbfm
                top = f'({t}.shr_s ({t}.shl {src} ({t}.const {width - 1 - imms})) ({t}.const {width - 1}))'
                return [self.W(rd, f'({t}.or ({t}.and {top} ({t}.const {c(~tm & ones(width))})) ({t}.and {bot} ({t}.const {c(tm)})))', w32)], False
            dst = self.R(rd, w32)                           # bfm
            bot = f'({t}.or ({t}.and {dst} ({t}.const {c(~wm & ones(width))})) {bot})'
            return [self.W(rd, f'({t}.or ({t}.and {dst} ({t}.const {c(~tm & ones(width))})) ({t}.and {bot} ({t}.const {c(tm)})))', w32)], False
        if w & 0x7fa00000 == 0x13800000:                    # extr
            lsb = bits(w, 15, 10)
            if lsb == 0:
                return [self.W(rd, self.R(rm, w32), w32)], False
            width = 32 if w32 else 64
            return [self.W(rd, f'({t}.or ({t}.shr_u {self.R(rm, w32)} ({t}.const {lsb})) ({t}.shl {self.R(rn, w32)} ({t}.const {width - lsb})))', w32)], False
        if w & 0x1f000000 == 0x10000000:                    # adr / adrp
            imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
            v = ((pc & ~0xfff) + (imm << 12)) if w >> 31 else pc + imm
            return [self.W(rd, f'(i64.const {v})', False)], False

        # loads and stores
        if w & 0x3b000000 == 0x39000000:                    # ldr/str (unsigned offset)
            size, opc = w >> 30, bits(w, 23, 22)
            if w >> 26 & 1:
                raise Unsupported('fp ldr/str')
            if size == 3 and opc == 2:
                return [], False                            # prfm
            ea = f'(i64.add {self.R(rn, False, True)} (i64.const {bits(w, 21, 10) << size}))'
            return self.ld_st(size, opc, rd, ea), False
        if w & 0x3b200000 == 0x38000000:                    # unscaled / pre / post index
            size, opc, mode = w >> 30, bits(w, 23, 22), bits(w, 11, 10)
            if w >> 26 & 1:
                raise Unsupported('fp ldur/stur')
            imm = sext(bits(w, 20, 12), 9)
            if size == 3 and opc == 2:
                return [], False
            if mode in (0, 2):
                return self.ld_st(size, opc, rd, f'(i64.add {self.R(rn, False, True)} (i64.const {imm}))'), False
            base = self.R(rn, False, True)
            if mode == 3:                                   # pre
                out = [f'(local.set $tb (i64.add {base} (i64.const {imm})))']
                out += self.ld_st(size, opc, rd, '(local.get $tb)')
            else:                                           # post
                out = [f'(local.set $tb {base})']
                out += self.ld_st(size, opc, rd, '(local.get $tb)')
                out.append('(local.set $tb (i64.add (local.get $tb) (i64.const %d)))' % imm)
            out.append(self.W(rn, '(local.get $tb)', False, True))
            return out, False
        if w & 0x3b200c00 == 0x38200800:                    # register offset
            size, opc, option, s = w >> 30, bits(w, 23, 22), bits(w, 15, 13), w >> 12 & 1
            if w >> 26 & 1:
                raise Unsupported('fp ldr reg')
            if size == 3 and opc == 2:
                return [], False
            off = self.extended(rm, False, option, size if s else 0)
            return self.ld_st(size, opc, rd, f'(i64.add {self.R(rn, False, True)} {off})'), False
        if w & 0x3b000000 == 0x18000000:                    # ldr (literal)
            opc = w >> 30
            if w >> 26 & 1:
                raise Unsupported('fp ldr literal')
            ea = f'(i64.const {pc + (sext(bits(w, 23, 5), 19) << 2)})'
            if opc == 3:
                return [], False
            return self.mem_op(True, [2, 3, 2][opc], opc == 2, False, rd, ea), False
        if w & 0x3a000000 == 0x28000000:                    # ldp / stp
            opc, typ, load = w >> 30, bits(w, 24, 23), w >> 22 & 1
            if w >> 26 & 1:
                raise Unsupported('fp ldp/stp')
            rt2 = bits(w, 14, 10)
            size = 3 if opc == 2 else 2
            imm = sext(bits(w, 21, 15), 7) << size
            base = self.R(rn, False, True)
            if typ == 1:
                out = [f'(local.set $tb {base})']
                ea = '(local.get $tb)'
            elif typ == 3:
                out = [f'(local.set $tb (i64.add {base} (i64.const {imm})))']
                ea = '(local.get $tb)'
            else:
                out = [f'(local.set $tb (i64.add {base} (i64.const {imm})))']
                ea = '(local.get $tb)'
            ea2 = f'(i64.add (local.get $tb) (i64.const {1 << size}))'
            if load:
                sg = opc == 1
                # load both into temps first (rt may equal rn)
                l1 = self.mem_op(True, size, sg, False, 31, ea)[0].replace('(drop ', '(local.set $tr ')
                l2 = self.mem_op(True, size, sg, False, 31, ea2)[0].replace('(drop ', '(local.set $ta ')
                out += [l1, l2]
                if typ == 1:
                    out.append(f'(local.set $tb (i64.add (local.get $tb) (i64.const {imm})))')
                if typ in (1, 3):
                    out.append(self.W(rn, '(local.get $tb)', False, True))
                out += [self.W(rd, '(local.get $tr)', False), self.W(rt2, '(local.get $ta)', False)]
            else:
                out += self.mem_op(False, size, False, False, rd, ea) + self.mem_op(False, size, False, False, rt2, ea2)
                if typ == 1:
                    out.append(f'(local.set $tb (i64.add (local.get $tb) (i64.const {imm})))')
                if typ in (1, 3):
                    out.append(self.W(rn, '(local.get $tb)', False, True))
            return out, False

        # data processing, register
        if w & 0x1f000000 == 0x0a000000:                    # logical (shifted register)
            opc, typ, nb, amt = w >> 29 & 3, bits(w, 23, 22), w >> 21 & 1, bits(w, 15, 10)
            b = self.shifted(rm, w32, typ, amt)
            if nb:
                b = f'({t}.xor {b} ({t}.const -1))'
            op = ['and', 'or', 'xor', 'and'][opc]
            e = f'({t}.{op} {self.R(rn, w32)} {b})'
            if opc == 3:
                tr = '$ur' if w32 else '$tr'
                return [f'(local.set {tr} {e})'] + self.flags_nz(w32) + ['(local.set $c (i32.const 0))', '(local.set $v (i32.const 0))',
                                                                          self.W(rd, f'(local.get {tr})', w32)], False
            return [self.W(rd, e, w32)], False
        if w & 0x1f200000 == 0x0b000000:                    # add/sub (shifted register)
            op, s, typ, amt = w >> 30 & 1, w >> 29 & 1, bits(w, 23, 22), bits(w, 15, 10)
            return self.addsub(self.R(rn, w32), self.shifted(rm, w32, typ, amt), w32, op, s, rd, False), False
        if w & 0x1fe00000 == 0x0b200000:                    # add/sub (extended register)
            op, s, option, sh = w >> 30 & 1, w >> 29 & 1, bits(w, 15, 13), bits(w, 12, 10)
            return self.addsub(self.R(rn, w32, True), self.extended(rm, w32, option, sh), w32, op, s, rd, not s), False
        if w & 0x3fe00410 == 0x3a400000:                    # ccmp / ccmn (register or immediate)
            op = w >> 30 & 1
            b = f'({t}.const {rm})' if w >> 11 & 1 else self.R(rm, w32)
            cmp_ = self.addsub(self.R(rn, w32), b, w32, op, True, 31, False)[:-1]
            nzcv = w & 15
            other = [f'(local.set $n (i32.const {nzcv >> 3 & 1}))', f'(local.set $z (i32.const {nzcv >> 2 & 1}))',
                     f'(local.set $c (i32.const {nzcv >> 1 & 1}))', f'(local.set $v (i32.const {nzcv & 1}))']
            return [f'(if {self.cond(bits(w, 15, 12))} (then {" ".join(cmp_)}) (else {" ".join(other)}))'], False
        if w & 0x3fe00000 == 0x1a800000:                    # csel / csinc / csinv / csneg
            op, op2 = w >> 30 & 1, bits(w, 11, 10)
            b = self.R(rm, w32)
            if op == 0 and op2 == 1:
                b = f'({t}.add {b} ({t}.const 1))'
            elif op == 1 and op2 == 0:
                b = f'({t}.xor {b} ({t}.const -1))'
            elif op == 1 and op2 == 1:
                b = f'({t}.sub ({t}.const 0) {b})'
            return [self.W(rd, f'(select {self.R(rn, w32)} {b} {self.cond(bits(w, 15, 12))})', w32)], False
        if w & 0x5fe00000 == 0x1ac00000:                    # 2-source
            opcode = bits(w, 15, 10)
            a, b = self.R(rn, w32), self.R(rm, w32)
            if opcode == 2:
                return [f'(local.set {"$ub" if w32 else "$tb"} {b})',
                        self.W(rd, f'(if (result {t}) ({t}.eqz (local.get {"$ub" if w32 else "$tb"})) (then ({t}.const 0)) (else ({t}.div_u {a} (local.get {"$ub" if w32 else "$tb"}))))', w32)], False
            if opcode == 3:
                tb = '$ub' if w32 else '$tb'
                ta = '$ua' if w32 else '$ta'
                mn = -(1 << 31) if w32 else -(1 << 63)
                return [f'(local.set {ta} {a})', f'(local.set {tb} {b})',
                        self.W(rd, f'(if (result {t}) ({t}.eqz (local.get {tb})) (then ({t}.const 0)) (else (if (result {t}) (i32.and ({t}.eq (local.get {ta}) ({t}.const {mn})) ({t}.eq (local.get {tb}) ({t}.const -1))) (then (local.get {ta})) (else ({t}.div_s (local.get {ta}) (local.get {tb}))))))', w32)], False
            if 8 <= opcode <= 11:
                op = ['shl', 'shr_u', 'shr_s', 'rotr'][opcode - 8]
                return [self.W(rd, f'({t}.{op} {a} {b})', w32)], False   # Wasm masks the count like ARM
            raise Unsupported(f'2-source opcode {opcode}')
        if w & 0x7fff0000 == 0x5ac00000:   # 1-source
            opcode = bits(w, 15, 10)
            if opcode == 4:
                return [self.W(rd, f'({t}.clz {self.R(rn, w32)})', w32)], False
            raise Unsupported(f'1-source opcode {opcode}')
        if w & 0x1f000000 == 0x1b000000:                    # 3-source
            op31, o0, ra = bits(w, 23, 21), w >> 15 & 1, bits(w, 14, 10)
            if op31 == 0:
                m = f'({t}.mul {self.R(rn, w32)} {self.R(rm, w32)})'
                e = f'({t}.{"sub" if o0 else "add"} {self.R(ra, w32)} {m})'
                return [self.W(rd, e, w32)], False
            if op31 in (1, 5) and not o0:                   # smaddl / umaddl
                ext = 'i64.extend_i32_s' if op31 == 1 else 'i64.extend_i32_u'
                m = f'(i64.mul ({ext} {self.R(rn, True)}) ({ext} {self.R(rm, True)}))'
                return [self.W(rd, f'(i64.add {self.R(ra, False)} {m})', False)], False
            if op31 == 6:                                   # umulh via 32-bit halves
                return ['(local.set $ta ' + self.R(rn, False) + ')', '(local.set $tb ' + self.R(rm, False) + ')',
                        '(local.set $tr (i64.add (i64.shr_u (i64.mul (i64.and (local.get $ta) (i64.const 4294967295)) (i64.and (local.get $tb) (i64.const 4294967295))) (i64.const 32))'
                        ' (i64.mul (i64.shr_u (local.get $ta) (i64.const 32)) (i64.and (local.get $tb) (i64.const 4294967295)))))',
                        '(local.set $tc (i64.add (i64.and (local.get $tr) (i64.const 4294967295)) (i64.mul (i64.and (local.get $ta) (i64.const 4294967295)) (i64.shr_u (local.get $tb) (i64.const 32)))))',
                        self.W(rd, '(i64.add (i64.add (i64.mul (i64.shr_u (local.get $ta) (i64.const 32)) (i64.shr_u (local.get $tb) (i64.const 32))) (i64.shr_u (local.get $tr) (i64.const 32))) (i64.shr_u (local.get $tc) (i64.const 32)))', False)], False
            raise Unsupported(f'3-source op31 {op31}')
        raise Unsupported('unknown')

    def ld_st(self, size, opc, rt, ea):
        if opc == 0:
            return self.mem_op(False, size, False, False, rt, ea)
        if opc == 1:
            return self.mem_op(True, size, False, False, rt, ea)
        if size == 3:
            raise Unsupported('ld_st size 3 opc %d' % opc)
        return self.mem_op(True, size, True, opc == 3, rt, ea)

    # ---------- the module ----------
    def translate(self):
        starts = {(self.entry - self.tbase) // 4, 0}
        bodies = []
        bad = {}
        for i, w in enumerate(self.words):
            try:
                code, ends = self.insn(i, w)
            except Unsupported as e:
                bad.setdefault(str(e), []).append(f'{self.tbase + 4 * i:#x}: {w:08x}')
                code, ends = ['unreachable'], True
            bodies.append(code)
            pc = self.tbase + 4 * i
            if ends:
                starts.add(i + 1)
            if w & 0x7c000000 == 0x14000000:
                starts.add((pc + (sext(w & 0x3ffffff, 26) << 2) - self.tbase) // 4)
            elif w & 0xff000010 == 0x54000000 or w & 0x7e000000 == 0x34000000:
                starts.add((pc + (sext(bits(w, 23, 5), 19) << 2) - self.tbase) // 4)
            elif w & 0x7e000000 == 0x36000000:
                starts.add((pc + (sext(bits(w, 18, 5), 14) << 2) - self.tbase) // 4)
        if bad:
            for k, v in bad.items():
                print(f'unsupported ({k}): {len(v)}x, e.g. {", ".join(v[:4])}', file=sys.stderr)
        starts = sorted(s for s in starts if 0 <= s < self.n)
        label = {s: k for k, s in enumerate(starts)}
        nb = len(starts)
        table = ' '.join(f'$B{label[s]}' if s in label else '$Btrap' for s in range(self.n))
        out = ['(module',
               '  (import "env" "sys" (func $sys (param i64 i64 i64 i64) (result i64)))',
               f'  (import "env" "mem" (memory {self.mem_size // 65536}))',
               '  (func (export "run") (param $entry i32) (param $sp0 i64) (result i64)',
               '    ' + ' '.join(f'(local {v} i64)' for v in [f'$x{r}' for r in range(31)] + ['$sp', '$ta', '$tb', '$tc', '$tr']),
               '    ' + ' '.join(f'(local {v} i32)' for v in ['$n', '$z', '$c', '$v', '$idx', '$ua', '$ub', '$ur']),
               '    (local.set $idx (local.get $entry)) (local.set $sp (local.get $sp0))',
               '    (loop $top']
        for k in range(nb - 1, -1, -1):
            out.append(f'    (block $B{k}')
        out.append('    (block $Btrap')
        out.append(f'      (br_table {table} $Btrap (local.get $idx)))')
        out.append('    unreachable')
        for k, s in enumerate(starts):
            out.append(f'    ) ;; B{k} = {self.tbase + 4 * s:#x}')
            end = starts[k + 1] if k + 1 < nb else self.n
            for i in range(s, end):
                for line in bodies[i]:
                    out.append('      ' + line)
        out.append('    unreachable')
        out.append('    ) unreachable))')
        return '\n'.join(out), len(bad)


def main():
    elf, wat, meta, memf = sys.argv[1:5]
    entry, segs, text = load_elf(elf)
    t = T(entry, segs, text)
    text, nbad = t.translate()
    open(wat, 'w').write(text)
    open(memf, 'wb').write(t.image)
    json.dump({'base': t.base, 'entry_slot': (entry - t.tbase) // 4, 'mem_size': t.mem_size,
               'sp': t.base + t.mem_size - 64, 'insns': t.n}, open(meta, 'w'))
    print(f'translated {t.n} instructions, {nbad} unsupported kinds', file=sys.stderr)
    sys.exit(1 if nbad else 0)


if __name__ == '__main__':
    main()
