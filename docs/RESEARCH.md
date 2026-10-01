# Feasibility research (2026-10-01)

Four web-research passes, done before building the next layers so we see blockers early.
Marked findings were checked in sources by the researcher (repos, docs, commit logs);
everything under "inferred" is judgement. Sources at the end of each section.

## Verdict table

| Layer | Verdict | Why |
|---|---|---|
| CPU, interpreter (no JIT) | **green** (done for 2 apps) | `build/isacheck`: 0 missing / 0 wrong on Qalculate + cube.run. Works everywhere, 10–50× slower than native. |
| Native `.so` via JIT, iOS 17.4–18.x | **green** | StikDebug sets CS_DEBUGGED, then MAP_JIT / dual mapping. |
| Native `.so` via JIT, iOS 26 on A15+/M2+ (TXM) | **yellow** | Debugger must stay attached; one large code region authorized at launch (cannot grow later); RX + separate RW alias; per-app StikDebug script. Confirmed working by Play! and iCube on iOS 26.4–26.6. |
| AOT: convert `.so` to signed Mach-O dylibs at install | **yellow/red** | On-device signing is proven (LiveContainer + ZSign), but no ELF→Mach-O converter exists. Large job; the only native path without JIT. |
| bionic for app `.so` files | **green** | Qalculate + cube.run native libs import only 235 external symbols, all with Darwin equivalents. ATL's `bionic_translation` wrappers are a reference. |
| ART (dex runtime), interpreter-only | **yellow** | Feasible; see "ART" below. Main issues: the low-4 GiB heap, no userfaultfd, Linux-isms. |
| ART with JIT | **yellow→red** | Needs TXM pre-registered code cache + heap-base codegen patches; sideload-with-debugger only. |
| Android framework (`android.*`) | **yellow** | Nothing ships it for iOS. ATL's Java `api-impl` is largely reusable, its C/GTK4 layer must be rewritten for UIKit. |
| GLES game → ANGLE → Metal | **green** | ANGLE-Metal: ES 2.0 + 3.0 complete (not 3.1), EGL 1.5, takes a `CALayer` as native window. Shipped by Safari for WebGL. |
| Compose / HWUI drawing | **yellow** | libhwui has a host (darwin) build without GPU pipelines; or own Canvas/RenderNode on Skia-Metal (Compose Multiplatform iOS precedent). Fidelity is the risk. |
| Audio, input | **green** | AudioTrack/SoundPool/MediaPlayer → AVAudioEngine/AudioUnit/AVPlayer; UITouch → MotionEvent maps 1:1. |
| App Store | **red** | Guideline 4.7 allows retro/PC emulators, but nothing with JIT passes review (also on AltStore PAL). Sideload is the target. |

## Two Darwin facts that change the design

1. **TPIDR_EL0 is not ours on iOS.** XNU stores the CPU number in TPIDR_EL0 and rewrites
   it on every context switch / return to user (`osfmk/arm64/cswitch.s`, `locore.s`).
   Darwin's TLS is TPIDRRO_EL0 (read-only). So in the native backend every
   `mrs xN, tpidr_el0` must be rewritten at load time — exactly the sites `apkscan`
   already counts (699 in Qalculate, 40 in cube.run). The interpreter is unaffected
   (guest register). x18 is also reserved on Darwin (matters only for ShadowCallStack code;
   none in either app).
2. **No mappings below 4 GiB.** ART stores object references as 32-bit absolute
   addresses, so its heap must live under 4 GiB; Darwin arm64 cannot map there (Apple DTS,
   2025). Fix used by AIM: heap window at a base address, references as offsets — touches
   runtime, nterp, entrypoints, codegen. **If ART runs as guest code in our interpreter, we
   own the guest address space and need no patch** (inferred).

## ART

- **art_standalone** (ATL): ART rebased on Android 10, host-built with a Makefile, glibc;
  JIT and dex2oat boot images on Linux x86_64/aarch64. Android 10 predates the
  userfaultfd GC. Builds core-oj, core-libart, okhttp, bouncycastle, apache-xml, wolfssljni.
- **AIM** (github.com/hahnlee/aim, Apache-2.0, active Sept 2026) — **closest prior art to
  this project.** Runs Android 16 ART on Apple Silicon macOS. First as a native Mach-O
  port (184 patches), now by running the **original android-arm64 ELF `libart` on an
  in-process Linux syscall layer** — our architecture — with 8 patches (heap base).
  Also: futex over `__ulock`, memfd/ashmem emulation, SIGBUS→SIGSEGV, CC GC with
  read barriers instead of userfaultfd, booting without an image first.
  Read `docs/art-exception-patches.md` and `docs/adr/0012-original-android-userspace.md`.
- No boot image: `ClassLinker::InitWithoutImage` works, slower startup. Oat files are tied
  to the runtime build: generate our own boot image with our dex2oat.
- SIGSEGV handlers work in iOS apps (Xamarin uses them for null checks); with a debugger
  attached, EXC_BAD_ACCESS reaches it first, so the StikDebug script must pass faults back.
- Alternatives (RoboVM/MobiVM, J2ObjC, dex2jar): all need build-time compilation, cannot
  run an unmodified APK's dex. Old Dalvik: portable but too old for modern dex.

## Framework: Android Translation Layer (ATL)

- gitlab.com/android_translation_layer, GPL-3.0+, ~1,280 commits, active (Sept 2026).
- Java reimplementation of `android.*` (`api-impl`) + C over GTK4 (`api-impl-jni`) +
  `libandroid` (ANativeWindow, AAsset, looper, input). Resources via AOSP libandroidfw.
  Canvas via GskSnapshot, text via Pango, EGL wrapped, audio over ALSA.
