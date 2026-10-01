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

## Step 3 reached on the host: ART runs a dex — and the first speed measurement

`make android-test` (with `AOI_ANDROID_ROOT`) now also does the boot-time setup and
runs ART. `tools/android-setup.sh` does what init/apexd would: the property area
(`tools/mkprops.py`, bionic's pre-split `prop_area` format, long values included),
then Android's own `linkerconfig` (223 KB config, 17 APEX namespaces) and
`derive_classpath` (BOOTCLASSPATH etc.), both in the interpreter. Then:

    aoiproc -e BOOTCLASSPATH=… -e DEX2OATBOOTCLASSPATH=… ROOT \
        /apex/com.android.art/bin/dalvikvm64 -Xverify:none -cp /data/local/tmp/hello.dex Hello
    Merhaba from ART

The dex is written by `tests/mkdex.py` (no d8 here: dl.google.com is blocked) and
checked by Android's own `dexdump` in the interpreter.

**Speed (the open risk from before), host x86:**

| run | guest instructions | time |
|---|---:|---:|
| ART hello, boot dex files verified | 1,520 M | 25.4 s |
| ART hello, `-Xverify:none` | **248 M** | **4.6 s** (≈ 3 s on the iPhone at 80 M/s) |

`aoiproc -p` (sampling profiler) showed 73 % of the first run in libdexfile's
`DexFileVerifier` re-checking ~40 boot jars; boot dex files are system files, and a
device skips that through the boot image's vdex, so `-Xverify:none` is right here.
Of the 248 M, 55 % is linker64 (relocating/linking libart & co.), 12 % libart.

ART currently runs **imageless**: the GSI's boot image was built for the CMC GC (no
read barriers), and ART only picks CMC when the kernel offers userfaultfd with SIGBUS
support, so it falls back to the CC GC and rejects the image. Using the image (AOT
code for all boot classes) needs: guest **signal delivery** (also needed anyway: ART's
implicit null checks are SIGSEGV), then userfaultfd in SIGBUS mode.

What ART needed from core/proc.c on the way: **green threads** (clone for threads,
a scheduler with 100k-instruction slices, real futex wait/wake/requeue with
timeouts, CLONE_CHILD_CLEARTID), a logd emulation (liblog's socket → "P/tag: msg"
on stderr), a synthetic /proc (self/stat incl. startstack, maps with [stack],
status, cmdline, meminfo, cpuinfo), membarrier, rt_sigtimedwait (sleeps until a
tgkill delivers the awaited signal), set/getpriority; in the CPU: dmb/dsb/isb,
clrex, dc/ic (dc zva zeroes), vector clz/cls.

**ART runs on the iPhone** (app 0.9, iPhone 16 Pro, iOS 27.0.1, no JIT, no
entitlement): `dalvikvm64` from AOSP 14 prints "Merhaba from ART" from a dex —
**85.9 M guest instructions, 1.14 s (75 M/s), 150 MiB of host chunks**. The same
log shows ART's own lines (CC GC, imageless start, nativeloader namespaces, ICU).

**ART on the phone, trimmed (app 0.9).** For the device, two measured cuts: the six
core boot jars are enough (`BOOTCLASSPATH`=core-oj, core-libart, okhttp,
bouncycastle, apache-xml, core-icu4j, with `-Ximage:` given so ART does not ask for a
mainline jar), and `tools/android-setup.sh` marks every public NDK library
`nopreload` in /system/etc/public.libraries.txt, so libhwui/libgui/libpdfium… are no
longer linked at start. ART hello: **85 M instructions, 1.7 s** on the host (from 248 M).
The bundled root (`ios/android-files.txt`, from `aoiproc -t` traces) is 73 MB; the
workflow builds aoiproc on the macOS runner and runs android-setup.sh there.

**Guest signals (done):** core/proc.c builds Linux arm64 signal frames (siginfo,
ucontext with x0-x30/sp/pc/pstate, fpsimd record, frame record), honours altstacks,
SA_RESTORER/NODEFER/RESETHAND and per-thread masks, implements rt_sigreturn, turns
CPU faults into SIGSEGV (MAPERR/ACCERR, si_addr) when the guest has a handler,
delivers tgkill/kill to any thread (a sleeping or futex-waiting target returns
EINTR), and applies default actions. `tests/signals.c` (in `make test`) checks the
fault → handler → edited context → sigreturn path, self-tgkill and masking.

**Boot image under CMC: works (opt-in, `AOI_UFFD=1`).** With userfaultfd offered
(SIGBUS mode in the UFFDIO_API handshake; nothing else of userfaultfd yet) ART picks the
CMC GC, maps all 15 boot image components at 0x70000000 with their oat files, and runs
the hello dex: **222 M instructions, 3.7 s host, 150 MiB of chunks** (full BCP; imageless
with the full BCP was 248 M). `make android-test` checks both paths. Two emulation bugs
were in the way, both found with the debug build's store watch (`AOI_WATCH`) and pc ring:

- **madvise(MADV_REMOVE) was a no-op.** CMC's linear-alloc arena pool frees arenas with
  MADV_REMOVE and relies on reading zeros afterwards; we left the old bytes, so a reused
  arena handed DexCache type slots that still held `String` references from the
  boot-framework image (`Class::FindClassMethod` got a String as `this`). Now zeroed like
  MADV_DONTNEED.
