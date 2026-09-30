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
  (`__gmpz_get_str`) still hits a bug (below).

This is the core proof: a widely-used Android native library executes unmodified
in our interpreter.

## The open bug (start here next session)

`__gmpz_get_str` faults with a store at `pc=0x40042798`
(`strb w12, [x25, #-0x1]!`) writing to `stack_base - 1`. The fault address is
always exactly one below the mapped stack, at any stack size, which means **SP
drifts down to the bottom of the stack** over the many internal GMP calls — a
stack-pointer tracking bug in the interpreter, not a missing instruction.

Most likely suspects, in order:
1. LDP/STP **pre/post-index writeback when Rn = SP** (`core/cpu.c`, the stp/ldp
   branch) — verify the writeback updates `cpu->sp`, not `x[31]`, and that the
   imm7 offset is scaled and sign-extended.
2. ADD/SUB **extended-register** form with SP operand is **not implemented** at
   all (only immediate and shifted-register are). GMP's `sub sp, sp, xN` /
   `add sp, sp, xN` would silently corrupt or undef. Add the extended-register
   add/sub (opcode `0x0b200000` / `0x4b200000`, with the `option`/`imm3` extend).
3. Confirm every place that computes an address from Rn=31 uses `cpu->sp`.

A good way to catch it: add an optional SP trace to `aoirun`/`gmpdemo` that logs
each write to SP with the pc, and watch where SP stops being restored.

## Instruction coverage in the interpreter (core/cpu.c)

Implemented: mov(z/n/k), add/sub (imm, shifted-reg), logical (imm, shifted-reg),
adc/sbc, madd/msub/smaddl/umaddl/smulh/umulh, udiv/sdiv/lslv/lsrv/asrv/rorv,
sbfm/bfm/ubfm, rbit/rev/clz/cls, csel/csinc/csinv/csneg, branches (b/bl/br/blr/ret/
b.cond/cbz/cbnz/tbz/tbnz), adr/adrp, ldr/str (unsigned, unscaled, pre/post,
register-offset), ldp/stp, svc, mrs/msr tpidr_el0, nop/hint.

**Not yet implemented** (add as real code needs them): the whole **NEON/SIMD** and
**floating-point** register file (libqalculate's math and any memcpy that uses q
registers will need it), add/sub extended-register (see bug #2), atomics
(ldxr/stxr, the LSE ldadd/swp — bionic locks use them), and load/store of SIMD.

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
