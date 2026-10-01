#!/usr/bin/env python3
"""Differential test of the interpreter (core/cpu.c) against Unicorn (QEMU's A64 core).

Each round picks an encoding class, fills its free bits at random, runs that one
instruction from the same random register/memory state in both CPUs, and compares
every register, the flags, the next pc and the memory. Needs `pip install unicorn`
and build/libstep1.so (`make build/libstep1.so`).

    python3 tests/difftest.py [rounds-per-class] [seed]

Exit status is non-zero if any instruction the interpreter executes gives a
different result. Encodings the interpreter rejects (AOI_STOP_UNDEF) are counted
per class, not failed: an unimplemented instruction stops visibly, it never runs
wrong.
"""
import ctypes, os, random, sys

try:
    from unicorn import (Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION,
                         UC_ERR_FETCH_UNMAPPED)
    from unicorn import arm64_const as A
except ImportError:
    print("SKIP difftest: unicorn not installed (pip install unicorn)")
    sys.exit(0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lib = ctypes.CDLL(os.path.join(ROOT, "build", "libstep1.so"))
lib.aoi_step1.restype = ctypes.c_int
lib.aoi_step1.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64), ctypes.c_char_p,
                          ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)]
AOI_RUN, AOI_STOP_UNDEF, AOI_STOP_FAULT = 0, 2, 3

MEM, MEMSZ = 0x100000, 0x10000
CODE = 0x400000

# (name, fixed value, fixed-bit mask): the free bits are randomised.
CLASSES = [
    ("b/bl",            0x14000000, 0x7c000000),
    ("br/blr/ret",      0xd61f0000, 0xff9ffc1f),
    ("b.cond",          0x54000000, 0xff000010),
    ("cbz/cbnz",        0x34000000, 0x7e000000),
    ("tbz/tbnz",        0x36000000, 0x7e000000),
    ("mrs/msr tpidr",   0xd51bd040, 0xffdfffe0),
    ("movn/movz/movk",  0x12800000, 0x1f800000),
    ("add/sub imm",     0x11000000, 0x1f800000),
    ("logical imm",     0x12000000, 0x1f800000),
    ("add/sub shifted", 0x0b000000, 0x1f200000),
    ("add/sub extended",0x0b200000, 0x1f200000),
    ("logical shifted", 0x0a000000, 0x1f000000),
    ("adc/sbc",         0x1a000000, 0x1fe0fc00),
    ("ccmp/ccmn",       0x1a400000, 0x1fe00410),
    ("csel/csinc/inv/neg", 0x1a800000, 0x1fe00800),
    ("dp 1-source",     0x5ac00000, 0x7fff0000),
    ("dp 2-source",     0x1ac00000, 0x7fe00000),
    ("dp 3-source",     0x1b000000, 0x1f000000),
    ("bitfield",        0x13000000, 0x1f800000),
    ("extr",            0x13800000, 0x7fa00000),
    ("adr/adrp",        0x10000000, 0x1f000000),
    ("ld/st reg offset",0x38200800, 0x3b200c00),
    ("ld/st uimm12",    0x39000000, 0x3b000000),
    ("ld/st imm9",      0x38000000, 0x3b200000),
    ("ld/st pair",      0x28000000, 0x3a000000),
    ("ld literal",      0x18000000, 0x3b000000),
]


def unpredictable(insn):
    """Encodings whose result the architecture leaves CONSTRAINED UNPREDICTABLE."""
    rt, rn, rt2 = insn & 31, insn >> 5 & 31, insn >> 10 & 31
    if insn & 0x3a000000 == 0x28000000 and not insn >> 26 & 1:            # ldp/stp
        wb = insn >> 23 & 3 in (1, 3)
        if insn >> 22 & 1 and rt == rt2:
            return True
        return wb and rn != 31 and rn in (rt, rt2)
    if insn & 0x3b200000 == 0x38000000 and insn >> 10 & 1:                # ld/st imm9 writeback
        return rn != 31 and rn == rt
    return False


def rand_state(rng):
    regs = []
    for _ in range(31):
        k = rng.random()
        if k < 0.5:   # a pointer into the data region, so loads/stores land
            regs.append(MEM + 0x8000 + rng.randrange(-0x1000, 0x1000) & ~rng.choice([0, 7, 15]))
        elif k < 0.6:
            regs.append(rng.choice([0, 1, 0xffffffffffffffff, 0x7fffffffffffffff, 0x8000000000000000,
                                    0xffffffff, 0x80000000, 0x7fffffff]))
        elif k < 0.75:
            regs.append(rng.randrange(0, 256))
        else:
            regs.append(rng.getrandbits(64))
    sp = MEM + 0x8000 + rng.randrange(-0x800, 0x800) * 16
    pc = CODE + rng.randrange(0, 0x400) * 4
    nzcv = rng.getrandbits(4) << 28
    tpidr = rng.getrandbits(64)
    return regs + [sp, pc, nzcv, tpidr]