- **No vDSO sigreturn.** bionic on arm64 installs handlers *without* SA_RESTORER: the
  kernel returns through the vDSO's `__kernel_rt_sigreturn`. Every handler that
  returned normally (ART's implicit null checks → NullPointerException) jumped to 0. A
  one-page `[vdso]` with `mov x8, #139; svc #0` now plays that role.

**CMC compaction works too.** `tests/mkdex.py OUT 0 20000` writes a dex that allocates
20 MB of `byte[1024]` and calls `Runtime.getRuntime().gc()`; under CMC ART logs
"Background concurrent mark compact GC freed 20MB" and the explicit GC, and exits 0.
What it took (core/proc.c, core/vm.c): a per-page **missing** bit in the VM (any access
faults); `mremap(MREMAP_DONTUNMAP)` moves the bytes (whole aligned 2 MiB chunks change
owner: the 256 MB moving space and the 1 GB linear-alloc space are not copied) and
leaves the source missing; a missing page inside a range registered with
UFFDIO_REGISTER raises the guest **SIGBUS** (BUS_ADRERR) that ART's handler answers with
UFFDIO_COPY/ZEROPAGE; outside one it is zero-filled on touch, as the kernel would;
madvise(DONTNEED/REMOVE) on a registered page makes it missing again; UNREGISTER and
WAKE. Both GCs are in `make android-test` (CC by default, CMC with `AOI_UFFD=1`).

**Memory fix that matters on the phone.** madvise(DONTNEED) zeroed page by page, and a
4 KB page is smaller than a host page (16 KB on iOS), so every byte was written, and so
allocated: the GC's 2.7 GB of madvise turned into 739 k host page faults and 1.5 GB RSS.
Now runs of pages are released as whole host pages (`chunk_clear`). GC dex, host:

| GC | before | after |
|---|---|---|
| CC (default) | 4.2 s, 722 MB RSS | **2.0 s, 155 MB** |
| CMC + boot image | 18.5 s, 1.5 GB RSS | **4.0 s, 198 MB** |

File mappings are no longer copies: where the guest address and the file offset agree
modulo the host page (16 KB covers iOS and Linux), whole host pages inside the file are
the file, mapped MAP_PRIVATE into the chunk (demand-paged, copy-on-write); edges and
anything past EOF are still read (a host mapping past EOF would SIGBUS the emulator).
CC GC dex: 155 → 120 MB RSS.

Where the 222 M go (`aoiproc -p`): 30 % liblz4 (decompressing the images), 29 % libart,
20 % libartbase, 10 % linker64. Storing the images uncompressed in the root would remove
the LZ4 third. The image only pays off with the full BCP (framework classes for real
apps); for the bare hello the six core jars imageless stay cheapest (81 M), which is
what the phone runs. userfaultfd stays opt-in for now: the phone ships only the core
jars (no boot image to gain), and CMC on the device is untested.
`aoiproc -t` prints a frame-pointer backtrace on each guest SIGSEGV and every
sigaction a guest installs.

**Next, in order:**
1. Uncompressed boot images in the root (LZ4 is 30 % of the image start), then decide
   whether the phone gets the full BCP + boot image + CMC (bundle size, device test).
2. fork/execve/pipe2/wait4 for mksh pipelines (roadmap step 2).

Known simplifications: green threads (one host thread runs all guest threads);
signals are delivered at time-slice and syscall boundaries; sockets other
than logd refuse to connect; file mappings are private copies (MAP_SHARED of a file
does not write back); uid 0; no fork/execve yet.

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

It now runs Unicorn's MAX CPU model, so exclusives, acquire/release, cas/casp,
ldxp/stxp and LSE atomics are fuzzed too (classes `ld/st excl/acq/cas`, `LSE atomics`).
Two Unicorn bugs are excluded by hand (`unicorn_wrong`: sub-64-bit ldsmax/ldsmin compare
unsigned there), a failing stxr to a bad address may fault or not (implementation
defined), and the random data page is mapped non-executable: a branch into it once made
QEMU translate random FP16 words and abort the process.

Known gaps it reports without failing: FPSR's cumulative exception bits are not
modelled (column `fpsr`), and in several integer and SIMD classes we still execute
unallocated encodings instead of stopping (`accepts-undefined`; for the exclusive and
atomic classes these are misaligned addresses, which real hardware faults and we allow). Neither affects
compiler-generated code, but both are cheap to close later.

## Instruction coverage

Across all 9 Qalculate libraries (1.47 M instructions) there are 213 distinct mnemonics.
`tools/isawords.py *.so > words.txt` lists every distinct encoding; `build/isacheck`
runs each once on our CPU and once on Unicorn from 4 random register states and prints
per mnemonic what is missing or wrong. For a new APK: run those two, fix the table.

- `core/cpu.c` — integer base set, all integer and SIMD&FP load/store forms, atomics
  (ldar/stlr, ldxr/stxr and ldxp/stxp with a monitor, cas/casp, LSE
  ldadd/ldclr/ldeor/ldset/max/min/swp), FP16 `fmov` vector immediate.
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
