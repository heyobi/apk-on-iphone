# Status and handoff (2026-10-01)

## What runs today

A **no-JIT AArch64 interpreter** loads real Android `.so` files from an APK, links
them, and runs their code on any host — no iPhone, no JIT.

- `make test` — the interpreter runs a freestanding aarch64-linux ELF correctly at
  -O0 and -O1, plus a tpidr_el0 round-trip. All green.
- `make build/gmpdemo && ./build/gmpdemo <libgmp.so> 50` — loads the **real
  `libgmp.so` from the Qalculate APK** (arm64-v8a), binds its 34 libc imports to a
  small host shim, and computes **50!** with GMP's own code:
  `__gmpz_init` and `__gmpz_fac_ui` complete (the factorial itself runs — millions
  of real GMP instructions through our CPU). Only the final decimal formatting
  (`__gmpz_get_str`) hit a fault, very likely caused by the decode bugs fixed below
  (not yet re-run: see "Start here").

This is the core proof: a widely-used Android native library executes unmodified
in our interpreter.

## The interpreter is now checked against a reference CPU

`tests/difftest.py` runs random encodings from every instruction class the
interpreter claims, from random register and memory state, through both
`core/cpu.c` and Unicorn (QEMU's A64 core), and compares all registers, flags,
the next pc and memory. `make test` runs a short pass (it prints SKIP without
`pip install unicorn`); `make difftest PYTHON=...` runs 5000 per class.

Its first run found real decode bugs that explain the old `__gmpz_get_str`
fault (an SP/pointer running off the stack) far better than an SP-writeback
bug. All of them are fixed now, and ~130k random instructions match:

- `smulh`/`umulh` were **swapped**. GMP's multiply and divide-by-invariant code
  depends on `umulh`, so every big-number result after the first carry was wrong.
- `ubfm`/`sbfm`/`bfm` were wrong whenever imms < immr: `lsl #n`, `ubfiz`, `sbfiz`
  and `bfi` all produced garbage. They are rewritten from the ARM ARM pseudocode
  (wmask/tmask).
- `csinv`/`csneg` (and `cinv`, `cneg`, `csetm`) computed `csel`/`csinc`.
- `ret xN` with N = 0 returned to x30.
- `[Xn, Wm, sxtw]` was treated as `uxtw`.
- 32-bit `ands`/`bics`/`tst` set N from bit 63, so it was always clear.
- `ldrsb`/`ldrsh` into W did not sign-extend, `ldpsw` did not sign-extend, `prfm`
  wrote a register or faulted, and the 64-bit `rev32` reversed all 8 bytes.
- SIMD&FP loads and stores (`ldr q0`, `str d1`, ...) silently ran as integer
  accesses. They now stop with AOI_STOP_UNDEF until the V register file exists.
- Newly implemented: add/sub **extended register** (`add sp, sp, x8` etc.),
  `ccmp`/`ccmn`, `extr`/`ror`, `ldr` literal, `rev16`. Unallocated encodings
  in the implemented classes are now rejected rather than executed.

## Start here next session

1. **Re-run `gmpdemo` on the Qalculate `libgmp.so`.** The APK was a chat upload
   and is not in the repo (F-Droid downloads are blocked from the container).
   Expect `50!` to print correctly now; if it still fails, the fault is new
   information, not the old bug.
2. Extend `difftest.py` CLASSES as each new class is implemented. Any class the
   interpreter runs must show `WRONG 0`.
3. Then roadmap item 2: the SIMD&FP register file (start with q/d/s loads and
   stores, `fmov`, `dup`/`movi`, since compilers use q registers for memcpy-like
   copies), and atomics (`ldxr`/`stxr`, LSE `ldadd`/`swp`/`cas`).

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
