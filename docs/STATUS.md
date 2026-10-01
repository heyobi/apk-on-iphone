# Status and handoff (2026-10-01, evening)

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

## Step 1 (in progress): unmodified Android programs with Android's own linker64

`build/aoiproc ROOT PROGRAM [args]` does what execve does for an Android binary:
maps it and its PT_INTERP (`/system/bin/linker64` → the runtime APEX) into the guest
address space, builds argv/envp/auxv, and serves the Linux syscalls on the host
(`core/proc.c`). Guest paths are confined to ROOT; absolute symlinks resolve inside it.

Running today, from the AOSP 14 GSI, unmodified (`make android-test`, 5 checks):

- `toybox echo/ls -l/cat/uname` — linker64 links libc, libcrypto, libz, liblog… then
  toybox runs: 13.7 M instructions, 0.18 s.
- `/system/bin/sh` (mksh) runs scripts: `echo $((6*7))` → 42 (2.1 M instructions, 0.05 s).
- `linkerconfig` runs to completion and writes `/linkerconfig/ld.config.txt` — but in
  **legacy** form (see next steps).

Get a root in a fresh session: `tools/fetch-android.sh ~/aroot` (≈800 MB download,
sha256-pinned), then `AOI_ANDROID_ROOT=~/aroot make android-test`. `aoiproc -t` logs
every syscall; on a stop it names the library and offset of pc and lr.

What it took beyond the loader and syscalls: user-readable system registers
(CTR_EL0, DCZID_EL0, CNTVCT/CNTFRQ, FPCR/FPSR), CRC32/CRC32C, `pmull`, `uminp`
& co., `ldapr`, a fix to LSE `swp` (it was unreachable), Top Byte Ignore for data
accesses (Android tags heap pointers), `MREMAP_FIXED` (the linker builds its CFI
shadow with it — without it every cross-library indirect call hit a CFI trap), and
a 64 GiB guest space: scudo reserves 8+ GiB at start. Mappings without a hint go
above 4 GiB, leaving the low 4 GiB to ART's heap. vm.c now maps a range with one
host call instead of one per page (scudo's reservation was 500k mmaps, 0.9 s).

**Next, in order:**
1. **System properties.** bionic reads them from `/dev/__properties__` (built by init
   from `build.prop` + `property_contexts`). Without them linkerconfig picks the
   legacy layout, so APEX namespaces (`libnativehelper.so` for dalvikvm64) are not
   visible. Write the property area (bionic's `prop_area` trie + `property_info`)
   from the root's build.prop files, plus `/apex/apex-info-list.xml`.
2. Then `dalvikvm64 -showversion`, then a hello-world dex.
3. Processes and threads (`clone`, `pipe2`, `wait4`, signals): mksh pipelines and
   ART's own threads need them (roadmap step 2).

**Address space on the iPhone (risk found and handled first).** App 0.6's probe on an
iPhone 16 Pro, iOS 27.0.1: the kernel reports a 450 GiB user range but grants at most
**6 GiB of contiguous reservation** (8 GiB and up: ENOMEM), without the
extended-virtual-addressing entitlement. A flat 64 GiB guest space was therefore
impossible there. `core/vm.c` is now **sparse**: per-4 KB guest protections as before,
host memory in 2 MiB chunks that exist only while a page in them is accessible. A
PROT_NONE reservation costs nothing; toybox/mksh/linkerconfig run with 60-76 MiB of
chunks (was 10.5 GiB: sys_mmap mapped every anonymous request RW before applying its
protection, which backed all of scudo's 8 GiB reservation). Cost: ~5 % (toybox echo
0.19 → 0.20 s). **Confirmed on the device** (app 0.7): the sparse 64 GiB space with an
8 GiB scudo reservation runs with 3 chunks — "fits on this device".

**Step 1 runs on the iPhone** (app 0.8, iPhone 16 Pro, iOS 27.0.1, no JIT, no
entitlement): Android's own linker64 loads toybox and mksh from the 9 MB guest root
bundled in the app (`ios/android-files.txt`, built by the workflow with
`tools/mini-root.sh`) and core/proc.c serves their syscalls on Darwin:

| program | result | instructions | time | host chunks |
|---|---|---:|---:|---:|
| `toybox echo` | correct, exit 0 | 13.6 M | 0.175 s (78 M/s) | 62 MiB |
| `mksh -c 'echo $((6*7)); …'` | "42", "7 harf", exit 0 | 0.94 M | 0.012 s | 60 MiB |
| `toybox ls /system/lib64` | all 14 entries, exit 0 | 13.8 M | 0.142 s (97 M/s) | 66 MiB |

**Biggest open risk now: speed of interpreted ART.** Starting ART and running even a
hello-world dex is likely billions of guest instructions; at ~80 M/s that is tens of
seconds per app start. Measure it as soon as dalvikvm runs on the host (next steps),
before building framework pieces on top; the answer decides how early the WebKit
(Wasm) JIT is needed.

Known simplifications: one thread, `futex` never blocks; signals are recorded but
never delivered; `socket` is ENOSYS (logd is absent, so logs go nowhere); file
mappings are private copies; uid 0.

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
- `core/linux.c` — minimal syscalls for `aoirun` (write/writev/exit).
- `core/vm.c` — sparse guest address space (2 MiB host chunks on demand, per-page R/W/X).
- `core/proc.c` — a Linux process: execve-style loader (PT_INTERP, auxv) and the
  syscall layer for unmodified Android programs (`tools/aoiproc.c`).
- `tools/fetch-android.sh`, `tools/android-root.sh` — the AOSP 14 guest root.
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
