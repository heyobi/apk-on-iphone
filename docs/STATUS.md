# Status and handoff (2026-10-02)

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
3. **(in progress, last part)** Android's own linker64 + bionic, ART, the framework in
   the app process with native services — done up to a resumed activity; now the
   UI surface: gralloc buffers, SurfaceFlinger frames, then the frame on the iPhone,
   touch and the keyboard.
4. Speed: the WebKit JIT route (docs/RESEARCH.md, "Route"); the interpreter stays the
   path that runs everywhere.

## Distribution note

Sideload only (SideStore/AltStore) with JIT via StikDebug. App Store allows only
WebKit's JIT, so an App Store build must run everything as Wasm in WKWebView —
which is exactly why the interpreter (backend #1) matters: it is that path's seed.