- Runs to some degree: Angry Birds, Worms 2, Unity games, NewPipe, WhatsApp, K-9, OsmAnd;
  **libGDX APIs added 2025-04**; Compose partly (RenderNode 2026-02); own GMS stubs.
- Linux-only (Wayland/X11 headers, glib everywhere). No macOS/iOS port exists.
- Port = keep `api-impl` + libandroidfw, write a UIKit/Metal backend for the JNI layer.

## Licensing

ATL is GPL-3.0+. Fine for sideloading if this project is GPL-3 too. App Store
distribution of other people's GPL code is contested (App Store terms vs GPL §10, VLC 2011),
but the App Store is closed to this project anyway (JIT / 4.7).

## AIM up close (cloned 2026-10-01, commit e6c6f03)

Decision taken: **route B** (sideload, native code via JIT, best compatibility), built on
AIM rather than ATL. What the code and docs say:

- Architecture (`ARCHITECTURE.md`, ADR 0012): the original Android 16 arm64 userspace runs
  unmodified; `svc #0`, x18 and TPIDR_EL0 sites are **rewritten once per file into a cache**
  (`linux-translate`) — the same fix our `apkscan` sites need. Syscall layer, binder as a
  library, a versioned host-call ABI for HALs, GLES driver forwarding to ANGLE. Rust
  (~490 files), Apache-2.0.
- **As shipped it does not fit an iPhone:** every Android process is a Darwin process;
  fork is done Cygwin-style with `posix_spawn` + Mach memory-entry snapshots
  (`docs/fork.md`). iOS apps cannot spawn processes. Measured on an M2 Pro
  (`docs/perf-baseline.md`): **72 processes / 4.4 GB RSS at boot, 126 / ~8 GB idle,
  ~40-50 s boot.**
- **ADR 0013 (accepted 2026-09-29) points exactly where an iPhone needs it:** keep only the
  app's own process original (ART + boot image, framework.jar, bionic, linker, the app's
  .so files) and replace the system services one by one with native implementations of
  their AIDL interfaces, registered under the original names, "until no Android system has
  to boot" — "Wine is fast because it does not run Windows". `crates/aim-services` already
  has ~21k lines of these (clipboard, notifications, location, settings, statusbar, …).

So the iPhone design is: **one iOS process = the original app process (AIM ADR 0013 style)
+ native services as in-process threads + binder in-process.** No zygote, no SystemServer,
no SurfaceFlinger boot. ATL's GPL `api-impl` is not needed on this route.

## Route (decided 2026-10-01)

Native execution via StikDebug was tried on the target phone (v0.1–0.3 of the test
app: the iOS 27 handshake worked, executing did not) and **dropped by decision**: it
needs a debugger at every launch and rules out the App Store. The route is:

1. **Interpreter** (now): any iPhone, no special permissions. Measured on an iPhone 16 Pro
   (iOS 27.0.1): ~85 M guest instructions/s.
2. **WebKit JIT** (later, for speed): translate guest code to WebAssembly and let
   WKWebView compile it — the only JIT iOS grants to ordinary apps.

Everything below that assumes native JIT is superseded by this.

## What this means for the plan

1. **Execution model per platform:** interpreter everywhere (works on any iOS, slow);
   native via JIT on StikDebug devices, with a code region reserved at launch (TXM);
   tpidr_el0 rewriting in the native loader.
2. **ART: run the original android-arm64 libart as guest code** on our CPU + Linux
   syscall layer (AIM's ADR-0012 route). It reuses everything built so far, avoids the
   4 GiB patch in the interpreter, and is the same binary later run natively under JIT.
   Cost: dex code is interpreted by an interpreted ART → slow until the JIT backend exists.
3. **Framework: the original framework.jar in the app process, native services**
   (AIM ADR 0013), single iOS process; first target the libGDX game (GLSurfaceView +
   ANGLE), Compose later. ATL's `api-impl` only as a fallback.
4. **Next concrete step:** get a stock arm64 `libart.so` + `dalvikvm` (AOSP / AIM's build)
   and see how far it gets on `aoirun` — the same "measure first" loop that worked for
   the CPU: run it, list the missing syscalls and libc symbols, add them.

## Sources

- iOS / JIT: github.com/StikDebug/StikDebug · github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md ·
  github.com/jpd002/Play--CodeGen/pull/34 · github.com/Provenance-Emu/iCube/issues/11 ·
  github.com/willfaust/Madeira/blob/main/ARCHITECTURE_ANALYSIS.md · github.com/LiveContainer/LiveContainer ·
  github.com/apple-oss-distributions/xnu · github.com/SideStore/SideStore/issues/1616 ·
  macrumors.com/2024/08/01/app-store-review-guidelines-pc-emulator-apps/
- ART: gitlab.com/android_translation_layer/art_standalone · github.com/hahnlee/aim ·
  developer.apple.com/forums/thread/781966 · source.android.com/docs/setup/start/requirements
- ATL: gitlab.com/android_translation_layer/android_translation_layer ·
  gitlab.com/android_translation_layer/bionic_translation · hackaday.com/2025/09/10/a-look-at-not-an-android-emulator/ ·
  github.com/minecraft-linux/mcpelauncher-manifest
- Graphics/audio: github.com/google/angle (README, src/libANGLE/renderer/metal/SurfaceMtl.mm) ·
  webkit.org/blog/11989/new-webkit-features-in-safari-15/ ·
  github.com/aosp-mirror/platform_frameworks_base/blob/main/libs/hwui/Android.bp ·
  github.com/mono/SkiaSharp/issues/4891 · github.com/kstenerud/ObjectAL-for-iPhone
