#!/usr/bin/env python3
"""Random AArch64 functions for the ahead-of-time translation's differential test (tests/aot/run.sh).

Writes fuzz.S: N functions fuzz_K(uint64_t *state, uint8_t *buf). Each loads x0-x27 (not x18)
and NZCV from state, runs a random block of the integer instructions tools/aotgen.c translates
(arithmetic with and without flags, logic, moves, bitfields, extract, conditional select and
compare, add/subtract with carry, multiply, divide, shifts, bit counting and reversal, loads and
stores of every size relative to buf, forward conditional branches), then stores the registers
and NZCV back. Memory accesses stay inside buf (4096 bytes); x28 holds buf, x18 is not touched.
The same function, run natively, interpreted and translated, must leave the same state and buf.
    python3 tests/aot/fuzzgen.py OUT.S [functions] [instructions] [seed]
"""
import random, sys

out = sys.argv[1]
NF = int(sys.argv[2]) if len(sys.argv) > 2 else 200
NI = int(sys.argv[3]) if len(sys.argv) > 3 else 48
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)

REGS = [r for r in range(28) if r != 18]
CONDS = "eq ne cs cc mi pl vs vc hi ls ge lt gt le al".split()

def x(r): return "x%d" % r
def w(r): return "w%d" % r
def reg(sz): r = rnd.choice(REGS); return (x(r) if sz == 64 else w(r)), r
def any_reg(sz): return reg(sz)[0]
def src(sz): return any_reg(sz)

def logimm(sz):
    # a valid logical immediate: a rotated run of ones in an element of 2..sz bits
    while True:
        e = rnd.choice([e for e in (2, 4, 8, 16, 32, 64) if e <= sz])
        ones = rnd.randint(1, e - 1)
        rot = rnd.randint(0, e - 1)
        pat = ((1 << ones) - 1)
        pat = ((pat >> rot) | (pat << (e - rot))) & ((1 << e) - 1)
        v = 0
        for i in range(0, sz, e): v |= pat << i
        return hex(v)

def shift(sz):
    return ", %s #%d" % (rnd.choice(["lsl", "lsr", "asr"]), rnd.randint(0, sz - 1)) if rnd.random() < 0.6 else ""