UC_X = [getattr(A, "UC_ARM64_REG_X%d" % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]


def run_unicorn(insn, st, mem):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(MEM, MEMSZ)
    uc.mem_write(MEM, bytes(mem))
    uc.mem_map(CODE, 0x1000)
    uc.mem_write(st[32], insn.to_bytes(4, "little"))
    for i in range(31):
        uc.reg_write(UC_X[i], st[i])
    uc.reg_write(A.UC_ARM64_REG_SP, st[31])
    uc.reg_write(A.UC_ARM64_REG_NZCV, st[33])
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, st[34])
    try:
        uc.emu_start(st[32], 0, count=1)
    except UcError as e:
        if e.errno in (UC_ERR_INSN_INVALID, UC_ERR_EXCEPTION):
            return "undef", None, None
        # A branch out of the code page: the instruction itself completed, only
        # Unicorn's look-ahead fetch of the target failed.
        if e.errno != UC_ERR_FETCH_UNMAPPED or uc.reg_read(A.UC_ARM64_REG_PC) == st[32]:
            return "fault", None, None
    out = [uc.reg_read(r) for r in UC_X] + [uc.reg_read(A.UC_ARM64_REG_SP), uc.reg_read(A.UC_ARM64_REG_PC),
                                            uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xf0000000,
                                            uc.reg_read(A.UC_ARM64_REG_TPIDR_EL0)]
    return "ok", out, bytes(uc.mem_read(MEM, MEMSZ))


def run_aoi(insn, st, mem):
    arr = (ctypes.c_uint64 * 35)(*st)
    buf = ctypes.create_string_buffer(bytes(mem), MEMSZ)
    fa = ctypes.c_uint64()
    r = lib.aoi_step1(insn, arr, buf, MEM, MEMSZ, ctypes.byref(fa))
    if r == AOI_STOP_UNDEF:
        return "undef", None, None
    if r == AOI_STOP_FAULT:
        return "fault", None, None
    return "ok", list(arr), buf.raw


NAMES = ["x%d" % i for i in range(31)] + ["sp", "pc", "nzcv", "tpidr"]


def diff(a, b):
    return ", ".join("%s aoi=%#x uc=%#x" % (NAMES[i], a[i], b[i]) for i in range(35) if a[i] != b[i])


def disasm(insn):
    try:
        import subprocess
        out = subprocess.run(["llvm-mc", "--disassemble", "-triple=aarch64", "-mattr=+v8.4a"],
                             input="0x%02x 0x%02x 0x%02x 0x%02x" % tuple(insn.to_bytes(4, "little")),
                             capture_output=True, text=True).stdout
        return " ".join(l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith("."))
    except OSError:
        return "?"


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    bad = 0
    for name, val, mask in CLASSES:
        n_ok = n_undef = n_extra = n_bad = 0
        shown = 0
        for _ in range(rounds):
            insn = val | (rng.getrandbits(32) & ~mask)
            if unpredictable(insn):
                continue
            st = rand_state(rng)
            mem = bytearray(rng.getrandbits(8) for _ in range(64)) * (MEMSZ // 64)
            u = run_unicorn(insn, st, mem)
            o = run_aoi(insn, st, mem)
            if u[0] == "undef":
                if o[0] == "ok":
                    n_extra += 1     # we execute an encoding the architecture rejects
                continue
            if o[0] == "undef":
                n_undef += 1
                continue
            if o[0] == "fault" and u[0] == "fault":
                n_ok += 1
                continue
            if o[0] == u[0] and o[1] == u[1] and o[2] == u[2]:
                n_ok += 1
                continue
            n_bad += 1
            if shown < 4:
                shown += 1
                if o[0] != u[0]:
                    why = "aoi %s, unicorn %s" % (o[0], u[0])
                elif o[1] != u[1]:
                    why = diff(o[1], u[1])
                else:
                    k = next(i for i in range(MEMSZ) if o[2][i] != u[2][i])
                    why = "memory differs at %#x" % (MEM + k)
                print("  MISMATCH %08x %-32s %s" % (insn, disasm(insn), why))
        bad += n_bad
        print("%-20s ok %5d  WRONG %5d  unimplemented %5d  accepts-undefined %4d"
              % (name, n_ok, n_bad, n_undef, n_extra))
    print("FAIL: %d mismatches" % bad if bad else "OK  difftest: no mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
