# Status and handoff (2026-10-01, updated)

## What runs today

A **no-JIT AArch64 interpreter** loads real Android `.so` files from an APK, links
them, and runs their code on any host — no iPhone, no JIT.

- `make test` — freestanding aarch64-linux ELF at -O0 and -O1, plus tpidr_el0. Green.
- `./build/gmpdemo <libgmp.so> N` — the **real `libgmp.so` from the Qalculate APK**
  computes N! with GMP's own code, including its NEON paths. Checked against Python:
  5000! (0.09 s), 20000! (0.6 s), **100000! — 456,574 digits, 327 M guest instructions,
  6.8 s** (≈ 48 M instructions/s interpreted).
- `./build/isacheck words.txt` — **all 284,953 distinct instruction words in Qalculate's
  9 libraries: 0 missing, 0 wrong** against Unicorn (system/hint/exception encodings and
  words that fault on every random state are skipped).

## The reference-CPU oracle (use it for every new instruction)

`make build/gmpdemo-check` (needs `pip install unicorn`) runs each guest instruction a
second time in **Unicorn** (QEMU's AArch64 core) on the same memory and stops at the
first instruction where registers, flags, V registers, pc or stored bytes differ:

```
[oracle] MISMATCH after 184 instructions at pc=0x40039e34 insn=0x9bc37c8a
[oracle]   x10    ours=0xfffffe0701418e98 ref=0x00000537bad9b3e9
```

The old "get_str SP drift" was not an SP bug. The oracle found, in order:
`umulh`/`smulh` swapped (so the factorial had always been wrong), `ubfm`/`sbfm`/`bfm`
computed wrong for the LSL/BFI forms, and missing SIMD `ldp q`. Also fixed while there:
csinv/csneg/csetm/cneg, rev16/rev32, SXTW register offsets, ldrsb/ldrsh to W, prfm
treated as a load, ldpsw, and SIMD loads decoded as integer loads.

Workflow for the next instruction: run the `-check` binary, it names the missing
encoding (`llvm-objdump` or capstone disassembles it), implement it, rerun until identical.

## Random-encoding fuzzer (`tests/difftest.py`, merged from main)

`isacheck` proves every word an app actually contains; `make test` / `make difftest`
complement it with random encodings from ~50 instruction classes, run from random
register and memory state against Unicorn. It found bugs that neither app happens to
contain, now fixed: `ret x0` returned to x30, `sdiv INT_MIN, -1` killed the host process
with SIGFPE, and `fmsub`/`fnmadd`/`fnmsub`/`fmls` returned a NaN with the wrong sign
(Arm negates before NaN propagation; inf*0 with a quiet-NaN addend is the default NaN).

Known gaps it reports without failing: FPSR's cumulative exception bits are not
modelled (column `fpsr`), and in several integer and SIMD classes we still execute
unallocated encodings instead of stopping (`accepts-undefined`). Neither affects
compiler-generated code, but both are cheap to close later.

## Instruction coverage

Across all 9 Qalculate libraries (1.47 M instructions) there are 213 distinct mnemonics.
`tools/isawords.py *.so > words.txt` lists every distinct encoding; `build/isacheck`
runs each once on our CPU and once on Unicorn from 4 random register states and prints
per mnemonic what is missing or wrong. For a new APK: run those two, fix the table.

- `core/cpu.c` — integer base set, all integer and SIMD&FP load/store forms, atomics
  (ldar/stlr, ldxr/stxr with a monitor, cas, LSE ldadd/ldclr/ldeor/ldset/max/min/swp).
- `core/simd.c` — NEON integer (three-same, two-reg misc, across-lanes, shifts, widen/
  narrow, copy/dup/ins/umov, zip/uzp/trn, ext, tbl, ld1 lane/ld1r) and floating point
  (scalar arithmetic/fma/compare/convert/round, vector fadd…fdiv/fmla/compares/converts,
  by-element fmul/fmla) with ARM NaN rules.

Not covered yet: half precision, saturating and crypto NEON ops, ld2-4 single-lane —
none occur in Qalculate; other apps will tell (isacheck).

## Architecture recap (what each file is)

- `core/elf.c` — ELF64/AArch64 reader.
- `core/scan.c` + `tools/apkscan` — find the sites an .so can't run unmodified on
  iOS (svc, tpidr_el0, x18 shadow stack). `tools/apkscan.py` unpacks an APK.
- `core/cpu.c` — the interpreter (no-JIT execution backend).
- `core/dl.c` — dynamic linker: maps .so files, applies RELA relocations, binds
  imports to loaded libs / host shim / a named-stop slot.
- `core/bionic.c` — host implementations of the libc functions .so files import
  (malloc/memcpy/strlen/localeconv/stdio pointers, …). Grown as needed.
- `core/linux.c` — Linux syscall layer (write/writev/exit so far).
- `core/load.c` — static-ELF loader + initial stack (for the `aoirun` path).
- `tools/gmpdemo.c` — the end-to-end demo: APK library → linked → GMP computes.

## The bigger roadmap (unchanged)

1. **(done)** interpret a real native lib from an APK. ← we are here, minus get_str.
2. **(CPU done for this app)** NEON/FP + atomics. Next: bionic shim + libc++ so libqalculate's math runs → compute "2+2" and big
   expressions through Qalculate's real engine, headless.
3. bionic proper + dynamic linker for a full lib set; then ART (the dex runtime)
   for the Java/Kotlin half; then a UI surface (Compose → Metal).
4. On-device: reuse Madeira's dual-mapped JIT (StikDebug) for a fast backend;
   keep the interpreter as the App-Store-safe path (compile it to Wasm).

## Distribution note

Sideload only (SideStore/AltStore) with JIT via StikDebug. App Store allows only
WebKit's JIT, so an App Store build must run everything as Wasm in WKWebView —
which is exactly why the interpreter (backend #1) matters: it is that path's seed.