def mem():
    """A load or store relative to buf (x28)."""
    kind = rnd.random()
    ops = [("ldr", 8, 64), ("ldr", 4, 32), ("ldrh", 2, 32), ("ldrb", 1, 32), ("ldrsw", 4, 64), ("ldrsh", 2, 64),
           ("ldrsh", 2, 32), ("ldrsb", 1, 64), ("ldrsb", 1, 32), ("str", 8, 64), ("str", 4, 32), ("strh", 2, 32), ("strb", 1, 32)]
    op, size, sz = rnd.choice(ops)
    rt = any_reg(sz)
    if kind < 0.45:                                   # unsigned offset, scaled
        return ["%s %s, [x28, #%d]" % (op, rt, size * rnd.randint(0, (4000 // size) - 1))]
    if kind < 0.65:                                   # unscaled 9-bit offset
        return ["%s %s, [x28, #%d]" % (op.replace("ldr", "ldur").replace("str", "stur") if op in ("ldr", "str") else op,
                                        rt, rnd.randint(0, 255))]
    if kind < 0.85:                                   # register offset, index kept in range
        k = any_reg(64)
        lsl = {8: 3, 4: 2, 2: 1, 1: 0}[size] if rnd.random() < 0.5 else 0
        return ["and x17, %s, #%s" % (k, hex(0xff0 >> lsl)),
                "%s %s, [x28, x17%s]" % (op, rt, ", lsl #%d" % lsl if lsl else "")]
    # pairs
    p = rnd.choice(["ldp", "stp"])
    a, b = rnd.sample(REGS, 2)
    return ["%s %s, %s, [x28, #%d]" % (p, x(a), x(b), 8 * rnd.randint(0, 63))]

def one():
    sz = 64 if rnd.random() < 0.6 else 32
    d = any_reg(sz)
    r = rnd.random()
    if r < 0.10:
        op = rnd.choice(["add", "sub", "adds", "subs"])
        return ["%s %s, %s, #%d%s" % (op, d, src(sz), rnd.randint(0, 4095), ", lsl #12" if rnd.random() < 0.2 else "")]
    if r < 0.20:
        op = rnd.choice(["add", "sub", "adds", "subs"])
        return ["%s %s, %s, %s%s" % (op, d, any_reg(sz), any_reg(sz), shift(sz))]
    if r < 0.25:
        op = rnd.choice(["add", "sub", "adds", "subs"])
        ext = rnd.choice(["uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"] + (["uxtx", "sxtx"] if sz == 64 else []))
        m = w(rnd.choice(REGS)) if ext[-1] != "x" else x(rnd.choice(REGS))
        return ["%s %s, %s, %s, %s #%d" % (op, d, any_reg(sz), m, ext, rnd.randint(0, 4))]
    if r < 0.33:
        op = rnd.choice(["and", "orr", "eor", "ands"])
        return ["%s %s, %s, #%s" % (op, d, any_reg(sz), logimm(sz))]
    if r < 0.41:
        op = rnd.choice(["and", "orr", "eor", "ands", "bic", "orn", "eon", "bics"])
        sh = ", %s #%d" % (rnd.choice(["lsl", "lsr", "asr", "ror"]), rnd.randint(0, sz - 1)) if rnd.random() < 0.6 else ""
        return ["%s %s, %s, %s%s" % (op, d, any_reg(sz), any_reg(sz), sh)]
    if r < 0.47:
        op = rnd.choice(["movz", "movn", "movk"])
        return ["%s %s, #%d, lsl #%d" % (op, d, rnd.randint(0, 65535), 16 * rnd.randint(0, sz // 16 - 1))]
    if r < 0.53:
        op = rnd.choice(["sbfm", "ubfm", "bfm"])
        return ["%s %s, %s, #%d, #%d" % (op, d, any_reg(sz), rnd.randint(0, sz - 1), rnd.randint(0, sz - 1))]
    if r < 0.55:
        return ["extr %s, %s, %s, #%d" % (d, any_reg(sz), any_reg(sz), rnd.randint(0, sz - 1))]
    if r < 0.61:
        op = rnd.choice(["csel", "csinc", "csinv", "csneg"])
        return ["%s %s, %s, %s, %s" % (op, d, any_reg(sz), any_reg(sz), rnd.choice(CONDS))]
    if r < 0.65:
        op = rnd.choice(["ccmp", "ccmn"])
        b = any_reg(sz) if rnd.random() < 0.5 else "#%d" % rnd.randint(0, 31)
        return ["%s %s, %s, #%d, %s" % (op, any_reg(sz), b, rnd.randint(0, 15), rnd.choice(CONDS))]
    if r < 0.69:
        op = rnd.choice(["adc", "sbc", "adcs", "sbcs"])
        return ["%s %s, %s, %s" % (op, d, any_reg(sz), any_reg(sz))]
    if r < 0.75:
        op = rnd.choice(["madd", "msub"])
        if sz == 64 and rnd.random() < 0.4:
            op = rnd.choice(["smaddl", "umaddl", "smsubl", "umsubl"])
            return ["%s %s, %s, %s, %s" % (op, any_reg(64), any_reg(32), any_reg(32), any_reg(64))]
        return ["%s %s, %s, %s, %s" % (op, d, any_reg(sz), any_reg(sz), any_reg(sz))]
    if r < 0.77:
        return ["%s %s, %s, %s" % (rnd.choice(["smulh", "umulh"]), any_reg(64), any_reg(64), any_reg(64))]
    if r < 0.80:
        return ["%s %s, %s, %s" % (rnd.choice(["udiv", "sdiv"]), d, any_reg(sz), any_reg(sz))]
    if r < 0.83:
        return ["%s %s, %s, %s" % (rnd.choice(["lslv", "lsrv", "asrv", "rorv"]), d, any_reg(sz), any_reg(sz))]
    if r < 0.86:
        ops = ["clz", "cls", "rbit", "rev", "rev16"] + (["rev32"] if sz == 64 else [])
        return ["%s %s, %s" % (rnd.choice(ops), d, any_reg(sz))]
    return mem()

def block(n, labels):
    lines = []
    i = 0
    while i < n:
        if rnd.random() < 0.08 and i < n - 4:         # a forward branch over a few instructions
            labels[0] += 1
            L = ".L%d" % labels[0]
            kind = rnd.random()
            if kind < 0.5: lines.append("b.%s %s" % (rnd.choice(CONDS[:-1]), L))
            elif kind < 0.75: lines.append("%s %s, %s" % (rnd.choice(["cbz", "cbnz"]), any_reg(rnd.choice([32, 64])), L))
            else: lines.append("%s %s, #%d, %s" % (rnd.choice(["tbz", "tbnz"]), any_reg(64), rnd.randint(0, 63), L))
            for _ in range(rnd.randint(1, 3)):
                lines += one(); i += 1
            lines.append(L + ":")
            continue
        lines += one(); i += 1
    return lines

LOAD = ["mov x16, x0", "mov x28, x1", "ldr x17, [x16, #248]", "msr nzcv, x17"] + \
       ["ldp x%d, x%d, [x16, #%d]" % (r, r + 1, 8 * r) for r in range(0, 16, 2)] + \
       ["ldr x17, [x16, #136]"] + ["ldp x%d, x%d, [x16, #%d]" % (r, r + 1, 8 * r) for r in range(19, 27, 2)] + \
       ["ldr x27, [x16, #216]", "ldr x16, [x16, #128]"]
STORE = ["ldr x28, [sp]"] + ["stp x%d, x%d, [x28, #%d]" % (r, r + 1, 8 * r) for r in range(0, 18, 2)] + \
        ["stp x19, x20, [x28, #152]", "stp x21, x22, [x28, #168]", "stp x23, x24, [x28, #184]",
         "stp x25, x26, [x28, #200]", "str x27, [x28, #216]", "mrs x0, nzcv", "str x0, [x28, #248]"]

with open(out, "w") as f:
    f.write("// Generated by tests/aot/fuzzgen.py; do not edit.\n.text\n")
    labels = [0]
    for k in range(NF):
        f.write(".global fuzz_%d\n.type fuzz_%d, %%function\n.p2align 4\nfuzz_%d:\n" % (k, k, k))
        body = ["stp x29, x30, [sp, #-112]!", "mov x29, sp", "stp x19, x20, [sp, #16]", "stp x21, x22, [sp, #32]",
                "stp x23, x24, [sp, #48]", "stp x25, x26, [sp, #64]", "stp x27, x28, [sp, #80]", "str x0, [sp, #96]",
                "sub sp, sp, #16", "str x0, [sp]"] + LOAD + block(NI, labels) + STORE + \
               ["add sp, sp, #16", "ldp x19, x20, [sp, #16]", "ldp x21, x22, [sp, #32]", "ldp x23, x24, [sp, #48]",
                "ldp x25, x26, [sp, #64]", "ldp x27, x28, [sp, #80]", "ldp x29, x30, [sp], #112", "ret"]
        f.write("".join("    %s\n" % l if not l.endswith(":") else l + "\n" for l in body))
        f.write(".size fuzz_%d, .-fuzz_%d\n" % (k, k))
    f.write(".data\n.global fuzz_table\n.p2align 3\nfuzz_table:\n")
    for k in range(NF): f.write("    .quad fuzz_%d\n" % k)
    f.write(".global fuzz_count\nfuzz_count: .word %d\n" % NF)
    f.write('.section .note.GNU-stack,"",%progbits\n')
