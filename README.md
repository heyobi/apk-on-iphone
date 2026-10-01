# apk-on-iphone

**Run Android apps natively on a non-jailbroken iPhone.** Not streaming, not a VM: the APK's own
ARM64 code runs on the iPhone's CPU, the way Wine runs Windows programs on Linux.

> Status: idea. Nothing here runs yet.

## Why it might be possible now

- **No CPU translation.** Android apps and their `.so` libraries are ARM64, like the iPhone.
  Madeira (Windows games on iPhone) spends most of its effort translating x86; here that
  cost is zero for native code.
- **JIT is available.** StikDebug attaches a debugger and unlocks JIT on iOS 17-27, which
  ART (Android's Java runtime) needs for usable speed. Without JIT, ART's interpreter
  still works, only slower.
- **The pieces exist as open source:** AOSP's ART and bionic, and the Madeira/Wine
  playbook for doing all of this inside one iOS process.

## Prior art (checked 2026-10-01)

- Cloud / remote: Redfinger, BrowserStack, Parsec to a PC emulator — streaming, not native.
- UTM on iOS: full Android VM, needs JIT, very slow; guides like leiting2327/run-apk-on-ios.
- Cycada (Columbia, 2010s): research compatibility layer for **iOS apps on Android** — the
  opposite direction, but the closest design reference.
- ib-2-3-android: iOS apps (UE3) on Android — again the opposite direction.
- No project found that runs APKs natively on an iPhone.

## Status

- **`gmpdemo`** (`make build/gmpdemo`): loads the **real `libgmp.so` from the Qalculate APK**,
  links it (34 libc imports bound to a small host shim in `core/bionic.c`), and computes a
  factorial with GMP's own code. `__gmpz_init` and `__gmpz_fac_ui` run — the factorial itself
  executes, millions of real GMP instructions through the interpreter. The final decimal
  formatting (`__gmpz_get_str`) faulted; the interpreter bugs behind it are fixed (see
  `docs/STATUS.md`) and the demo needs a re-run.
- **`difftest`** (`make test` / `make difftest`): checks the interpreter instruction by
  instruction against Unicorn (QEMU's A64 core) with random encodings and state.
- **`aoirun`** (host tool, `make test`): a no-JIT AArch64 interpreter + a small Linux/aarch64
  syscall layer + an ELF loader. It runs a real static `aarch64-linux` ELF and produces correct
  output and exit code, at both `-O0` (loops and branches actually execute) and `-O1`. This is
  **milestone 1**, and it runs on any host — no iPhone, no JIT — which also makes it the basis
  of the future App-Store-safe path (the same interpreter compiled to Wasm). Coverage of the
  A64 base set grows as real code needs it; an unimplemented instruction stops visibly rather
  than running wrong.
- **`apkscan`** (host tool, `make test`): reads the arm64-v8a `.so` files of an APK and counts
  the instructions that cannot run unmodified on iOS — Linux `svc #0` syscalls, `tpidr_el0`
  thread-pointer access, and x18 shadow-call-stack pushes/pops. These are exactly the sites
  the loader will have to rewrite or trap. Run `tools/apkscan.py app.apk`.

## Distribution

This targets **sideloading** (SideStore / AltStore / Sideloadly) with JIT through StikDebug,
like Madeira. The App Store only allows WebKit's own JIT, so an App Store build would have
to run everything as WebAssembly inside `WKWebView`: fine for Java/Kotlin apps (ART compiled
to Wasm), slow for native `.so` code (an ARM64 interpreter or translator in Wasm). The design
keeps the syscall layer and framework independent of how code executes, so that path stays
open.

## Architecture sketch

```
APK ──► ART (dex → interpreter / JIT) ──► Android framework (Java)
                    │                              │
               bionic libc  ◄── native .so ──►  libandroid, EGL/GLES, AAudio
                    │
        Linux syscall layer (futex, mmap, epoll, binder…)  ← the "Wine" part
                    │
                 iOS (Darwin) · Metal · AVAudio · UIKit surface
```

1. **Syscall layer:** a user-space Linux ABI on Darwin — the hard core of the project.
   Binder can be emulated in-process (all "processes" are threads of one iOS app,
   exactly like Madeira's in-process wineserver).
2. **Graphics:** GLES → Metal via ANGLE (ANGLE already has a Metal backend).
3. **Framework:** a trimmed AOSP `system_server` running in-process; SurfaceFlinger
   replaced by a single `CAMetalLayer`.

## Milestones

1. A static ARM64 Linux "hello world" (bionic) runs inside an iOS app.
2. `dalvikvm` runs a `.dex` that prints to the log.
3. A pure-Java APK draws a `View` on screen.
4. A GLES game (NDK) renders a frame.
5. The demo video: a real Play Store game on an iPhone.

## Risks

Size of the Android framework, Google Play Services (most apps need them; microG is the
open replacement), Apple's sideloading limits (7-day signing, JIT only with a debugger).
