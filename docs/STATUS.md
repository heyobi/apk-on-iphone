# Status and handoff (2026-10-01)

## What runs today

A **no-JIT AArch64 interpreter** loads real Android `.so` files from an APK, links
them, and runs their code on any host — no iPhone, no JIT.

- `make test` — the interpreter runs a freestanding aarch64-linux ELF correctly at
  -O0 and -O1, a tpidr_el0 round-trip, and the instruction-level differential test
  against Unicorn (below). All green.
- `make build/gmpdemo && ./build/gmpdemo <libgmp.so> N` — loads the **real
  `libgmp.so` from the Qalculate APK** (arm64-v8a), binds its libc imports to a
  small host shim, and computes **N!** end to end with GMP's own code, including its
  NEON paths (`mpn_popcount`, q-register copies). Checked against Python for N = 0,
  1, 20, 21, 100, 1000, 1626, 3000, 10000 and 30000. 30000! (121,288 digits) takes
  53 million guest instructions.

This is the core proof: a widely-used Android native library executes unmodified
in our interpreter and produces correct results.

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

### Second round: NEON and FP (`core/simd.c`)

The 32x128-bit V register file, all SIMD&FP loads/stores (single, pair,
literal, `ld1`-`ld4`/`st1`-`st4` multiple and single-lane, `ld1r`), and the
Advanced SIMD integer groups: modified immediate (`movi`/`mvni`/`orr`/`bic`/
vector `fmov`), three-same (logic, add/sub, compares, min/max, mul/mla,
pairwise), two-reg misc (`rev*`, `cnt`, `not`, `rbit`, `[su]addlp`,
`[su]adalp`, `clz`/`cls`, compares with zero, `abs`/`neg`, `xtn`, vector
`fabs`/`fneg`), shifts by immediate (`[su]shr`, `[su]sra`, rounding forms,
`shl`, `sli`, `sri`, `shrn`, `[su]shll`), three-different (`[su]addl/w`,
`[su]subl/w`, `[su]mull`, `[su]mlal/sl`, `[su]abdl/abal`, `addhn`/`subhn`),
across lanes (`addv`, `[su]addlv`, `[su]maxv`/`minv`), copy (`dup`, `ins`,
`umov`, `smov`), `zip`/`uzp`/`trn` and `ext`. Scalar FP (single and double):
`fmov` (all forms), `fadd`/`fsub`/`fmul`/`fdiv`/`fnmul`, `fmax`/`fmin`(`nm`),
`fmadd` family, `fsqrt`, `fabs`/`fneg`, `fcvt` s<->d, `frint*`, `fcmp(e)`,
`fccmp(e)`, `fcsel`, `scvtf`/`ucvtf`, `fcvt[nzpma][su]`.

FP uses host IEEE arithmetic for rounding but AArch64 rules where hosts
differ (NaN propagation order, positive default NaN, signed-zero max/min,
saturating conversions, tininess before rounding), and keeps FPSR's
exception bits. The fuzzer also caught `sdiv INT_MIN, -1` trapping the host
(SIGFPE); it now wraps as on Arm.

The last `gmpdemo` failure (from 1626! up) was the demo's own fixed
4096-byte output buffer, not the interpreter; it is now sized with GMP's
`mpz_sizeinbase`.

## Start here next session

1. **libqalculate**: load the Qalculate set (`libc++_shared`, `libgmp`,
   `libmpfr`, `libxml2`, `libiconv`, `libqalculate`) and evaluate "2+2"
   headless. Needs C++ runtime pieces in the shim (`__cxa_*`, `operator new`,
   locale/pthread stubs) and whatever new instructions show up; add every
   new class to `tests/difftest.py`.
2. Still missing in the CPU: vector FP arithmetic (`fadd v.2d`...), FP16,
   saturating integer SIMD (`sqadd`...), `pmull`, crc32, atomics
   (`ldxr`/`stxr`, LSE `ldadd`/`swp`/`cas`), which bionic locks need.
3. Then the libGDX path (cube.run, see MEASUREMENTS.md): `libgdx.so` is
   small and its imports are plain libc/libm, but the game itself is
   Java, so it needs ART first.

## Architecture recap (what each file is)

- `core/elf.c` — ELF64/AArch64 reader.
- `core/scan.c` + `tools/apkscan` — find the sites an .so can't run unmodified on
  iOS (svc, tpidr_el0, x18 shadow stack). `tools/apkscan.py` unpacks an APK.
- `core/cpu.c` — the interpreter (no-JIT execution backend).
- `core/dl.c` — dynamic linker: maps .so files, applies RELA relocations, binds
  imports to loaded libs / host shim / a named-stop slot.
- `core/bionic.c` — host implementations of the libc functions .so files import
  (malloc/memcpy/strlen/localeconv/stdio pointers, …). Grown as needed.
- `core/simd.c` — Advanced SIMD (NEON) and scalar FP for the interpreter.
- `core/linux.c` — Linux syscall layer (write/writev/exit so far).
- `core/load.c` — static-ELF loader + initial stack (for the `aoirun` path).
- `tools/gmpdemo.c` — the end-to-end demo: APK library → linked → GMP computes.

## The bigger roadmap (unchanged)

1. **(done)** interpret a real native lib from an APK: GMP computes 30000! correctly.
2. **(NEON/FP base done; atomics next)** NEON/FP + atomics, so libqalculate's math runs → compute "2+2" and big
   expressions through Qalculate's real engine, headless.
3. bionic proper + dynamic linker for a full lib set; then ART (the dex runtime)
   for the Java/Kotlin half; then a UI surface (Compose → Metal).
4. On-device: reuse Madeira's dual-mapped JIT (StikDebug) for a fast backend;
   keep the interpreter as the App-Store-safe path (compile it to Wasm).

## Distribution note

Sideload only (SideStore/AltStore) with JIT via StikDebug. App Store allows only
WebKit's JIT, so an App Store build must run everything as Wasm in WKWebView —
which is exactly why the interpreter (backend #1) matters: it is that path's seed.
