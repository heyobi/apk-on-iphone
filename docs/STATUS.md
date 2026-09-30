# Status and handoff (2026-10-01, updated)

## What runs today

A **no-JIT AArch64 interpreter** loads real Android `.so` files from an APK, links
them, and runs their code on any host — no iPhone, no JIT.

- `make test` — freestanding aarch64-linux ELF at -O0 and -O1, plus tpidr_el0. Green.
- `make build/gmpdemo && ./build/gmpdemo <libgmp.so> 50` — the **real `libgmp.so` from
  the Qalculate APK** computes `50! = 30414093201713378043612608166064768844377641568960512000000000000`
  (checked against Python for n = 10, 50, 100). n ≥ ~300 stops at the next unimplemented
  NEON instruction (`cnt v6.16b`, pc 0x40043764 in libgmp).

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

## Instruction coverage

Across **all 9 Qalculate libraries (1.47 M instructions) there are only 213 distinct
mnemonics**; ~59 are FP/SIMD, 15 atomics. So the CPU layer is a finite list, not an
open-ended one — and it is shared by every app.

Implemented now: integer base set, extr, ccmp/ccmn, add/sub extended (SP), bitfield,
all integer and SIMD&FP ld/st forms (incl. literal, ld1-4/st1-4 multiple), movi/mvni/
orr/bic vector immediate, V register file.

Missing (by frequency in these libs): NEON arithmetic (cnt, addv, ext, ushll/sshll,
dup, umov, cmhi, tbl, …), scalar FP (fmov, fcmp, fcsel, scvtf, fmul, fdiv, fcvt*),
atomics (ldar/stlr, ldxr/stxr, ldadd, cas, swp).

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
2. NEON/FP + atomics, so libqalculate's math runs → compute "2+2" and big
   expressions through Qalculate's real engine, headless.
3. bionic proper + dynamic linker for a full lib set; then ART (the dex runtime)
   for the Java/Kotlin half; then a UI surface (Compose → Metal).
4. On-device: reuse Madeira's dual-mapped JIT (StikDebug) for a fast backend;
   keep the interpreter as the App-Store-safe path (compile it to Wasm).

## Distribution note

Sideload only (SideStore/AltStore) with JIT via StikDebug. App Store allows only
WebKit's JIT, so an App Store build must run everything as Wasm in WKWebView —
which is exactly why the interpreter (backend #1) matters: it is that path's seed.
