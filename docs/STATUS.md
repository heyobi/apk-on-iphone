# Status and handoff (2026-10-04)

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

**Uncompressed boot images.** `tools/uncompress-art.py` (run by android-root.sh) rewrites
each `.art` as its in-memory layout (header with no blocks, objects, then the bitmap at
the next page): ART maps the file instead of decompressing it. Hello with the boot image:
**222 M → 156 M instructions, 3.8 → 2.6 s** (the images grow from 8 to 29 MB on disk).
What is left (`aoiproc -p`, sampled): 28 % libartbase, nearly all one loop: libziparchive
zero-fills its 64 KB end-of-central-directory buffer byte by byte (~330 k instructions per
zip open, and ART opens the BCP jars ~120 times); 7 % BoringSSL's FIPS integrity test
(an HMAC of libcrypto's own text at load). Both are guest code: only a faster
interpreter makes them cheaper. The image only pays off with the full BCP (framework classes for real
apps); for the bare hello the six core jars imageless stay cheapest (81 M), which is
what the phone runs. userfaultfd stays opt-in for now: the phone ships only the core
jars (no boot image to gain), and CMC on the device is untested.
`aoiproc -t` prints a frame-pointer backtrace on each guest SIGSEGV and every
sigaction a guest installs.

**On the phone, app 0.10** (iPhone 16 Pro, iOS 27.0.1): toybox, mksh, `ls`, and ART
hello in **1.07 s** (85 M instructions, 80 M/s; 1.14 s on 0.9). App 0.11 adds an ART GC
step (`gc.dex`: 20 MB of garbage, `Runtime.gc()`, CC GC) and logs the process's
phys_footprint now and at its peak after every Android run. **On the phone (0.11):**
toybox/mksh 27-29 MB, ART hello 1.06 s with a **121 MB peak** (limit 3540 MB). **App 0.12:**
the ART GC step runs on the phone: 20 MB of garbage, CC GC (background + explicit),
**1.12 s, 130 MB peak footprint**. (0.11 lacked gc.dex: the workflow generated the dex
files inside its cached root step; now they get their own step.) Its one warning,
`mincore` ENOSYS, is gone: mapped pages report resident, missing ones not.

**Toward the app process: `app_process64` (frameworks/base/cmds/app_process).** It is
how Android runs framework-using Java commands (`am`, `pm`): AndroidRuntime starts ART
with the full BCP and boot image, registers the framework's JNI from
libandroid_runtime, then RuntimeInit runs a class's main. With
`CLASSPATH=/data/local/tmp/hello.dex app_process64 /system/bin Hello` (and
`AOI_UFFD=1`) it gets through AndroidRuntime and ART's start (401 M instructions) and
stops where expected: ProcessState aborts because **/dev/binder** cannot be opened.
Fixed on the way: the CPU ABI properties a GSI lacks (vendor partition) are now
defaults in `tools/mkprops.py`; `pipe2` exists, with host ends non-blocking and a
blocking guest read/write turned into a 1 ms sleep plus a re-run of the syscall
(`AOI_STOP_RESTART`, so other green threads can write meanwhile; `tests/pipes.c` in
`make test`). memfd_create stays ENOSYS on purpose: ART's JIT would then dual-map its
code cache through MAP_SHARED, and our file mappings are private copies.

**app_process64 runs a dex through the framework runtime** (`make android-test`):
`CLASSPATH=hello.dex app_process64 /system/bin Hello` prints "Merhaba from ART" —
AndroidRuntime, libandroid_runtime's JNI for all framework classes, RuntimeInit,
ProcessState and a binder thread pool, full BCP + boot image + CMC: **386 M
instructions, ~8 s host** (35 % linker64 linking libandroid_runtime's ~150 libraries,
32 % libart). It works over **`core/binder.c`, an in-process binder driver**: open of
/dev/binder, hwbinder, vndbinder; VERSION, SET_MAX_THREADS and friends; mmap of the
receive buffer; BINDER_WRITE_READ walking BC_* commands by their encoded size and
resuming from write/read_consumed like the kernel; a looper with nothing to read sleeps
(5 ms) and retries. Handle 0 is a built-in servicemanager: PING and INTERFACE answered,
the Android 14 IServiceManager calls answered "no such service" (null binder, empty
lists) — `aoiproc -t` logs each call as `[binder] servicemanager call N (...) "name"`.
Any other handle is a dead object. This hello asks for no service yet.
Also: the profiler counts only expired time slices (a thread waiting on a pipe no
longer shows up as `read`), and the code-name table forgets unmapped ranges when full.

**ActivityThread.main() starts** (`app_process64 /system/bin android.app.ActivityThread`,
every app process's real entry): its main Looper runs on emulated **eventfd** and
**epoll** (+ `ppoll`): core/proc.c keeps the counter and the interest lists itself
(Darwin has neither); readiness is level-triggered, from the eventfd counter or the
host's poll(2) with no wait for pipes and files, and a wait with nothing ready sleeps
≤ 2 ms and re-runs until its own deadline, so another thread's eventfd write wakes it
(`tests/pipes.c` checks it). ActivityThread.attach then asks servicemanager for
**"activity"**, gets null, and dies in `IActivityManager.attachApplication` — the expected
wall. (Fixed on the way: binder replies were written with the guest's protection, but the
receive buffer is read-only to the guest, so every non-empty reply became DEAD_REPLY.)

**System services will be Java, in the app process.** Building framework Parcelables
(ApplicationInfo, Configuration, …) by hand in C would be fragile and tied to one
Android version; instead our services are Java classes (`java/src`) loaded into the app
process, subclassing the framework's own `I…Manager.Stub`s, so they can use the framework
to build those objects. The servicemanager in core/binder.c keeps the flat_binder_object
`addService` gave it and returns that same object from `getService`: libbinder resolves
it to the local BBinder and Java gets its own Binder back — every call is a plain Java
call, no parcel crosses the driver. As the kernel does for a node it holds, it sends the
owner BR_INCREFS + BR_ACQUIRE (without them the native JavaBBinder was freed after
addService and getService returned a dangling pointer). Toolchain: `tools/javadex.sh`
compiles `java/src` with javac (Java 8 level) against hand-written stubs of the hidden
framework classes in `java/stubs` (compile-only), then dx (dalvik-dx from Maven Central,
pinned; d8 lives on the unreachable dl.google.com). `aoi.ServiceTest` in
`make android-test`: "servicemanager: local binder ok".

**A real APK's process starts** (Qalculate, `aoi.Main`): `app_process64 /system/bin
aoi.Main /data/app/qalculate/base.apk` parses the APK with the framework's own
PackageParser (package, launcher activity, targetSdk 35), registers our Java services,
and runs ActivityThread.main. Our ActivityManager answers attachApplication with
bindApplication (the guest framework's exact 27-argument signature, read from its dex
by `tools/dexsig.py`), the app's **class loader, Application and onCreate run**
(handleBindApplication to the end), and finishAttachApplication launches the launcher
activity with a ClientTransaction (LaunchActivityItem + ResumeActivityItem, as the real
realStartActivityLocked does): **performLaunchActivity starts on MainActivity**. Services
so far (java/src/aoi): `activity` (attach/bind/launch, permissions, receivers,
broadcasts, memory, crash report), `package` (the parsed APK: application, package and
activity info, uid; nothing else installed), `permissionmgr` (all granted, no split
permissions). Missing ones are reported by the AbstractMethodError they raise. Learned:
AIDL stubs of permission-annotated interfaces need our own PermissionEnforcer (the
default wants system_server's context); a Configuration must carry a locale.

Since then (same loop: run, read the AbstractMethodError or NPE, add the call):
`activity_task` (IActivityTaskManager + an ActivityClientController for lifecycle
reports, display 0, one task), `user` (user 0, unlocked, unrestricted), a **settings
provider** (`aoi.SettingsProvider`, a real ContentProvider answering
`call("GET_global"/"GET_secure"/"GET_system")` from phone defaults — Settings does not
tolerate a missing one; handed out by getContentProvider("settings")), and the app's
**own content providers** passed to bindApplication, so androidx.startup's
InitializationProvider runs its initializers (PackageManager.getProviderInfo returns
the manifest meta-data it reads). CPU: `shll{2}` (ART's compaction uses it; checked
against Unicorn). Property `servicemanager.ready=true` (libbinder's C++ client waits on
it). Where it stands: inside `Activity.attach`/the initializers, native code waits for
**SurfaceFlinger** (`SurfaceFlingerAIDL`): Choreographer's vsync comes from its display
event connection. framework.jar has no Java binding of android.gui.ISurfaceComposer, so
our SurfaceFlinger must be native.

**SurfaceFlinger, natively** (`core/sf.c`, behind an in-process binder handle; the binder
driver now has native objects whose transactions call C functions, replies with
handles, fds or a status): `SurfaceFlingerAIDL` answers bootFinished,
getPhysicalDisplayIds and createDisplayEventConnection; the connection hands out a host
SEQPACKET socketpair (`stealReceiveChannel`: BitTube travels as two fds in this build)
and writes 216-byte DisplayEventReceiver::Event records at 60 Hz on requestNextVsync /
setVsyncRate (`aoi_sf_tick`, run by the scheduler). C++ AIDL codes follow the .aidl
order of the build: `tools/aidlcodes.py` reads them from the guest's libgui.
`display` (Java): one display, 1179 x 2556 px at 480 dpi (393 x 852 dp, the iPhone's
points at 3x), 60 Hz. Rendering: a GSI has no vendor GLES driver and its ANGLE needs
Vulkan, so aoi.Main turns ThreadedRenderer off: windows draw **in software** (Skia on
the CPU) — the buffer path they need (layers, gralloc buffers SurfaceFlinger can read)
is the same one a later GL-forwarding driver will use. **Qalculate's
`MainActivity.onCreate` runs** (the app's own code) and stops at `window`.

**MainActivity is resumed.** With `window` (IWindowManager + a WindowSession: windows get
a real InputChannel pair, full-screen frames, a configuration with window bounds, a BLAST
layer from our SurfaceFlinger on relayout), `input_method` (no IME yet: NO_IME),
PackageManager.queryProperty, SurfaceFlinger's createConnection + ISurfaceComposerClient
.createSurface (each layer its own native handle, a structured CreateSurfaceResult) and
socketpair/sendto/recvfrom/sockopts in core/proc.c, the trace reads "window added:
…/MainActivity" and "activity resumed". It then stops on NEON in libhwui (Skia):
`tools/isawords.py` + `build/isacheck` over the rendering libraries (libhwui, minikin,
harfbuzz, freetype, codecs, libgui/libui; 498 k distinct words) list 4,501 encodings in
~60 mnemonics we do not execute yet — saturating/rounding/narrowing NEON, FP16
conversions, reciprocal estimates, by-element multiplies, ld/st2-4 lanes.

**Those NEON encodings now run** (core/simd.c): saturating/rounding/narrowing shifts,
by-element integer multiplies, FP16 conversions, reciprocal/rsqrt estimates and steps,
frint*, fmaxnm/fminnm (vector, pairwise, across lanes), ld/st2-4 single lanes and ld2r-4r;
isacheck over the rendering libraries: **0 missing, 0 WRONG** of 497,719 distinct words,
difftest WRONG 0 (new class `mrs/msr nzcv`, which harfbuzz uses). The app now runs
through text layout (libhwui, minikin, harfbuzz) and asks for **gralloc** next.

**The first frame (host).** Qalculate's real UI — top bar, the keypad, Material 3
colours — drawn by the app in software (Skia), queued by BLASTBufferQueue into a
gralloc buffer from core/gralloc.c, received by our SurfaceFlinger's setTransactionState
and written out with `AOI_SF_DUMP`: 1179 x 2556, stride 1184, format RGBX_8888, ~4.2 G
instructions (88 s on the host) from process start. Fixes on the way: allocate2's
request has an `additionalOptions` array before `count`; fcntl record locks (F_GETLK,
F_SETLK(W), OFD) are granted (one process), SQLite needs them.

**On the iPhone (app 0.13, not yet run on the device):** an "Uygulama" button runs the
picked APK the same way (`aoi_android_app`, ios/androidtest.c) and shows each frame full
screen (`p->frame`, called by core/sf.c with packed RGBX rows). The bundle is read-only,
so the guest's /data is a writable copy in Documents (`p->data`: core/proc.c maps guest
/data/... there). The bundled root grows to ~385 MB: ios/android-files.txt now lists
what Qalculate's process opens (tools/tracefiles.py over a trace; .apex packages that
are only stat'ed with O_PATH become empty files). The workflow builds aoi.dex (setup-java)
and mapper.aoi.so (brew lld). Checked on the host with `build/iostest`
(AOI_ANDROID_ROOT + AOI_APP_DATA): same frame. Touch: the iOS view sends one-finger
touches (`aoi_android_touch` -> `aoi_proc_touch`) into a host pipe the guest opens as
/dev/aoi_input; `aoi.Input` (java/src) reads the records and sends MotionEvents with the
framework's own InputEventSender on the server end of the window's input channel.
`iostest` taps with `AOI_APP_TAPS="x,y;..."` (`aoiproc` with `AOI_TAPS`). Buffers come
back: when a new buffer replaces the one on screen, sf.c calls the app's
ITransactionCompletedListener.onReleaseBuffer (code 2, one-way; ReleaseCallbackId is a
nullable parcelable) through `aoi_binder_send`, the driver's first host-to-guest call
(BR_TRANSACTION queued for a looper thread). BLASTBufferQueue then cycles two buffers;
before that it stalled after three. A tap on Qalculate's ↵ adds a history entry.
`AOI_SF_DUMP=frame%d.ppm` keeps every frame. IPA v0.13.39 (174 MB, root 373 MB).
On the phone v0.13.39 said "no framework": app_process64 is exec'd, not opened, so the
trace-made list missed it. Now checked before a release: a root made by mini-root.sh
from ios/android-files.txt runs the app on the host (frames, peak RSS 426 MiB). That
also found the boot image's arm64/*.vdex links (stat'ed, so the trace only shows their
targets; tracefiles.py now adds links next to listed files) and a missing `content`
service (aoi.ContentService: registerContentObserver and friends, no sync).

**Qalculate runs on the iPhone (v0.13.49, iPhone 16 Pro, iOS 27.0.1, no JIT):** the
app's real UI on the phone's screen, touches reach its keys, and its own engine
(libqalculate, interpreted) computes: "9985" ↵ gives 9.985 × 10³ in the history.
Keys repeated while held ("9855555999…"): an interpreted tap lasts longer than the
400 ms long-press timeout in the app's time; v0.13.50 sets long_press_timeout to 5 s
(core settings in bindApplication). 0.14 hides iOS's status bar over the app's frames.

**Where a frame's time goes** (aoiproc -p from the first frame on, six taps; ~6 s per
frame on the phone): libart 63 %, libhwui 28 %, libc 5 %, the rest small. The app's
dex code (Compose, Kotlin) is interpreted by ART inside our interpreter; the
framework is AOT-compiled in the boot image, the APK is not. Next: dex2oat the APK
(`speed`) once at install, so its code runs as compiled arm64. dex2oat works in the
interpreter now (MAP_SHARED writes reach the vdex/odex; `verify` takes 18 s / 1 G
instructions on the host).

**The APK compiled ahead of time** (dex2oat `speed`, 12.7 G instructions / 3.7 min on
the host, once per APK; ios/androidtest.c runs it before the first launch into
/data/app/apk/oat/arm64/): first frame after 2.08 G instructions (was ~3-4 G), a frame
after a tap ~135 M (was ~400 M); libart falls to 2.6 % of a frame and libhwui (Skia's
software rasterizer) is 88.7 %. So the display is now 2x (786 x 1704 px at 320 dpi,
393 x 852 dp as before): 2.25 times fewer pixels to rasterize.

**Interpreter speed while drawing** (host, `framebench`: guest instructions per second
over the frames after taps; Skia's raster pipeline is NEON float code): 24.1 M/s before,
33.4 M/s with SIMD&FP data processing dispatched straight to simd.c (it sat at the end
of a 60-pattern chain) and 4 x single fadd/fsub/fmul/fmax/fmin on host floats when no
NaN is involved, 37.0 M/s with the chain started at the instruction's group (op0):
+54 %. difftest WRONG 0, isacheck 0 missing / 0 wrong.

**The network (app 0.47).** Apps reach the internet through the host's sockets:
- core/proc.c: AF_INET/AF_INET6 sockets are host sockets, non-blocking underneath
  (a blocking call sleeps the thread and runs again, as pipes do; poll/epoll ask
  the host socket): connect (blocking or EINPROGRESS), bind/listen/accept4,
  send/recv(from/msg), get/setsockopt (the options worth passing on), getsockname/
  getpeername, shutdown, FIONREAD/FIONBIO, Linux errno for the network errors.
  Addresses are translated (Darwin's sockaddrs have a length byte, AF_INET6 is 30
  there). On a host without IPv6, Java's dual-stack AF_INET6 sockets get an AF_INET
  host socket and v4-mapped addresses (::ffff:a.b.c.d) both ways. SIGPIPE ignored.
- DNS: bionic asks netd at /dev/socket/dnsproxyd ("getaddrinfo host serv flags
  family socktype protocol netid"); the connect gives a host pair and the answer
  (netd's "222" format, Linux sockaddrs) comes from the host's getaddrinfo, logged
  as "I/aoi: dns NAME: ok". It blocks the guest while it resolves. The guest runs
  with ANDROID_NO_USE_FWMARK_CLIENT=1, so libnetd_client does not ask netd's
  fwmarkd to tag every socket (ios/androidtest.c, tools/android-setup.sh).
- aoi.ConnectivityService ("connectivity"): one Wi-Fi network (netId 100), CONNECTED,
  with INTERNET, VALIDATED, NOT_METERED...; the framework's own Network,
  NetworkInfo, NetworkCapabilities and LinkProperties, made by reflection. Network
  callbacks (registerNetworkCallback) are not answered yet.
- The CA certificates (/apex/com.android.conscrypt/cacerts, 135 files) are in the
  iOS root now: HTTPS had no trust anchors there.
- java.net.Socket.close: PlainSocketImpl shuts down a socketpair end (its close
  marker): shutdown works on host-backed pairs.
tests/run_android.sh "network": aoi.NetworkTest sees the network through the
framework's IConnectivityManager proxy, resolves localhost and GETs a page from a
local python HTTP server. On the host, Qalculate resolves www.ecb.europa.eu and
reaches the TLS handshake (refused here by the sandbox proxy's own CA).
Kiwi: Chromium checks that its child-process service exists (getServiceInfo);
aoi.PackageManager answered null. It now answers from the manifest (services and
receivers). Its renderer still needs a process of its own (bindService to
SandboxedProcessService0 is not done): open.

**On the phone (0.47 log) and stand-ins that answer empty lists (app 0.48).**
DNS and HTTPS work on the phone: Qalculate downloads its exchange rates (coinbase,
jsdelivr); www.ecb.europa.eu alone fails with "Trust anchor for certification path
not found" although the store has the 135 roots (likely a chain the server sends
incomplete; Android does not fetch missing intermediates): open. WhatsApp reaches
Google's servers (a real 403, API_KEY_ANDROID_APP_BLOCKED: the app's signature check)
and gets past the EULA to RegisterAsCompanionActivity (no SIM), then died in
ShortcutManager.getShortcuts on a null ParceledListSlice from the "shortcut" stand-in.
Services.NullService now reads the interface name from the call's token, finds the
method by its Stub's TRANSACTION_ code and answers an empty ParceledListSlice when
that is the return type (arrays and lists already read empty from zeros).
tests/run_android.sh "stand-in services answer empty lists": getShortcuts and
IJobScheduler.getAllPendingJobsInNamespace through the framework's proxies.
Kiwi: bindServiceInstance (missing) and then a Chromium CHECK (brk #0 in libchrome)
on the fallback to SandboxedProcessService1: it needs a renderer process. open.

**Shared storage and ashmem (app 0.49).** From the 0.48 log:
- WhatsApp died in Environment.getExternalStorageState (an index into an empty
  volume list). aoi.StorageService now answers getVolumeList with one volume: the
  primary emulated shared storage, mounted, at /data/media/0 (inside the app's data,
  so writable). The framework's own StorageVolume, made by reflection.
- Uptodown's SQLite queries failed ("Row too big to fit into CursorWindow": the
  window's ashmem region could not be made) and SoundPool's MemoryHeapBase too:
  no /dev/ashmem. libcutils makes ashmem regions as memfds when sys.use_memfd is
  true (tools/mkprops.py now sets it): core/proc.c implements memfd_create (an
  unlinked host file under the guest's /data/local/tmp) and F_ADD_SEALS/F_GET_SEALS
  (recorded, not enforced). ART's JIT code cache ("jit-cache"...) still gets ENOSYS:
  with a memfd it maps its cache twice (RX and RW) and our MAP_SHARED is a copy per
  mapping (it aborted in debugger_interface).
tests/run_android.sh "storage volume, ashmem as memfd": the volume through the
IStorageManager proxy, SharedMemory written and read, a CursorWindow with a 100 KB blob.
Still open from the log: Uptodown's WebView ("not allowed in privileged processes":
the guest runs as uid 0), Kiwi's renderer process, ECB's certificate chain.

**Testing apps on the host like the phone; AndroidKeyStore (app 0.50).** Apps like the
user's (WhatsApp, Kiwi) are tested here first: GitHub release assets are reachable from
the cloud sessions (F-Droid and Google are not): Molly (a Signal fork: WhatsApp-like),
Element (Matrix), NewPipe, Cromite (Chromium: Kiwi-like).
`tools/app-install.sh ROOT APP.apk DIR` installs one as ios/main.m does, and
`build/iostest` with AOI_APP_DATA runs the phone's own path (ios/androidtest.c:
libraries, dex2oat, the app, taps, snapshot; AOI_APP_LOG for the log). What it found:
- Native libraries: an APK whose libraries are deflated (extractNativeLibs; Molly,
  Element and most apps) could not load them; only stored ones load from inside the
  APK. core/apk.c aoi_apk_extract_libs, run once per APK (ios/androidtest.c
  install_libs), writes them to /data/app/apk/lib/arm64 (nativeloader's path).
- dex2oat aborted big apps after 9.5 minutes (its watchdog): now --no-watch-dog. And a
  run cut short left a partial odex taken for done: oat/arm64/.done now marks a run
  that finished (an app with an odex but no marker is compiled once more).
- AndroidKeyStore was not even a provider (the zygote installs it; we start without
  one): Element died of "AndroidKeyStore KeyStore not available". aoi.Keystore is
  keystore2 (android.system.keystore2.IKeystoreService/default) with software keys
  kept in the app's /data/misc/keystore/aoi: AES (GCM/CBC/CTR/ECB), 3DES, HMAC, EC and
  RSA signing, RSA decryption, ECDH, imported keys, certificate entries; a new key pair
  gets a self-signed X.509 certificate (DER built by hand). Reported as TEE keys.
  tests/run_android.sh "AndroidKeyStore": the framework's provider end to end.
- NewPipe died of a null BatteryManager: "batteryproperties" (not a Context name)
  now has a stand-in.
- When an app stops on a fault, every thread's backtrace goes to its log
  (aoi_proc_log_threads, as /dev/aoi_threads does).
- Cromite reads /data/local/chrome-command-line (Chromium's rooted-device flags file):
  with --single-process it got past the renderer launch, then stopped on a CHECK. open.

**Chromium in one process, signatures, timerfd (app 0.51).** More from the host app tests:
- Cromite (Chromium 153, as Kiwi) stopped on a CHECK right after loading libchrome:
  timerfd_create was ENOSYS (its message loop). core/proc.c now has timerfd
  (create/settime/gettime; monotonic and realtime, absolute or relative, intervals;
  readable through read, poll and epoll; a blocking read sleeps until it is due).
  Then PowerManager.addThermalStatusListener threw: a stand-in's boolean
  register...() calls now answer true. With /data/local/chrome-command-line
  "_ --single-process" (Chromium reads that rooted-device flags file without a debug
  build; ios/androidtest.c writes it once) the renderer runs inside the app: Cromite
  shows its first-run screens, takes taps, resolves names and does TLS. Kiwi takes the
  same path. ("media.camera" got a stand-in: CameraManager retried every second.)
- Signatures: PackageInfo.signatures / signingInfo (GET_SIGNATURES,
  GET_SIGNING_CERTIFICATES) from the APK's signing block via the framework's
  ApkSignatureVerifier (without the digest pass), and hasSigningCertificate. NewPipe
  (ACRA) died on a null SigningInfo; Google's APIs send the certificate's SHA-1, which
  is the likely cause of WhatsApp's API_KEY_ANDROID_APP_BLOCKED.
- Stand-ins answer an empty LocaleList (LocaleManager.getApplicationLocales:
  AppCompat; NewPipe died of it) besides an empty ParceledListSlice.
- Realm (Element): fallocate (the file grows; KEEP_SIZE; no hole punching) and
  mknodat for FIFOs (opened as a non-blocking pipe underneath); *xattr calls answer
  ENOTSUP, mlock* succeed. tests/pipes.c steps 8 (timerfd), 16 (fallocate), 32 (FIFO).
- UFFDIO_ZEROPAGE (and a UFFDIO_COPY of a zero page) no longer writes the page: a
  missing page reads as zero already, and ART's GC zero-fills its whole moving space
  this way (1 GB with a large heap) - writing it made the memory real.
- Host testing: the sandbox's TLS inspection CAs can be added to a local root's
  cacerts (never to the repository's) so HTTPS apps work here; the snapshot reads every
  page on Linux unless AOI_SNAP_MINCORE=1 (as on iOS), which made "guest in host
  memory" look like 3.7 GB.
Now: NewPipe opens to its main screen (YouTube answers this cloud's address with
something it cannot parse); Molly reaches its passphrase screen; Element gets past
Realm and the keystore, then a thread of libmaplibre recurses until its stack runs
out (open: a deep recursion or one of ours). And dex2oat `speed` took 41 min (Molly)
and 50 min (Element, 917 MB peak) here: too long and too big for the phone - next.

**Big apps compiled in two steps (app 0.52).** dex2oat `speed` on a big app took 41 min
and 750 MB (Molly, 59 MB of dex) or 50 min and 917 MB (Element, 68 MB): the phone would
end it (memory) or the user would give up; WhatsApp is of that size. Measured here:
Molly `verify` 172 s / 250 MB, `speed-profile` with the app's profile 337 s / 305 MB
(9.6 MB of compiled code: its startup and hot paths). ios/androidtest.c compile_apk:
- up to 16 MB of dex (Qalculate 2.9, NewPipe 11, Cromite 11): `speed` once, as before;
- more: `verify` at install, then at a later launch, once the app has a profile in
  /data/misc/profiles/cur/0/<package>/primary.prof, `speed-profile` with it.
The profile: ProfileInstaller (androidx, in most apps) writes the APK's baseline
profile there at the first launch when the directory exists (installd makes it on a
phone: aoi.Main does now); ART adds the methods its JIT finds hot
(dalvik.vm.usejitprofiles, tools/mkprops.py). oat/arm64/.done records the last filter
(speed, verify, speed-profile, failed). core/apk.c aoi_apk_dex_bytes. Molly here:
verify 169 s, then speed-profile 323 s / 313 MB, then the app as before.

**On the phone (0.52 log) and every IWindowSession call (app 0.53).** WhatsApp's 86 MB of
dex verified in 249 s (409 MB peak); it reached RegisterAsCompanionActivity and ran
for minutes. Kiwi, with --single-process, got through its native start, the network
and its first-run screens. Both then died the same way, when a Transition started:
AbstractMethodError IWindowSession.getWindowId. The framework calls aoi.WindowSession
directly (no proxy, so no default answer for a method we left out): it now has
getWindowId (an IWindowId that knows whether the window has focus) and every other
method of the real interface (22 more, answered with nothing: drag and drop,
wallpapers, embedded windows, moving tasks), checked descriptor by descriptor against
framework.jar's IWindowSession (0 missing).

**First launch no longer waits for dex2oat (app 0.54).** On the phone the first launch
sat behind dex2oat for minutes (WhatsApp `verify` 249 s, a `speed` compile up to ~10
min) before any frame - nobody waits that long. ios/androidtest.c compile_apk now runs
dex2oat on a detached host thread while the app starts uncompiled (the interpreter
runs the dex as is): output goes to app/apk/oat.new/arm64 and is renamed over
oat/arm64 when done; app/apk/oat/.state records the filter (speed, verify,
speed-profile, failed). The odex size/mtime is part of the snapshot key, so the next
launch cold-starts with the compiled code. For this the core's function-static
buffers (signal frames, shm_sync, DNS, net, binder req/rep) are `_Thread_local`, and
the GPU hooks only apply to the app run, not the compiler. tools/iostest.c waits
for the compile after the app ends. NewPipe here, fresh install: first frame at 56 s
(before: ~10 min of dex2oat first), compile done at 547 s in the background; second
launch (compiled, cold) first frame at 21 s (runtime + framework start 7 s, app init
to activity idle 13 s); later launches resume from the snapshot in ~1 s.

**The background compile, kind to the phone (app 0.55).** dex2oat next to the app
means two busy cores for a few minutes, once per app. Now: its thread has
QOS_CLASS_UTILITY (behind the app's threads, iOS prefers the efficiency cores); it
waits while the phone is hot (thermal state serious or critical) or in Low Power
Mode (ios/main.m tells aoi_android_compile_hold; struct aoi_proc pause_request makes
aoi_proc_run wait between time slices); it does not start with less than 900 MB
(speed) / 1100 MB (verify, speed-profile) available (os_proc_available_memory: the
app starts uncompiled); and when available memory falls under 200 MB with both
running, it is ended (no .state is written: a later launch compiles again) before
iOS ends the app. tools/iostest.c AOI_COMPILE_HOLD=N holds it for N s. NewPipe here
with the compile held: first frame at ~40 s instead of 56 s with it running.

**Today's TLS roots (app 0.56).** Qalculate's exchange rates from www.ecb.europa.eu
failed "Trust anchor for certification path not found" while api.coinbase.com worked:
Android 14's GSI has the root store of its build, a phone gets newer roots with its
updatable conscrypt module. The 21 roots of Mozilla's store (certifi 2026.06.17) the
GSI lacks - among them Sectigo Public Server Authentication Root R46/E46, which
Sectigo's chains end in since 2025, and SSL.com's 2022 roots - are in
tools/cacerts-extra.pem with their conscrypt file names (subject_hash_old.0);
tools/android-root.sh adds them to /apex/com.android.conscrypt/cacerts and
ios/android-files.txt ships them. aoi.CaTest (tests/run_android.sh, 20 checks): the
system TrustManager has them and trusts a chain ending in R46. Not checked against
ECB itself from here (the cloud's egress re-signs TLS): the phone's log will tell.

**WebView (app 0.57).** Uptodown died on "WebView is not allowed in privileged
processes": the guest ran as uid 0. Now a WebView page renders with JavaScript
(tools/mktestapk.py OUT.apk web: aoi.webapp loads a page from a string; here: first
frame, page finished, `title "web 42"`, the text drawn by the GPU). What it took:
- **The app runs as uid 10100** (aoi.Main's ApplicationInfo.uid; struct aoi_proc uid:
  getuid & co., getresuid; dex2oat and the tests stay root). Files are owned as on a
  device (core/proc.c owner()): the app's /data/data, /data/user*, /data/media,
  /data/misc/profiles; root the rest - bionic only maps a property area owned by root,
  and ART refuses a dex the app could write (faccessat W_OK on another's file: EACCES).
  aoiproc AOI_UID=10100 runs a program so.
- **The provider:** the GSI's AOSP WebView 119 (/product/app/webview/webview.apk;
  tools/android-root.sh drops its 32-bit library with tools/zipdrop.py, which keeps
  stored entries 4 KiB-aligned: 185 to 118 MB). aoi.WebViewUpdate ("webviewupdate")
  names it, multiprocess off (its renderer in the app's process); aoi.PackageManager
  knows com.android.webview (App.webview()) and has android.software.webview.
- **Read-only shared memory:** Chromium tells a read-only region by a writable
  MAP_SHARED mmap failing: on a memfd sealed F_SEAL_WRITE/F_SEAL_FUTURE_WRITE it is
  EPERM now.
- **128 GiB of guest addresses** (was 64): PartitionAlloc's two 16 GiB pools left no
  room for V8's sandbox (8 GiB, 4 GiB-aligned: "V8 process OOM").
- **"media.player"** (aoi.MediaPlayerService): an empty codec list. Without it
  libstagefright builds the list in-process from the Codec2 HALs, which abort without
  hwservicemanager (Chromium reads it at start).
- **MAP_SHARED is shared:** two mappings of one memfd (Chromium's GPU command buffer:
  the renderer writes, the GPU thread reads) were two copies ("raster_decoder Error: 1
  for Command Noop", tiles never drawn). core/vm.c maps a shared file's 16 KiB host
  pages MAP_SHARED (aoi_vm_map_file shared; a bit per host page in vm->shared), and
  sys_mmap places a non-fixed MAP_SHARED mapping where address and offset agree
  modulo 16 KiB, with the rest of its last host page reserved (a few KiB of address
  space stay reserved after munmap). Unmapping or mapping over such a page gives it
  private memory first (never zeros into the file); MADV_DONTNEED leaves it (Linux
  does). A read-only fd falls back to a private mapping. A snapshot is refused while
  one file is mapped twice (restored they would be two copies): WebView apps start
  cold each time for now.
  Only for a writable fd (a read-only one cannot be mapped shared: libandroidfw maps
  every asset it reads MAP_SHARED read-only), and the pad pages (AOI_PROT_SLACK) are
  unmapped with their mapping: left behind, thousands of them made every mmap's
  first-fit search crawl.

**Cromite to its New Tab page; Element past maplibre (app 0.58).** Host runs of
the Chromium and Matrix apps, each fix found where they stopped:
- prctl PR_GET_NAME/PR_SET_NAME keep a name per thread (struct aoi_thread comm):
  maplibre's logger asks the thread's name and logs the failure, endlessly (Element's
  stack overflow).
- memfd_create and eventfd2 refuse unknown flags (EINVAL): Chromium's mojo probes the
  kernel with memfd_create("", ~0) and CHECKs the answer (channel_linux.cc).
- mremap MREMAP_DONTUNMAP of a file's pages copies them and leaves the source as it
  was (Linux refaults it from the file): V8 remaps libchrome's builtins next to its
  code range and CHECKs both views agree. Anonymous memory still moves (ART's GC).
- Stand-ins answer a StorageStats (zero bytes) and an empty Bundle (restrictions)
  instead of null: Chromium's storage metrics and policy reader dereferenced them.
Cromite now runs to its New Tab page and stays up. Open: web content (chrome://version)
stays white - its compositor never draws into the SurfaceView (only the window layer
gets buffers); WebView, which draws through HWUI, works. tools/iostest.c
AOI_APP_STOP_AFTER=N stops the app after N s and logs every thread.

**No more waiting for the audio services (app 0.59).** Native AudioSystem waits for
"media.audio_policy" (the log's "getService: waiting for media.audio_policy"):
Chromium's AudioThread sat there, Element's start too. "media.audio_policy" and
"media.audio_flinger" are stand-ins now: their calls fail at once (no sound yet; an
AudioFlinger of our own is the way to sound). Thread dumps name each thread (its
prctl name). AOI_GL_TRACE also logs every EGL op.
Cromite's web content, what is known: after a navigation the renderer, viz and the
GPU thread all sit idle; the GPU thread makes a context but never a window surface;
with --disable-gpu-compositing it is the same, so it is not the GPU. The SurfaceView
Chromium keeps (translucent, the BLAST layer) is created, valid and visible, with
mDrawFinished false: its first frame never comes. Next: whether the browser
compositor gets the surface (CompositorImpl::SetSurface) and is visible, and whether
begin-frames reach it. The same for an https page typed in the omnibox (tools/iostest.c
AOI_APP_TAPS "type:TEXT" types on the host keyboard, then return), so it is not WebUI's
own process; this build's VLOGs are compiled out.

Host testing: `make build/iostest-gpu` (Mesa's GLES as the phone's GPU: HWUI and
WebView's GPU thread), AOI_APP_TRACE=file (every syscall of the app run). Cromite and
Molly still reach the screens they did. The phone root gains the WebView, its
libraries and libmedia_jni's (ios/android-files.txt, the end).

**Compatibility run; a sensor service (2026-10-05).** tools/compat.sh ROOT OUT APK...
installs, compiles and launches each APK through build/iostest-gpu for $RUN s and sorts
them into OPENS / CRASH / NO-FRAME, with tools/appcheck.sh's to-do list per app.
First finding: Element aborted in native SensorManager ("getService(SensorService)
NULL"); "sensorservice" is now a native stand-in with no sensors (core/af.c), and
Element opens. tools/iostest.c's AOI_APP_STOP_AFTER now counts from the app's start,
not from before a compile.
Results, 17 open-source APKs from GitHub releases (host, 150 s each, no input):
10 OPENS (Element, Molly, Cromite, Shattered Pixel Dungeon, Aegis, KeePassDX, Mihon,
Jellyfin, Seal, Delta Chat), 1 intermittent (NewPipe: once aborted on a 112-byte read
from a display-event BitTube, "partial events", with three apps at once; alone it opens
twice in a row), 6 CRASH:
- Material Files, Bitwarden: Firebase without Google Play services ("Default FirebaseApp
  is not initialized"; Bitwarden then faults in its own native code).
- Termux: bindService() of its own service failed.
- LibreTube: "size must be > 0" in okio while the Application is created.
- LocalSend (Flutter): WifiManager is null (no "wifi" service), then SIGSEGV.
- Obtainium (Flutter): ends without a frame after its first-run log.

**Deterministic mode, AOI_SEED (host tool).** AOI_SEED=n makes a run repeatable:
AT_RANDOM, getrandom and /dev/urandom draw from a splitmix64 generator seeded with n,
and the clocks follow the instruction count (10 ns per guest instruction; the wall
clock starts in 2023, so TLS certificate dates may fail). When every thread sleeps,
the virtual clock jumps ahead instead of the host sleeping, so sleep loops end. Two
runs with the same seed give identical syscall traces (toybox ls; ART hello world;
OWASP UnCrackable Level 4's obfuscated native constructors, 1.94 B instructions).
For replay debugging and analysis; unset, nothing changes. One process at a time:
the generator and clock are global.

**Runtime.exec: child processes (app 0.74).** An app's Runtime.exec / ProcessBuilder
failed with "error=38, Function not implemented" (fork). Now (core/proc.c):
- libopenjdk's childproc calls vfork() (clone CLONE_VM|CLONE_VFORK|SIGCHLD). The child
  runs as vfork's does: in the parent's memory, on the calling thread, while the other
  threads wait (schedule), with a copy of the fd table (its dup2s and closes are its
  own; closing them leaves the parent's epoll lists alone). getpid/gettid answer its
  pid (from 20000).
- Its execve reads path, argv and envp, finds the file (ENOENT/ENOEXEC as Linux would,
  so JDK_execvpe walks PATH; a "#!" script runs its interpreter) and starts it as a
  guest process of its own on a host thread (struct aoi_child), with the fds not
  marked close-on-exec (the guest's FD_CLOEXEC is tracked now: F_SETFD/F_GETFD,
  O_CLOEXEC, pipe2, socket(pair), dup3, F_DUPFD_CLOEXEC, accept4, eventfd, epoll,
  memfd, timerfd). The parent then returns from vfork with the pid. An _exit before
  exec is the child's exit status.
- Only vfork (and bionic's posix_spawn, which uses it): a real fork() needs a copy of
  the memory and still answers ENOSYS (code that forks handles that; run in the
  parent's memory, a fork child corrupted it: the storage test's stack check).
- wait4/waitid report it (exit code or signal; WNOHANG, WNOWAIT), kill stops it
  (Process.destroy). A child's fds 0-2 are its own pipes and close for real (a parent
  waiting for EOF on its stdout saw none).
- stat("/proc/self/exe") is the program: linker64 fell back to argv[0] ("sh") and
  failed.
tests/run_android.sh "Runtime.exec": aoi.ExecTest runs `toybox echo hello`,
`sh -c 'echo $((6*7)); exit 3'` and a missing program: hello (exit 0), 42 (exit 3),
IOException. The programs are the ones in the bundle (toybox, mksh); a snapshot does
not keep running children. AOI_CHILD_TRACE=file traces a child's syscalls (host).

**Chromium browsers show web pages (app 0.73).** Cromite (Chromium 153) renders
chrome://version and an http page with CSS, JavaScript and a canvas (host, Mesa as the
GPU). Its compositor had never drawn: three causes, each found with Chromium's own
startup trace (--trace-startup=..., read with Perfetto's trace_processor):
- **sendmsg/recvmsg on socketpairs** were ENOSYS. Mojo's channel between the browser
  and the in-process GPU/viz passes file descriptors (SCM_RIGHTS): the channel shut
  down (ChannelPosix::ShutDownImpl) right after viz's FrameSinkManager was made, and
  the browser's root CompositorFrameSink never reached viz. core/proc.c unix_msg: the
  host fds travel in a host SCM_RIGHTS message, in order with the data; each one's
  guest side (kind, path, socketpair, eventfd counter) waits in a table and is found
  again by the host file's identity on the receiving end.
- **eventfd counters are shared** by every fd for them (dup, dup3, F_DUPFD, received
  over a socket), as on Linux (aoi_proc.ev, refcounted). Mojo then upgrades its
  channels to shared memory signalled by an eventfd (ChannelLinux); with a counter per
  fd the peer was never woken.
- Chromium's flags file (/data/local/chrome-command-line, ios/androidtest.c) adds
  --disable-features=AndroidSurfaceControl,EnableDrDc: with SurfaceControl viz
  renders into AHardwareBuffers and with DrDc tiles are shared between GPU threads,
  both through EGLImages from AHardwareBuffers (GL_OES_EGL_image), which our EGL does
  not offer ("SharedImageFormat RGBA_8888 can not be used to create a GL texture from
  AHardwareBuffer", then a lost context). Without them viz draws into the SurfaceView's
  EGL window surface like any GL app. A file the app wrote before (only
  --single-process) is replaced.
Host debugging added: AOI_WATCH_LIB=libX.so logs calls into a library from outside it
(core/hle.c), AOI_SF_LAYER_DUMP=ID (AOI_SF_LAYER_FILE) writes that layer's buffers,
the trace logs display event connections, requestNextVsync and fds passed over sockets.
Open: TLS pages on the host (the sandbox's proxy CA is not in Chrome's root store;
the phone has none), saving Chromium apps in a snapshot (GPU state).

**SoundPool plays: static AudioTracks (app 0.72).** Games' effects (SoundPool, so
libGDX's Sound: cube.run) were silent: libsoundpool decodes a sound into a shared
buffer (MemoryHeapBase, a memfd) and plays it as a MODE_STATIC AudioTrack, which
core/af.c took for a streaming one. Now CreateTrackRequest's sharedBuffer
(SharedFileRegion: fd, offset, size) makes a static track: its frames are read from
a host dup of that memfd; the cblk's u.mStatic is served as
StaticAudioTrackServerProxy does (the client's StaticAudioTrackState queue at 0xbc:
position and loop with their sequences; the position/loop queue back at 0xdc;
mServer; CBLK_LOOP_CYCLE / LOOP_FINAL / BUFFER_END; stop() at once). Loops are
honoured (loop count -1: forever). tests/run_android.sh "SoundPool": aoi.SoundPoolTest
loads a 0.5 s WAV (22.05 kHz) and plays it; $AOI_AUDIO_OUT holds 0.5 s of 440 Hz.

**MediaPlayer plays (app 0.71).** Android's own MediaPlayer service (NuPlayer) runs in
the app's process, with the extractors and the software codecs:
- guest/media.c starts, on a native thread ("aoi-codecs"), what mediaserver's and
  media.extractor's main() do: MediaPlayerService::instantiate() (it registers
  "media.player" over aoi.MediaPlayerService), MediaExtractorFactory::LoadExtractors()
  (MP4, MP3, Ogg, WAV, MKV, FLAC, AAC, AMR, MPEG-2, MIDI), then the codec registrant.
  The libraries are opened in the default namespace (the linker's own
  __loader_android_dlopen_ext); media.stagefright.extractremote=false.
- **Binder relays** (core/binder.c): libbinder calls a local object directly, without
  a parcel round trip, and MediaPlayerService's Client::invoke reads its request from
  the parcel's start, which only a transaction rewinds (prepare() failed with
  NOT_ENOUGH_DATA). "media.player", and every object passed through its calls, is
  handed out as a relay handle: a call becomes BR_TRANSACTION for a looper thread, the
  caller waits for its BC_REPLY, as between processes; objects in the parcels become
  relay handles too (BR_INCREFS/BR_ACQUIRE to the sender before
  BR_TRANSACTION_COMPLETE). Calls that may go into Java go to ART-attached loopers
  only; native threads get relays from servicemanager. Snapshots keep relays (kind 48).
- **BR_SPAWN_LOOPER**: when a pool thread takes the last work and no other waits, the
  driver asks for one more (up to BINDER_SET_MAX_THREADS), as the kernel does. With one
  looper MediaPlayer hung: notify() waited for MediaPlayer's lock while the thread
  holding it waited for getCurrentPosition, queued behind notify.
- **AudioTrack timestamps:** CreateTrackResponse lacked afTrackFlags, so outputId (13)
  was read as flags with DIRECT set and AudioTrack asked IAudioTrack::getTimestamp
  (unsupported). Now the mixer's ExtendedTimestamp (server and kernel position, time)
  is used: AudioTrack.getTimestamp works and NuPlayer's clock moves (the position).
- Stand-ins MediaPlayer waits for: media.extractor, media.codec, media.metrics,
  permission (core/af.c); POSIX timers (timer_create..., never fire: Watchdog).
tests/run_android.sh "MediaPlayer": aoi.MediaPlayerTest plays a 1 s WAV of 440 Hz:
duration 1 s, playing, the position moves; $AOI_AUDIO_OUT holds the tone. "AudioTrack"
now also checks getTimestamp. The IPA gets the extractors, libmediaplayerservice and
its libraries (ios/android-files.txt). Not yet: MediaPlayer's video on a Surface.

**No more crash on MediaCodec; the software codecs (app 0.69, 0.70).**
- Any app that touched MediaCodecList (ExoPlayer, SoundPool loading a sound, a video)
  aborted: Codec2Client CHECKs that hwservicemanager runs. /dev/hwbinder now has one
  (core/binder.c hwmanager: android.hidl.manager@1.2::IServiceManager: get, add,
  getTransport, list*, listManifestByInterface, IBase's ping/interfaceChain/
  interfaceDescriptor; HIDL strings read from the caller's buffers, hidl_vec /
  hidl_string replies as binder_buffer_objects the driver copies next to the reply,
  aoi_pbuffer), hwservicemanager.ready is set and access("/dev/hwbinder") succeeds.
  MediaCodecList now lists no codecs and createDecoderByType fails with
  NAME_NOT_FOUND, which apps handle (aoi.CodecTest).
- **(0.70) Android's own software codecs work, in the app's process:** AAC, MP3, Opus,
  Vorbis, FLAC, AMR, G.711, AVC, HEVC, VP8, VP9, AV1, MPEG-4/H.263 (55 codecs listed).
  What it took, past the registrant below: media.c2.hal.selection=aidl and
  ro.vendor.api_level=202404 (libcodec2_vndk honours the selection from that level
  on: client and store both AIDL; tools/mkprops.py); "media.resource_manager" and
  "package_native" as native stand-ins that MediaCodec waits for (core/af.c);
  /dev/dma_heap/system for the linear buffers (an allocation is a memfd; dma-buf
  sync ioctls succeed; core/proc.c); the registrant on a thread of its own (it does
  not return); "media.player" answers no codec list, so the list is built in the
  process. aoi.Main loads libaoi_media.so on a thread at start (NewPipe's hidden start
  is as quick as before: saved at 40.5 s). tests/run_android.sh "MediaCodec":
  aoi.CodecTest encodes 0.5 s of 440 Hz with c2.android.aac.encoder (21 frames) and
  decodes it with c2.android.aac.decoder: 21504 samples, pitch 440 Hz. The IPA gets
  the APEX's libraries and links (ios/android-files.txt, from a trace of that test)
  and libaoi_media.so. MediaPlayer (the media.player service) is still to do.
- Before that: Android's own software codecs (the swcodec APEX, Codec2) in the app's
  process. guest/media.c (/system/lib64/libaoi_media.so) calls RegisterCodecServices()
  from libmedia_codecserviceregistrant.so, as media.swcodec's main() does; the APEX's
  libraries the system lacks are linked into /system/lib64 and its directory permitted
  to the default namespace (tools/swcodec-ns.py, run by tools/android-setup.sh);
  servicemanager declares android.hardware.media.c2.IComponentStore/software (as a
  device manifest would). On the host the store registers itself ("Software Codec2
  service created and registered"); MediaCodec finding it is next.

**Sound (app 0.68).** AudioFlinger and AudioPolicy are native services now (core/af.c,
the way core/sf.c is SurfaceFlinger), so AudioTrack plays:
- "media.audio_flinger" (IAudioFlingerService) and "media.audio_policy"
  (IAudioPolicyService), codes from the guest's audioflinger-aidl-cpp.so and
  audiopolicy-aidl-cpp.so (tools/aidlcodes.py), parcel layouts from their
  readFromParcel code (CreateTrackRequest/Response, SharedFileRegion,
  AudioChannelLayout, AudioFormatDescription). getOutput, sampleRate, frameCount,
  latency, session ids: one 48 kHz stereo output (io 13).
- createTrack makes an IAudioTrack and a memfd (aoi_proc_memfd) with the
  audio_track_cblk_t and the ring of frames; offsets found in libaudioclient.so
  (mServer 0, mFutex 8, mVolumeLR 0x10, mBufferSizeInFrames 0xa8, mFlags 0xb0, mFront
  0xb8, mRear 0xbc, mFlush 0xc0, mStop 0xc4; frames at 0xe8). The cblk is read and
  written through the app's own mapping of it (found by the memfd's name in p->maps).
- The mixer: aoi_af_tick, from the scheduler (and the idle wait, which now also
  sleeps until the mixer's next tick), takes what a 48 kHz clock has used since the
  last tick from every started track: u8/s16/s24/s32/float, mono to 8 channels (the
  first two), resampled (linear), with the track's volume (mVolumeLR minifloats);
  moves mFront and mServer (the app's position), counts underruns, honours mFlush and
  mStop, and wakes a writer waiting on mFutex. start/stop (plays out to mStop)/pause/
  flush. The mix goes to p->audio: on iOS a 1 s ring and an AudioQueue (paused after
  3 s of silence; the audio session is Playback, mixed with other apps' sound, and
  restarted after an interruption); in aoiproc to $AOI_AUDIO_OUT (raw s16le).
- Snapshots: a restored track cannot go on (its memfd is a copy then): it is marked
  CBLK_INVALID in the app's cblk and answers DEAD_OBJECT, and AudioTrack makes a new
  one (restoreTrack_l), as after an audioserver restart.
- AAudio is told not to use MMAP (aaudio.mmap_policy 1, tools/mkprops.py): its legacy
  path is an AudioTrack.
tests/run_android.sh "AudioTrack": aoi.AudioTrackTest plays 0.5 s of 440 Hz, 44.1 kHz
stereo; all 22050 frames are played (the position), and $AOI_AUDIO_OUT holds 24000
frames of a clean 440 Hz tone at 48 kHz (no discontinuity). Decoders: see 0.70 and 0.71
above.

**English, errors the user sees, disk space, more syscalls (app 0.67).**
- The interface is English, or Turkish when the phone's first language is Turkish
  (L(tr, en) in ios/main.m; CFBundleLocalizations en/tr, InfoPlist.strings for the
  Photos permission text).
- An app that dies by itself (an exit code other than 0, or a fault; not one LiquidAPK
  stopped) gets an alert: how it ended (aoi_android_last_end) and the line of its log
  that says why (FATAL EXCEPTION / Caused by / SIGSEGV), with "Copy log".
- "Not enough memory next to the open app" offers to save and close that app and then
  compile (aoi_android_save_stop, then the compile once its process has ended).
- Disk space: a compile needs the dex's size four times over plus 300 MB free (else
  -3: "Not enough space"); a snapshot is not written with less than 400 MB free, and it
  is flushed, checked (ferror) and fsync'd before it replaces the last one.
- Frames are turned into images only while the app's screen is shown; it is redrawn
  when shown again (aoi_android_redraw). The card's compile-time estimate is kept for
  30 s instead of read from the APK every second. A memory warning saves the open app.
- Syscalls: preadv/pwritev, sendfile, fchdir, wait4/waitid (ECHILD), rt_sigpending,
  inotify_init1/add_watch/rm_watch (watches that never fire). tests/pipes.c step 128.
- Right after 0.66 was built (iOS SDK 26.2), its retain-cycle warning in the task's
  expiration handler is fixed.

**The compile in the background, the iOS way; out of an app by the right edge (app 0.66).**
- A compile (and the save after it) asks iOS for a continued-processing task (iOS 26
  BGContinuedProcessingTask, BGTaskSchedulerPermittedIdentifiers
  "com.heyobi.apkoniphone.compile.*", registered when first needed): iOS shows the
  compile's progress in its own UI and keeps LiquidAPK running in the background until
  the queue and the saves are done. The silent-audio keep-alive stays only as the
  fallback (an older iOS, or a sideloaded copy re-signed under another bundle
  identifier, which no longer matches the permitted identifier).
- The two-finger tap that closed an app is gone (it reached apps that use two fingers);
  a glass drop pulled from the right edge (the mirror of the back gesture) goes to the
  launcher, the app staying alive behind it.

**A code review's fixes (app 0.65).** A read-through of ios/ and core/proc.c found:
- **A stop while an app was still starting was lost** (`running` is set only after its
  libraries, snapshot load and exec): tapping B during A's first seconds left B waiting
  behind A forever. Now a stop then is kept (stop_pending) and taken as the process
  begins; switching apps saves and stops *that* process (aoi_android_save_stop, by
  generation), never the next one.
- **Use after free:** touches, keys, snapshots and the memory watcher read `running`
  while run_guest freed it. They hold it now (proc_get/proc_put under run_lock) and the
  process is freed once none does.
- **The warm handoff:** an app handed to the warm Android that ended before taking it
  (aoi_android_warm returns -3) left the screen spinning and its ScreenVC retained; now
  it is released and reported. A warm start that ended by itself is retried (at most
  three quick ends in a row).
- **The compile queue** stopped at an entry that did not start (nothing to do, or no
  memory), so it never drained (and kept the background audio on): the next is tried.
- **An APK update** rewrote base.apk in place under a running app's mapped pages
  (SIGBUS); now written to a new file and renamed, the write checked, and a compile of
  the old APK cancelled. Package names are checked ([A-Za-z0-9_.], no "..") before they
  name a directory. The picker's copy is deleted.
- **dup3 onto fd 0-2** dup2'd onto the iOS process's own stdout/stderr; the guest gets
  its own host fd now.
- Thermal/power notifications are handled on the main thread; run_guest's environment
  buffer is per call (dex2oat's thread runs it too); "Baştan başlat" deletes the
  snapshot after the process ends; a snapshot that fails to load also drops the clean
  one if it is the same file; .nosnap only when the app cannot be saved (not on a
  timeout: retried at the next launch); removing an app stops its hidden start too.
- The log keeps its last ~300 KB and is written at most every 2 s.
- Syscalls: setrlimit, fadvise64, sync, sync_file_range, syncfs, sched_setparam /
  setscheduler / setaffinity / get_priority_max / min, getcpu, getrusage.
- For App Store readiness: PrivacyInfo.xcprivacy (file timestamps, boot time, disk
  space), UIRequiresFullScreen. Still against review: the silent-audio keep-alive
  (2.5.4; iOS 26's BGContinuedProcessingTask is the sanctioned way) and running code
  the app does not ship (2.5.2).
Host: make test (25 OK), NewPipe hidden save, resume 0.3 s, a stop mid-run, the warm
handoff.

**License, credits, the JIT wording (2026-10-04).** LiquidAPK is GPL-3.0-or-later
(LICENSE); NOTICE.md lists what the IPA carries (AOSP 14 from the pinned GSI, ANGLE,
Khronos headers, certifi's roots, dx), the build and test tools, and the prior work this
project learned from. The IPA's licenses/ gets LICENSE and NOTICE.md, the Android root
its NOTICE.xml.gz (ios/android-files.txt); Geliştirici › Lisanslar shows them. "No JIT"
is spelled out (README): nothing runs as host code generated at run time; ART's own JIT
makes guest code that is interpreted. Outdated text fixed: STATUS's distribution note
(StikDebug), the roadmap, docs/IOS.md's steps, RESEARCH's App Store and license rows.

**A compiled app always opens fast; the compile goes on in the background (app 0.64).**
The 0.62 phone log and what it asked for: WhatsApp opened in a second after its compile
but not always later, not after "Baştan başlat"; the compile stopped when LiquidAPK left
the screen; heat paused a compile that was the phone's only work.
- **The clean snapshot (<dir>.snap0):** the snapshot the hidden start saves after a
  compile is kept, as a hard link (a save writes a new file and renames it, so the two
  names never share a change). When the one the app runs on (.snap) is gone (restart,
  a crash soon after it: dropped as before) or does not fit, the clean one takes its
  place (snap_restore, in aoi_android_snapshot_fits and aoi_android_app): the app opens
  in a second, as it first started. If the app dies soon after resuming the clean one,
  that is dropped too (same inode). Host, NewPipe compiled: hidden start saved, .snap
  removed, launch "resumed from its snapshot in 0.3 s".
- **A LiquidAPK update:** every snapshot is of one build (core/snap.c AOI_SNAP_BUILD);
  aoi_android_snapshot_fits now checks that too (aoi_snap_this_build), so after an
  update the launcher saves every compiled app again, out of sight, one at a time,
  instead of each first tap starting from nothing.
- **Yeniden derle** (card menu): the compiled code, both snapshots and .nosnap go
  (aoi_android_compile_remove removes the snapshots too), then the compile and the save
  run again. Removing an app removes .snap0 and .nosnap as well.
- **In the background:** while a compile, a queued one or the save after it is pending,
  LiquidAPK plays silence mixed with other apps' sound (UIBackgroundModes audio,
  AVAudioPlayer at volume 0), so iOS does not suspend it; a 5 s timer stops it when the
  work is done, and it stops when LiquidAPK comes back. An app that was open is saved
  and, if that worked, ended (it resumes in a second): the compile is the one job. The
  warm Android is not started in the background. A compile iOS still cut short (the
  process ended) is noted (<dir>.compiling) and started again at the next launch, twice
  at most.
- **Heat:** dex2oat pauses only at the critical thermal state, or at serious / Low Power
  Mode while an app is in use on screen next to it.
- **WhatsApp's truncate (errno 38):** truncate(2) by path (nr 45) is implemented; and
  /proc/sys/kernel/random/boot_id reads as a UUID (Uptodown's "ashmem: Failed to read
  boot_id"). tests/pipes.c step 64.
- Kiwi (Chromium) still cannot be saved (its GPU process state): it starts on the warm
  Android, which saves the runtime's start only.

**Compiled, then saved out of sight: the first tap resumes (app 0.62).** The 0.61 phone
log: WhatsApp compiled (verify, 505 s; the estimate said 5 min), Kiwi opened on the warm
Android ("app: on the Android that was already up"), but the first launch after a
compile was still a full start: the snapshot's key has the odex in it, so the old
snapshot no longer fits. Now a finished compile starts the app without a screen
(aoi_android_app_hidden: frames dropped, a watcher thread ends it once aoi.Main has
saved it at idle, or after 4 min), and the card says "Hazır ✓ · anında açılır": the
first tap resumes it (host, NewPipe compiled: saved 39 s after the start, resumed in
0.9 s). A tap while it is still starting shows that same run (aoi_android_show: its
frame target, home callback and a redraw at the next time slice, core/proc.c
redraw_request), so nothing starts twice; another app's tap ends it and it is saved
later. Apps that cannot be saved (a GLSurfaceView keeping its context, Chromium's GPU
state, WebView) get a .nosnap marker and are not tried again until the next compile.
tools/iostest.c AOI_APP_HIDDEN=1 [AOI_APP_SHOW_AFTER=N].
0.63: apps compiled before (0.61, 0.62) are queued too: when the launcher appears, every
compiled app without a snapshot that fits (aoi_android_snapshot_fits: the key of its
APK and odex as they are now) and without .nosnap is saved this way, one at a time.

**Compiled once, with its progress; Android up before the tap (app 0.61).** From the
0.60 phone log and what it asked for ("one build, visible; the 7 s simulator at once"):
- **dex2oat runs once per APK, when asked:** at install (the card shows "Derleniyor %42 ·
  ~3 dk kaldı", a bar, İptal), or from the card's "Derle"; a launch never starts one any
  more (0.54-0.60 did, unseen: a cut-short run started over at every launch). One at a
  time, the rest queued. Progress: instructions run against what the filter takes per MB
  of dex (speed 3.2 G, verify 0.21 G, speed-profile 0.37 G, measured 0.53-0.60), corrected
  by each finished run on the device (Documents/apps/.dex2oat-rates); the time left from
  the recent rate. Big apps (verify) get their hot code compiled only from the menu
  ("Hızlandır"). A tap on an uncompiled app asks: compile first, or open it slow. The
  compiled code can be removed ("Derlemeyi sil"). While it runs the phone does not lock
  (a locked phone suspends us); in the background it goes on for the time iOS gives.
  ios/androidtest.h: aoi_android_compile/_cancel/_info/_state/_remove/_estimate;
  tools/iostest.c AOI_APP_COMPILE=1 compiles first and prints the progress. Host:
  NewPipe 699 s, estimate 7 % short at first (learned: 1.07).
- **The warm process:** LiquidAPK starts Android at once (aoi_android_warm: aoi.Main with
  AOI_WARM, /data = Documents/warm, the bundle's without an APK): the runtime, the
  services that do not depend on the app, the package parser's first run (parsing the
  WebView APK: 4 s otherwise), then it saves itself once (warm.snap: 0.5 s to resume)
  and waits in open("/dev/aoi_warm"). A tap on an app without a snapshot hands it over
  (aoi_android_go): on the guest's thread the process takes the app's /data (p->data),
  log (dup2 onto its stdout/stderr and liblog fds), frames and snapshot path, and goes
  on to parse the APK. Host, NewPipe compiled: 37.9 s cold to idle, 30 s from the tap
  with the warm process (package parsed 0.75 s after it); glapp 24 s -> 11.6 s. Apps with
  a snapshot still resume from it (the warm process is ended for them). The app's
  snapshot taken this way resumes like any other. tools/iostest.c AOI_APP_WARM="dir N".
- **WhatsApp's crash loop:** it died 0.1 s after each resume. Its snapshot had been taken
  just before a crash ("NetworkCallback was not registered"): a snapshot is now dropped
  when the app dies by itself within 30 s of resuming it or 60 s of taking it. The crash
  itself: aoi.ConnectivityService answers requestNetwork/listenForNetwork with a
  NetworkRequest (ConnectivityManager files the callback under it) and onAvailable through
  the caller's Messenger; tests/run_android.sh checks it ("callback true 2").

**Permission requests, links, GL out-buffers at a mapping's end (app 0.60).** From
the 0.59 phone log:
- WhatsApp died 10 s after its snapshot resume: Activity.requestPermissions starts
  "android.content.pm.action.REQUEST_PERMISSIONS" (the system's dialog), which had no
  activity (ActivityNotFoundException). aoi.ActivityTaskManager answers it at once, all
  granted, as the dialog's result (aoi.Activities.result: an ActivityResultItem to the
  activity, so onRequestPermissionsResult runs).
- cube.run: libGDX's DefaultTextureBinder threw "Illegal arguments": its
  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS) read 0. gpu/host.c passes that call 512
  bytes (the longest answer) and copies back what the guest can take, but
  aoi_vm_span returned nothing when any page of the range was unmapped, against its
  own comment: a 64-byte direct buffer near the end of its mapping got no answer
  (the layout differs on the phone's 16 KiB pages, so the host ran fine). It stops at
  the first page it may not touch now (tests/test_vm.c). The same hit gl_str (a shader
  source at a mapping's end read as empty) and a read()/write() whose buffer ends
  before the requested length (EFAULT instead of a short count).
- A web, mail or phone link (ACTION_VIEW with no activity of the app for it, e.g.
  Qalculate's "about" links) goes to iOS: aoi.Clipboard's host channel has op 'u'
  (ios/main.m opens it with UIApplication openURL: Safari).
- The background dex2oat in the app's process writes the same odex, byte for byte, as
  a run on its own (glapp, webapp): no sign that running it beside the app corrupts it.
The test apps check both: webapp asks for CAMERA in onCreate, logs the result and then
opens a link; glapp logs its texture units.

**WhatsApp to its welcome screen; games keep their GL context (app 0.46).** 0.45 on
the phone: no more libart faults. WhatsApp starts (EULA, "Agree and continue"), with
three things in its way, two fixed here:
- Its worker died on ShortcutManager == null, and Kiwi logged "No service published"
  for power and uimode: core/binder.c's servicemanager held 128 services and silently
  dropped the rest of aoi.Services' ~205 stand-ins. Now 512, and a full table is logged.
- "You have a custom ROM installed": tools/mkprops.py now sets a release build's
  identity over the GSI's (ro.build.type=user, ro.debuggable=0, release-keys, "user"
  in the fingerprint, description and display id).
- "An internet connection is required": open. There are no AF_INET sockets yet
  (core/proc.c serves AF_UNIX only), no DNS (bionic asks netd's
  /dev/socket/dnsproxyd) and the "connectivity" stand-in reports no network.
Also "power" is aoi.PowerService now: isInteractive is true (the stand-in said the
screen was off).
cube.run drew spiky geometry after its first snapshot: 0.43 made a GLSurfaceView
that keeps its EGL context on pause (libGDX) drop it for the snapshot, and libGDX
rebuilds only its "managed" meshes after a loss. Such an app is now not snapshotted
at all (/dev/aoi_snapshot_skip: the host stops waiting, the next launch is fresh).

**A fault no longer moves the base register: ART's CMC crashes (app 0.45).**
WhatsApp (at start) and cube.run (after long play) faulted in libart's
Class::FindInstanceField / FindStaticField, called from nterp's field resolution
(the symbols come from libart's .gnu_debugdata). The class came out of a DexCache's
resolved-types array, and its field array pointed at the last 4 bytes of a 1 GiB
linear-alloc pool. Reproduced on the host with a stress dex (scratch, not committed):
8 threads loading 1,500-class dex files through PathClassLoaders while the main
thread runs Runtime.gc(). Under CMC it failed within ~1.5 G instructions, with a
VerifyError naming the wrong class for a field, then the same SIGSEGV; under CC it
passed. The cause: pre/post-indexed loads and stores (ldr/str, ldp/stp, their SIMD
forms) wrote the new base register even when the access faulted. CMC moves pages
away (MREMAP_DONTUNMAP), a touch raises SIGBUS, ART maps the page and the
instruction runs again: from a base already moved, so `ldr w8, [x0], #4` walking a
field array read the wrong words. Now the writeback happens only after the access
succeeds (the SIMD multi-structure forms already did this). tests/signals.c step 16
checks it (fails before the fix). The stress now passes under CMC: 15.8 G
instructions, ~200 MB of host chunks instead of 3.4 GB. difftest WRONG 0.

**Snapshots while the user taps (app 0.44).** 0.43 on the phone: cube.run's
snapshot is saved now (the GPU free 5 and 58 ms after the trim). Qalculate's was
refused once, and the thread dump showed no deadlock: the user was tapping, so HWUI
drew again and made a new context between the trim's last destroy and aoi.Snapshot's
next 50 ms look. Now the host takes the snapshot itself the moment its count of
contexts and surfaces reaches 0 (/dev/aoi_snapshot_gpu_free, asked before the trim;
the GL call that ends it also ends the time slice, so no other thread runs first).
aoi.Snapshot waits on /dev/aoi_snapshot_pending and cancels after 10 s. Host test:
Qalculate tapped every second through the snapshot: saved, drawing on after it.
Also in the 0.43 logs: WhatsApp (at start) and cube.run (0.42, after long play)
fault in libart's Class::FindInstanceField / FindStaticField. The "class" read
there holds no class (its ifields_/sfields_ is 0x4_7ffffffc / 0x4_3ffffffc), which
looks like a stale class reference after a CMC compaction (next).

**A game on the phone's GPU, and its snapshot (app 0.43).** 0.42 on the iPhone:
cube.run (libGDX) runs on ANGLE over Metal ("OGL renderer: ANGLE ... Apple A18 Pro
GPU"), thousands of frames, playable; Qalculate's HWUI on the GPU reads back a frame
in ~3.5 ms. The game's snapshot was refused ("the GPU still holds state"), for two
reasons:
- libGDX keeps its EGL context on pause (setPreserveEGLContextOnPause), so
  aoi.Snapshot now turns that off for its own onPause and back on after onResume.
- A deadlock in core/binder.c, found with a new /dev/aoi_threads (every guest
  thread's state, futex and frame-pointer backtrace to the log; aoi.Snapshot opens
  it when the GPU will not let go). Host one-way calls (SurfaceFlinger's
  onReleaseBuffer) went straight into a looper thread's own queue. A binder thread
  calling SurfaceFlinger synchronously (uncache a buffer, holding libgui's
  BufferCache lock) read it nested while waiting for its reply, and wanted
  BLASTBufferQueue's lock. RenderThread held that lock, disconnecting the window's
  surface in trimMemory, and waited for the BufferCache lock. Now, as in the
  kernel, they wait in a process queue, and a looper takes one only when it reads
  with nothing of its own.
The test app (tools/mktestapk.py) keeps its context on pause as libGDX does: 6 of 6
snapshots saved (GPU free ~56 ms after the trim, was ~1 in 2 hung), and the resumed
process carries on drawing from the frame it was at. Left from the 0.42 logs: no
sound in cube.run (SoundPool wants the native media.extractor), Kiwi stops at
"Illegal meta data value: the child service doesn't exist" (Chromium's child-process
services), WhatsApp faults in libart (0x480000008).

**The GPU crash found (app 0.42).** 0.41's backtrace placed it: libEGL (copying the
GL extension string, called from HWUI's setup) read guest address 0x559e42000, the
string the host had copied into the *previous* app's memory. On the phone the
host's GL state lives across app launches, and aoi_gpu_end (which forgets the cached
strings, contexts and surfaces) was called after aoi_android_run_env's processes but
not after run_guest's, the apps': so the second app with the GPU in a session crashed
(context ids 2, 3 in the logs gave it away), the first worked. Now run_guest ends
with it too. Also "mount" (aoi.StorageService): an empty volume list, so
Context.getExternalFilesDir is null instead of a NullPointerException (cube.run,
libGDX's DefaultAndroidFiles, got this far: libgdx.so loaded, EGL up).

**The iPhone's keyboard in apps (app 0.41; host-tested, the iOS side not yet run).**
aoi.InputMethodManager is now the input method: a focused window's editor starts
input there (startInputOrWindowGainedFocus: its EditorInfo and the app's own
RemoteInputConnectionImpl), showSoftInput/hideSoftInput show and hide the host's
keyboard (/dev/aoi_ime/1|0 -> p->ime), and what is typed comes back as input records
(aoi_proc_key: 6 a character, 7 backspace, 8 return, 9 closed) that go to the
editor's InputConnection on the main thread: commitText, the selection or one code
point deleted, performEditorAction for a single-line field with an action, else
KEYCODE_ENTER. No IME session exists, so the InputConnection is called directly
(in-process). Window focus, which asking for the keyboard needs: a touch gives it,
2 s after a tap it goes again unless the keyboard or a popup is up (a cursor blinks
in a focused window: repaints), a long press keeps it for selection as before,
closing the keyboard takes it. iOS: the app's screen is a UIKeyInput (no
autocorrection), first responder while the app wants the keyboard, with a "Kapat"
bar. Host check (aoiproc AOI_TAPS "keys:TEXT", ~ backspace, | return): the test
APK's EditText takes "hi", backspace, "ab" -> "hab", then "x" -> "habx"; Qalculate
never asks for the keyboard (its taps work, the copy toolbar still stays). Snapshots
are only prepared for when the host wants one (/dev/aoi_snapshot_wanted).

**GL games' path: GLSurfaceView (host, app 0.40).** tools/mktestapk.py builds a test
APK without the Android SDK (java/testapp: a binary AndroidManifest.xml written by
the script, classes.dex by javac + dx against java/testapp/stubs): aoi.glapp, a plain
Activity whose GLSurfaceView clears to a stepping color and scissors a white square,
as a game's render thread would. It runs on the host GPU (HWUI on, AOI_HWUI=1): the
frames reach the screen in the right place. What it took:
- a SurfaceView has layers of its own (a container and, below it, the BLAST layer
  its buffers go to), children of the window's, placed by SurfaceView in transactions
  we do not parse. aoi.WindowSession puts a pre-draw listener on each window and
  tells core/sf.c where each SurfaceView's BLAST layer is: the window's place plus
  the view's, just below the window (whose transparent hole shows it) or above it
  for setZOrderOnTop. Window z values step by 4 to leave room.
- core/sf.c composes onto a black screen as large as the layers reach (the bottom
  layer no longer sets it: a SurfaceView's is smaller than the screen), converting a
  layer whose RGBA/BGRA order differs from the screen's.
- "input" (aoi.InputService): the virtual keyboard device (-1, empty key map).
  KeyCharacterMap.load asks for it when a plain Activity's action bar prepares its
  menu; without it the app died (UnavailableException).
- snapshots: aoi.Snapshot pauses every GLSurfaceView (onPause: its EGL context and
  surface go) with HWUI's trim, and resumes them after; a process resumed from the
  snapshot makes a new context and the renderer carries on (host: saved in 1.2 s,
  resumed in 0.3 s, frames 480, 510, ... drawn on).
Run: python3 tools/mktestapk.py glapp.apk, install it as DATA/app/apk/base.apk with
the classpath and aoi.dex of an app data dir, aoiproc ... aoi.Main.

**Qalculate on the phone's GPU (app 0.38).** With 0.38 Qalculate ran on ANGLE/Metal
from start to finish: HWUI's EGL setup, Skia's first GL calls, frames drawn and read
back (804x1556: 9.5 ms for the first frame, then 3.6-3.7 ms on average per frame,
3.3 ms with popups), the snapshot taken after the GL state was dropped (destroy
surface/context, saved in 0.67 s) and HWUI coming back afterwards, popups and the
text toolbar (their own window surfaces), back to the launcher. 0.37's crash
(SIGSEGV at 0x53b626008, libc copy from libc++) did not come back; the 0.38 run has
the logging to place it if it does. For games: libstdc++.so (libgdx needs it: cube.run
stopped there), libOpenSLES.so and libaaudio.so with their dependencies are in the
bundle (0.39).

**The GPU on the phone, first try (app 0.37 -> 0.38).** On the iPhone, Qalculate,
WhatsApp's EULA and Kiwi all got as far as HWUI's EGL setup on ANGLE (configs chosen;
ANGLE has no 1010102 config, a warning) and then died the same way: SIGSEGV at
0x53b626008 on the RenderThread, pc in libc (+0x61990, a copy), lr in libc++
(+0xa3c60), right after the context came up. The host (Mesa) does not do it. 0.38 logs
what is needed to find it: on iOS every guest EGL operation and each GL function's
first call with its arguments and result (glGetString's text too; AOI_GL_FIRST=1 on
the host), and a native fault now logs x0-x2 and the callers along the frame-pointer
chain. Also: libjnigraphics.so in the bundle (Kiwi's libchrome.so needs it).

**The GPU on the phone: ANGLE on Metal (app 0.37, not yet run on the phone).** The
iOS build links gpu/host.c with ANGLE's static libraries (Godot's godot-angle-static
release for iOS arm64, chromium/7578, BSD-3 + MIT; licence texts in the app's
licenses/) and the Khronos headers, pinned in .github/workflows/ios.yml (gpu/angle_ios.cpp
adds what those libraries leave out on iOS: two system_utils functions and a stand-in
for the astc-encoder API); the guest's
driver libGLES_aoi.so goes into the bundle's /vendor/lib64/egl. gpu/host.c asks ANGLE
for its Metal display (EGL_PLATFORM_ANGLE_ANGLE). If it comes up, ios/androidtest.c
gives apps the GPU (p->gpu, aoi_hle_gpu, AOI_HWUI=1): HWUI draws with GLES on Metal and
each frame comes back into the window's buffer. The log says it: "I/aoi-gpu: EGL ...,
N configs: ..." at the start and every 100 frames "frame N: WxH, readback X ms on
average" (the cost of the trip back, to be measured). The host's GL objects are freed
when the process ends (aoi_gpu_end); the GPU is part of the snapshot key.

**Snapshots with the GPU.** Host GL state cannot go into a snapshot, so core/snap.c
is not asked while the process holds host contexts or surfaces (p->gpu_live; a save
then logs "the GPU holds its state"). aoi.Snapshot does what Android does when an app
goes to the background: WindowManagerGlobal.trimMemory(TRIM_MEMORY_COMPLETE)
destroys the windows' renderers and RenderThread's EGL context; it waits for
/dev/aoi_gpu_live to stop failing with EBUSY, opens /dev/aoi_snapshot, and has every
window draw again (ViewRootImpl brings its renderer back). A process resumed from the
snapshot carries on from there and draws again the same way; the host's EGL comes
up lazily in the new process. The iOS app's own snapshot on going to the background
(aoi_android_snapshot) sends input action 5, and aoi.Snapshot does the same. Host,
Qalculate with AOI_HWUI=1: the snapshot saved after the trim (74 MB, 1.7 s), then
resumed (0.7 s), redrawn by HWUI on the GPU, and taps computed 7x87x84+5 correctly.

**Qalculate drawn by HWUI on the GPU (host).** With AOI_HWUI=1 aoi.Main leaves
ThreadedRenderer on, and HWUI's SkiaGL pipeline runs on our driver: Qalculate's
window is an EGL window surface, each frame Skia renders with GLES on the host GPU
(Mesa here) and eglSwapBuffers copies into the window's buffer. Same picture as the
software path, taps work. Guest instructions per frame: ~2-3 M (a tap's first frame
~20 M) where software rendering took ~65 M: the pixels are no longer the guest's job.
What HWUI needed on the way:
- extensions: the guest sees only the host's extensions that add enums/shader
  features or whose functions are core GLES 3.2 under an EXT/OES/KHR/NV/APPLE name
  (gpu/host.c's list; glGetString, glGetStringi and GL_NUM_EXTENSIONS agree), and
  eglGetProcAddress maps those names to the core stubs (glDiscardFramebufferEXT ->
  glInvalidateFramebuffer). Skia's GrGLInterface validates every advertised one.
- core/sf.c: ISurfaceComposer getStaticDisplayInfo (12), getDynamicDisplayInfoFromId
  (13, Android 14's layout read off libgui's readFromParcel: one 60 Hz mode, sRGB
  only) and getCompositionPreference (34): HWUI aborted "Failed to acquire physical
  displays for WCG support!" without them.
- core/hle.c's no-GPU stand-ins for HardwareRenderer.nSetSurface/nCopySurfaceInto
  stay out when the guest has a GPU (aoi_hle_gpu): HWUI and the text Magnifier get
  real surfaces.
AOI_HWUI stays off by default on the host (aoiproc); the iOS app turns it on when
ANGLE's display comes up (0.37).

**OpenGL ES for the guest, first light (host).** A GPU driver of our own, so apps
and games can draw with OpenGL ES and HWUI can leave software rendering:
- guest/gles.c is /vendor/lib64/egl/libGLES_aoi.so, which Android's own libEGL loads
  for ro.hardware.egl=aoi (tools/mkprops.py; tools/android-setup.sh installs it).
  Freestanding like mapper.aoi.so. Each of the 358 GLES 3.2 functions
  (gpu/gles32.api, from Khronos' gl32.h) is a stub generated by tools/glgen.py that
  makes the private syscall AOI_SYS_GL (core/gpu.h) with its number and arguments.
  Its EGL keeps configs/contexts/surfaces as host numbers; every surface is a host
  pbuffer, and a window surface's eglSwapBuffers dequeues the window's buffer, has
  the host write the frame into it (AOI_EGL_READBACK, top row first, RGBA/BGRA/565)
  and queues it.
- gpu/host.c runs the calls on the host's EGL/GLES (Mesa's llvmpipe here, ANGLE on
  Metal on the phone next), the host context following the calling guest thread.
  Pointers: guest memory is contiguous only within 2 MiB chunks, so each pointer
  argument has a length rule (tools/glgen.py: counts, image sizes from the pixel
  store state, strings, buffer offsets while a buffer is bound); one inside a chunk
  is used in place, one across chunks is copied (and back). By hand: client-side
  vertex arrays (copied at each draw, index range from the indices), mapped
  buffers (a guest copy written back on flush/unmap), strings the GL returns
  (copied into guest memory once), sync objects, string arrays.
- aoiproc gets it when pkg-config finds egl and glesv2 (Makefile GPU=1); the iOS
  app does not link it yet, so there AOI_SYS_GL is ENOSYS and nothing changes.
aoi.GlesTest (run_android.sh, 15 checks): EGL14/GLES20 from Java, a 64x64 pbuffer,
a red triangle over a blue clear from a client-side array and from a buffer object,
glReadPixels: right pixels both ways, on "OpenGL ES 3.2 Mesa ... llvmpipe".
Next: window surfaces end to end (a GLSurfaceView), HWUI on the GPU for Qalculate
(ThreadedRenderer on, EGLImage/hardware buffers), ANGLE in the iOS app.

**Kiwi's Play-services check, app data dirs, SoundPool (app 0.36).** 0.35 on the
phone: Kiwi went through its alias, ChromeTabbedActivity and FirstRunActivity, then
died on GooglePlayServicesMissingManifestValueException: the <application>
<meta-data> (com.google.android.gms.version) was not in our ApplicationInfo. aoi.Main
now sets ai.metaData from PackageParser's mAppMetaData and creates the app's data dirs
(/data/data/PKG, /data/user_de/0/PKG; Kiwi's SharedPreferences got ENOENT).
cube.run: libsoundpool.so was not in ios/android-files.txt (its dependencies were);
it is now, and aoi.SoundPoolTest (run_android.sh, 14 checks) builds and releases a
SoundPool against aoi.AudioService (trackPlayer/releasePlayer get default answers).
cube.run will next need OpenGL ES. WhatsApp: the new fault line places its SIGSEGV in
libart.so (mirror::Class::FindInstanceField, address 0x480000008): a bad class
reference while resolving a field, right after a JIT compile on its EULA screen;
not reproduced on the host (no APK here).

**Kiwi's alias, WhatsApp's finishAffinity, native faults in the log (app 0.35).** On
the phone 0.34 brought Qalculate back (copy/paste toolbar and handles stay up) and
Kiwi got as far as starting its browser activity, then died: its launcher
com.google.android.apps.chrome.Main is an activity-alias, Kiwi starts it again by
that name, and our startActivity swapped the alias for its target's ActivityInfo
while the intent still named the alias, so ActivityThread looked for a class
"...chrome.Main". The alias now stays itself (ActivityThread runs targetActivity, as
for the launcher) and an implicit intent gets the resolved component. WhatsApp
reached its EULA activity (Main -> EULA, then finishActivityAffinity, now
implemented: the activity and those below it go) and died of a native SIGSEGV
(fb-breakpad) that the log did not place. A SIGSEGV whose pc is in a .so now goes to
the app's log with pc and lr as library+offset (the first 40; ART's implicit null
checks fault in .oat code and are left out). ART's JIT checked on the host:
a hot-loop dex (arithmetic, doubles, floats, arrays, strings, virtual calls,
exceptions, HashMap) gives the same output with -Xusejit:false and with
-Xjitthreshold:50, so JIT code is not the suspect.

**0.32 killed every app on the phone; activities on a stack (app 0.34).** 0.32's
stand-in "connectivity" service gave ActivityThread a ConnectivityManager, and
handleBindApplication's getDefaultProxy loads libframework-connectivity-jni.so
(NetworkUtils); the phone's bundle (ios/android-files.txt, the files traced from
earlier runs) did not have it, so every app died in bindApplication. The host's full
root has it, which is why the host runs passed. It is in the list now; Qalculate
starts on a root made by tools/mini-root.sh from that list (plus
/vendor/lib64/hw/mapper.aoi.so, which the workflow adds). Run an app on the mini root
before shipping a change that adds services or libraries.
Kiwi's launcher activity is a trampoline that starts the browser's activity
(IActivityTaskManager.startActivity was missing). aoi.Activities now keeps the app's
task: startActivity resolves the app's own activity (component, or an intent filter's
action; an activity-alias stands for its target) and launches it on top (the one
below pauses, stops, its window hides); another app's intent gets
START_INTENT_NOT_RESOLVED (ActivityNotFoundException in the app). finish() of the top
activity resumes the one below and destroys it; back on an activity above the root
asks it to finish (IRequestFinishCallback), on the root it leaves the app as before.
isTaskRoot/isTopOfTask answer from the stack, and the host's back goes to the top
activity. Host: a second MainActivity started over Qalculate's first, back finished
it, the first came back and took taps, back on it went home. Kiwi and WhatsApp still
to be tried on the phone (no APKs on the host); startActivityForResult results are not
returned yet.

**Text selection stays up: Cut / Copy / Paste / Select all (app 0.33).** On the
phone a long press in Qalculate's input brought the copy/paste toolbar up for a
moment, then it went. Compose shows a text field's selection handles and toolbar
only in a focused window (and hides the toolbar in an unfocused one), and our windows
never had focus: a focused field's cursor blinks, a full repaint twice a second.
Now WindowSession.focus gives an app window focus (its ViewRootImpl's
windowFocusChanged, in this process) only while text is being selected: when a
finger is held still for 0.8 s (aoi.Input: a long press is coming) or a text toolbar
(type 1005) comes up, and takes it back on a touch that goes to the window itself or
when its last popup (toolbar, handles) goes. Two more fixes the selection needed:
- a touch outside a FLAG_NOT_FOCUSABLE popup goes to the window below, as in
  InputDispatcher (a selection handle was swallowing the tap meant for "Copy");
- android.widget.Magnifier (the loupe over a dragged handle, Compose and TextView)
  renders with a HardwareRenderer of its own, and its first GL call aborted the app:
  there is no OpenGL ES implementation. core/hle.c makes HardwareRenderer.nSetSurface
  and nCopySurfaceInto return at once (libhwui offsets, checked by hash), so that
  renderer never gets a surface: it skips its frames, the loupe stays invisible.
Host (Qalculate, fresh start; aoiproc's held taps now move a pixel every 0.1 s, as a
finger does): long press -> two handles and Cut / Copy / Select all, still up 7 s
later; Copy -> host clipboard "87"; long press again -> Paste is offered; Paste
replaces the selection with the host's "12345"; afterwards the window lets go of
focus and the frames stop. The long-press timeout is still 5 s (core settings,
app 0.14). run_android.sh 13/13, difftest WRONG 0.

**No more one-at-a-time service crashes (app 0.32).** Each new app or gesture was
finding a service we lacked (clipboard, audio: null managers) or a call our services
had not written (AbstractMethodError: Kiwi's startActivity, IUserManager...). Now
aoi.Services covers both kinds:
- every Context *_SERVICE name we do not provide gets a NullService at startup (206
  of them), which answers any call with "no exception" and zeros/false/null/empty, so
  a manager is never null; kept absent on purpose: textclassification (falls back to
  a local classifier), autofill and content_capture (they wait 5 s for a reply);
- our own services take their calls as transactions (queryLocalInterface null, so
  the framework goes through Stub.Proxy as across processes) and catch
  AbstractMethodError in onTransact: an unwritten call gets the default answer and
  one log line, "aoi: missing <Service>: <signature>". WindowSession keeps direct
  calls (per-frame relayout/finishDrawing).
tools/appcheck.sh turns an app's log into its checklist: crashes, missing calls,
stand-in services used, services still absent, missing native libraries. Qalculate
from a fresh start: no crash, 2 missing calls (handleApplicationWtf,
queryIntentContentProviders), 3 stand-ins used (accessibility, connectivity,
network_management); dialog, menu and delete still work; run_android.sh 13/13.

**Copy and Paste no longer kill the app (app 0.31).** On the phone, Qalculate's
text toolbar (PopupWindow type 1005) came up, and tapping Copy or Paste killed the
app: View.performClick plays the click sound, ViewRootImpl asks AudioManager
areNavigationRepeatSoundEffectsEnabled, and with no "audio" service its IAudioService
is null (NullPointerException). aoi.AudioService answers the calls views and
AudioManager make (sound effects off, normal ringer/mode, stream volumes); no sound
comes out yet.

**The clipboard is the iPhone's (app 0.30).** Text an Android app copies goes to
UIPasteboard, and its paste reads UIPasteboard, so copying works both ways between
iOS apps and Android ones. aoi.Clipboard writes or reads the text in
/data/local/tmp/aoi.clip and opens /dev/aoi_clip/s (set), /g (get) or /h (has text)
for the host (proc.h, p->clip; ios/main.m clipboard_cb; aoiproc's AOI_CLIP buffer).
"Has text" uses hasStrings, which does not read: only a real paste reads the
pasteboard, so iOS shows its "Allow Paste" question only then (Settings > LiquidAPK
> Paste from Other Apps: Allow silences it). An app's own clip (non-text items too)
is kept while the pasteboard still holds its text. tests/run_android.sh checks it
(aoi.ClipboardTest: the host's text pastes in, a copy comes out).

**Clipboard (app 0.29).** Qalculate died on a touch on the phone: its copy action
asks for the "clipboard" service, ClipboardManager's constructor threw
ServiceNotFoundException, getSystemService answered null and the app's own code
dereferenced it (NullPointerException in b5.a). aoi.Clipboard now serves
IClipboard in the process (set/get/clear/has, listeners); not yet the iPhone's
pasteboard. IUserManager.getApplicationRestrictionsForUser answers an empty Bundle
(Kiwi's background task died on it). aoiproc's AOI_TAPS takes "x,y,ms" for a long
press.

**The snapshot's skip, made to work on iOS (app 0.28).** 0.27's lines settled it: a
Qalculate resumed from its snapshot stays at ~405 MB, flat, while a fresh start jumps
from 377 MB to 2.3 GB right when its first snapshot is saved, and closing an app (which
saves one) took the footprint to 2.9 GB. And mincore() reported all 3.5 GB of the
guest's chunks as present while the footprint was 377 MB: Darwin sets
MINCORE_ANONYMOUS (0x80) on every page of anonymous memory, untouched ones too, so
0.26's "skip pages mincore says do not exist" never skipped anything on the phone.
The test is now vec & AOI_MINCORE_EXISTS (0x7f: resident, referenced, modified,
paged out (compressed), copied), in the save and in the memory lines, which also give
how much of the chunks carries each mincore bit, to check this on the phone.

**Where the phone's memory goes (app 0.27).** 0.26 still showed a fresh Qalculate
at 2.4 GB footprint (1.8 GB internal) where the host's RSS for the same run is 412 MB,
and it stayed there: the snapshot's reads were not (all of) it. The memory lines now
also give malloc's bytes in use (Apple) and the guest's share: the host pages of its
chunks that exist (mincore: resident or compressed), those in chunks with no file
mapped (anonymous), the guest bytes mapped, and the six 64 MB guest windows holding
the most, named by their largest mapping. On the host: 258 MB (179 anonymous) of
14.9 GB mapped.

**Snapshots no longer read untouched memory (app 0.26).** With 0.25 the footprint no
longer grew while Qalculate was used (826 MB, flat), but a fresh start went from 421 MB
to 2585 MB within 10 s, and back to ~800 MB half a minute later, where the host's peak
for the same start is 412 MB. The jump came with the first snapshot: save_memory read
every mapped guest page to see whether it was zero, and ART maps ~3 GB it never
touches. On Linux such a read maps the shared zero page; XNU has none for anonymous
memory, so every read allocated a page (and the phone slowed while the compressor
caught up). The save now asks mincore() which host pages of a chunk exist: a page
neither resident nor compressed (MINCORE_PAGED_OUT) was never written and is skipped,
as it reads as zero. Only on Apple hosts (Linux reports swapped-out anonymous pages
the same way; AOI_SNAP_MINCORE=1 forces it for a host test), and only in chunks no
file was mapped into (vm.filemap, set by aoi_vm_map_file: a file page not yet read is
not resident either). Host check: the snapshot is the same size (78.8 MB), its save
took 0.9 s instead of 1.6 s, and it resumes and takes taps.

**Windows on top of windows; the memory leak (app 0.25).** Qalculate's menus (the
row's ⋮, a Compose DropdownMenu: "Pop-Up Window", type 1002) and dialogs (EXACT's
"Approximation mode", type 2) turned the screen black: every window got a full-screen
frame and layer, and sf.c showed whichever buffer came last. Now aoi.WindowSession
lays each window out as WindowManager would (MATCH_PARENT fills the screen, otherwise
Gravity.apply with its x/y, kept on screen), keeps one layer per window (its buffer
size includes surfaceInsets, the shadow) and tells sf.c where it goes by opening
/dev/aoi_layer/ID/X/Y/Z/DIM (ENOENT, like /dev/aoi_home; .../ID/hide when the window
goes away). sf.c keeps a buffer per layer (each buffer is matched to the layer_state
record it sits in: our layer handle and id start each record), and composes them
bottom to top, premultiplied source over, a FLAG_DIM_BEHIND window darkening what is
below by its dimAmount. aoi.Input sends a gesture to the window it went down in, or to
a touch-modal one above it (so a touch outside a menu or dialog closes it), in that
window's coordinates, and ACTION_OUTSIDE to windows watching outside touches. Checked
on the host from a snapshot: the dialog is centred over a dimmed screen and closes on
an outside tap or a choice; the menu opens below its ⋮ with its shadow, and "Delete"
deletes the row. The phone's footprint growth (+150 MB per 10 s, internal memory) was
our frame callback: it runs on the emulator's thread for the whole session, so each
frame's autoreleased UIImage (5 MB) waited for a pool that never drained. The
callbacks now drain their own @autoreleasepool.

**Other APKs, memory (app 0.24).** Kiwi Browser (Chromium) died in
Context.createWindowContext: IWindowManager.attachWindowContextToDisplayArea was
missing (AbstractMethodError); aoi.WindowManager now answers it (and the display-content
and window-token forms) with display 0's configuration. cube.run dies loading
libsoundpool.so (not in the bundle; it needs GL anyway). On the phone a Qalculate
process resumed from its snapshot showed a 2.1 GB footprint where the host has 364 MB
RSS (file pages: 61 MB mapped, 272 MB copied at load): the app now logs, every 10 s
while an app runs, phys_footprint with internal/compressed/external/resident, the guest's
host chunks and the file pages mapped/copied (aoi_vm_mapped_bytes/copied_bytes), to
find where it goes.

**Name: LiquidAPK (app 0.21).** The home-screen name and the launcher's title; links are
liquidapk://open?app=<package> (aoi:// still opens). The bundle id stays
com.heyobi.apkoniphone, so an update keeps the installed apps and their data.
(liquidapk.com and .app had no DNS records when chosen; registration not checked.)

**Java heap (app 0.20).** A Qalculate session on the phone ended after 40 s with exit
137 (the app's KillApplicationHandler: kill(SIGKILL) after an uncaught exception) and
"Clamp target GC heap from 30MB to 16MB" in the log: without dalvik.vm.heap* properties
AndroidRuntime gives ART -Xmx16m. tools/mkprops.py now sets heapgrowthlimit 128m and
heapsize 256m, as a phone's vendor partition does. (Also setting heapstartsize,
heapminfree, heapmaxfree and heaptargetutilization makes the CMC GC's compaction walk
past its space through our userfaultfd: SIGSEGV at the space's end, 1.4 GB touched;
left at ART's defaults until that is understood.) Each app's previous log is kept
(<package>.log.1, also in "Logu kopyala"), so a crash's stack survives the relaunch.

**A launcher (app 0.19).** ios/main.m is now a launcher of installed APKs: glass cards
(iOS 26's UIGlassEffect, looked up at run time; a blur material before), "APK ekle",
and a developer page with the old tests and logs. Each app lives in Documents/apps/
<package> (its /data; <package>.snap, .key, .log, .plist with its label; 0.17's
Documents/adata moves there); `aoi_apk_manifest` (core/apk.c) reads the package and
label from the binary AndroidManifest.xml. One process at a time: opening another app
snapshots and ends the running one. Each app opens with aoi://open?app=<package> (the
Info.plist URL scheme), from a Shortcuts home-screen icon (the tile's menu copies the
link, saves the app's icon to Photos and opens Shortcuts) or the app icon's quick
actions (the first four apps). The app's screen fills the safe area: the host writes
"width height dpi" (safe area in points x2, 320 dpi; 804 x 1556 on an iPhone 16 Pro) to
/data/local/tmp/aoi.display, aoi.DisplayManager reads it, the snapshot key includes it.
Back is a glass drop pulled from the left edge (stretches, a haptic once it would go
back). Back on the app's root activity (or finish) opens "/dev/aoi_home" -> p->home ->
the launcher comes up; the process stays for the next open. App icon: an Android head
on an iPhone, ios/icons (CFBundleIconFiles).

**Skia's hot code natively (core/hle.c, app 0.18).** Software rendering spends its
frames in a few libhwui.so functions: the highp raster pipeline's stages (seed_shader,
matrix_2x3, a 2-stop gradient, clamps, dither, load_8888_dst, dstin, store_8888,
just_return) and the loop that runs them over 4-pixel chunks, plus rect_memset32,
blit_row_color32 and blit_row_s32a_opaque. A br/blr/bl/b/b.cond into libhwui's range
asks `aoi_hle_run` whether the target is one of them (offsets of the bundled
libhwui.so; each checked by an FNV hash of its code before use, so another libhwui
just runs interpreted); the stand-in does what the instructions do in the same order
(fused where the code has fmla, no contraction elsewhere, ARM min/max on zeros, the
NEON rounding of x/255) and declines (interpreted) on NaNs, a non-default FPCR or
memory that is not plain. `AOI_HLE_CHECK=n` runs every n-th stand-in call against the
interpreter too (registers and pixels compared): 371,132 checked over Qalculate taps
and its drawer, 0 differ.
The dither stage was also checked against Unicorn. libhwui's base comes from its
executable mapping (sys_mmap with PROT_EXEC; an earlier non-exec mapping of the file
gave a wrong one). Host, Qalculate taps from a snapshot: 37 -> 109 M guest
instructions/s equivalent, the first frame after a tap 1.8 -> 0.7 s; callgrind 199 ->
78 host instructions per guest instruction. What is left: scudo malloc/free (~10 %),
text and other blits, ART.

**Usable app (0.17):** the iOS app starts the installed APK by itself (from its
snapshot: about a second); picking an APK installs and starts it. A swipe from the
left edge is Android's back: `aoi_android_back` -> record 3 on /dev/aoi_input ->
aoi.Input calls the resumed activity's onBackPressed on the main thread (found through
ActivityThread.mActivities). Not a KEYCODE_BACK event: key events need window focus,
and focus makes text cursors blink, a full repaint twice a second. A touch the host
takes back (iOS cancels it for a gesture) is ACTION_CANCEL (record 4). Host check:
Qalculate menu -> Settings, then `AOI_TAPS=...;back` returns to the calculator. On the
phone v0.16 gives ~0.5 s per frame after a tap (Choreographer skips ~31, was ~70).

**Interpreter 1.6x faster while drawing (app 0.16).** Measured from a Qalculate
snapshot with taps (`AOI_SNAPSHOT_LOAD` + `AOI_TAPS`; callgrind over 60 M guest
instructions): 327 -> 199 host instructions per guest instruction; 24 -> 37 M guest
instructions/s on the host. What did it: a decode cache (instruction word -> its branch
of cpu.c's chain, via labels as values; SIMD words -> their class decoder), no memcpy
calls in guest loads/stores and lane access, fast paths for the forms Skia's raster
pipeline runs (4 x float fadd/fsub/fmul/fmax/fmin, bitwise ops, fmla by element,
int<->float conversions, shifts), fetch from the current code page. difftest WRONG 0.
Where a tap's time goes now: 86 % libhwui, mostly Skia's highp raster pipeline (4
pixels per stage call, float). Every frame repaints ~800 K pixels (store_8888 runs
199,394 times per frame, whatever changed): Compose in software mode invalidates the
whole view. A tap gives 3-4 frames of ~65 M instructions. Next: the raster pipeline's
hot stages natively on the host (the system image is fixed, so their addresses are
known), and/or fewer pixels.

On the phone, app 0.15: a launch from the snapshot takes 0.1 s, with the history kept.

**Snapshots: the second launch resumes in under a second (host), app 0.15.**
`core/snap.c` saves the whole guest process to a file and loads it back: struct
aoi_proc as is (host pointers and fds fixed up), guest memory page by page (a page that
equals its file mapping is mapped from the file again, zero pages are skipped, the rest
is stored: ~80 MB for Qalculate), epoll lists and file offsets, host pipes and socket
pairs rebuilt from their pair ids, binder/SurfaceFlinger/gralloc state (`*_snap`; native
binder objects travel as (kind, index)), and the apps' files under /data/data (put back
on load, so SQLite's cache and WAL index match the files). The guest's monotonic clock
carries on (`aoi_mono_offset`). Every vsync connection gets one vsync after a load (an
event in flight is lost). Taken when aoi.Main's activityIdle + 3 s opens
`/dev/aoi_snapshot` (once per fresh start), and again by the iOS app when it goes to
the background (`aoi_android_snapshot`, so history typed since is kept). Loaded only
for the same build (inside the file) and the same APK + odex (`<datadir>.snap.key`);
otherwise the app starts afresh. Host: save 1.7-1.8 s, load 0.3-0.9 s, then taps and
frames as before (`AOI_SNAPSHOT_SAVE` / `AOI_SNAPSHOT_LOAD` in aoiproc; iostest's
`AOI_APP_SNAPSHOT=1` snapshots after its taps). Not yet run on the phone.

**On the phone with v0.14.60:** a frame after a tap ~1.1 s (Choreographer skips ~68,
was ~125); the first full draw ~5.8 s (348, was 513); the second launch reuses the
compiled odex and /data (no dex2oat).

**On the phone with v0.14.57:** dex2oat compiles Qalculate once in 167 s (12.7 G
instructions, 196 MB peak); then a frame after a tap takes ~2 s (Choreographer skips
~125 frames; was ~360 = 6 s with 0.13.49). "112+113" = 225 and "112+1123" = 1235.

**First run on the iPhone (v0.13.48, iPhone 16 Pro):** the app's process starts, binds
the application and runs its content providers in 13 s (1.04 G instructions), 588 MB
peak; then createDisplayEventConnection fails: Darwin has no SOCK_SEQPACKET. Message
pairs (vsync's BitTube, InputChannel's socketpair) now fall back to SOCK_DGRAM with
256 KiB buffers (`aoi_host_msgpair`; `AOI_NO_SEQPACKET=1` takes that path on Linux).

**Past layout, toward the first frame.** The window configuration now carries the
screen bounds (they were 0 x 0: Qalculate's keypad grid computed negative cell sizes);
the activity metrics are logged at resume. New, not yet seen working end to end:
- `core/gralloc.c`: the allocator AIDL service `IAllocator/default` (V2; codes from the
  guest's allocator-V2-ndk.so: allocate2 = 2, isSupported = 3, suffix = 4). Buffers are
  guest memory mapped by the host plus a metadata page; the native_handle is a
  placeholder fd and 13 ints (`core/gralloc.h`). servicemanager's isDeclared now says
  yes for registered names.
- `guest/mapper.c` -> `build/mapper.aoi.so`: a freestanding AIMapper v5 (raw syscalls,
  a private syscall for buffer references), installed by tools/android-setup.sh in
  /vendor/lib64/hw (the sphal namespace).
- `core/sf.c`: the legacy `SurfaceFlinger` (android.ui.ISurfaceComposer) service;
  setTransactionState (code 8, one-way) finds the queued buffer (flattened 'GB01', or
  the client's buffer-cache id) and keeps it as the frame on screen; `AOI_SF_DUMP=x.ppm`
  writes it out. Also getLatestVsyncEventData and getMaxAcquiredBufferCount.

**Next, in order:**
1. gralloc: a native allocator service (`android.hardware.graphics.allocator.IAllocator/
   default`) and a guest mapper library (`mapper.aoi.so`, stable-C AIMapper v5) over
   host-shared memory; SurfaceFlinger's transactions; the first frame on the iPhone.
2. Decide whether the phone gets the full BCP + boot image + CMC (bundle size: the
   framework jars, oat and vdex files; a device test of CMC), since real APKs need
   framework classes.
3. fork/execve/wait4 for mksh pipelines (roadmap step 2).

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
- `core/snap.c` — snapshots of a whole process (save/load), for fast relaunch.
- `core/hle.c` — native stand-ins for hot guest functions (Skia's raster pipeline).
- `tools/fetch-android.sh`, `tools/android-root.sh` — the AOSP 14 guest root.
- `core/load.c` — static-ELF loader + initial stack (for the `aoirun` path).
- `tools/gmpdemo.c` — the end-to-end demo: APK library → linked → GMP computes.

## The bigger roadmap

1. **(done)** interpret a real native lib from an APK.
2. **(done)** NEON/FP + atomics: every instruction word of Qalculate's libraries and of
   the rendering libraries runs, Unicorn-checked.
3. **(done)** Android's own linker64 + bionic, ART, the framework in the app process
   with native services; gralloc, SurfaceFlinger, HWUI on the GPU, touch, keyboard,
   network, WebView, snapshots, dex2oat on the phone.
4. **(open)** Sound (an AudioFlinger of our own), web content in Chromium browsers,
   Google Play services stand-ins, notifications, camera.
5. Speed: the WebKit JIT route (docs/RESEARCH.md, "Route"); the interpreter stays the
   path that runs everywhere.

## Distribution note

Sideloaded (SideStore / AltStore / Sideloadly), no JIT and no debugger: the StikDebug
route was dropped after 0.3 (docs/RESEARCH.md, "Route"). Everything runs in the
interpreter, so nothing technical needs JIT; what stands between this and the App Store
is review policy (running code the app does not ship, guideline 2.5.2; the silent-audio
background keep-alive, 2.5.4; naming other platforms in metadata) and the GPL. License:
GPL-3.0-or-later (LICENSE, NOTICE.md).
