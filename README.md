# LiquidAPK (apk-on-iphone)

**Android apps on a non-jailbroken iPhone, running on the phone itself.** Not streaming,
not a VM image: the APK's code runs on the iPhone in a no-JIT AArch64 interpreter, with
Android's own runtime (ART), framework and libraries from AOSP 14 bundled in the app, and
the Linux kernel, binder and Android's system services provided in-process, as Wine does
for Windows programs.

It is a normal sideloaded app (SideStore / AltStore / Sideloadly): no JIT, no debugger,
no jailbreak. "No JIT" means the iPhone's CPU never runs code generated at run time: there
is no writable-executable memory. Android's own ART may still JIT-compile hot Java
methods inside the guest, but what it produces is AArch64 guest code that the
interpreter runs like any other.

## What runs today (app 0.72)

Tested on an iPhone 16 Pro (iOS 27) and, for every change, on the host
first (`build/iostest`, the same code path as the phone):

| App | State |
|---|---|
| Qalculate! (Compose, native GMP) | runs, computes, touch and keyboard; exchange rates over TLS (ECB's certificate fix in 0.56, not yet confirmed on the phone) |
| cube.run (libGDX, OpenGL ES) | runs on the iPhone's GPU (ANGLE on Metal); its SoundPool effects play since 0.72 (host-tested) |
| WhatsApp | starts, EULA, on to its registration screen |
| NewPipe | main UI and navigation |
| Molly (Signal fork), Element (Matrix) | start to their first screens |
| WebView apps (e.g. Uptodown) | Android's WebView 119 renders pages and runs JavaScript |
| Kiwi / Cromite (Chromium browsers) | browser UI and New Tab page; web page content not shown yet |

What an app gets: windows drawn by HWUI on the GPU (OpenGL ES through ANGLE on Metal),
touch, the iOS keyboard, clipboard, back gesture, sound (AudioTrack, SoundPool, MediaPlayer, and
MediaCodec with Android's software codecs: AAC, MP3, Opus, Vorbis, H.264, VP9...), network (TCP/UDP, DNS, TLS with
today's root store), storage, AndroidKeyStore, WebView, home-screen links per app, and a
snapshot of the running app so the next launch resumes in about a second.

Not yet: video on screen (decoding works; MediaPlayer's picture is not drawn yet),
**web content in Chromium browsers**, Google Play services, camera, notifications. `docs/STATUS.md` has the full,
dated record of what works and what is open.

## Speed

Everything runs interpreted, about 80-90 M guest instructions/s on an iPhone 16 Pro.
What makes apps usable is that ART does not have to interpret the app's dex code inside
our interpreter: the app is compiled ahead of time with Android's own `dex2oat`, in the
background, while the app already runs.

Measured on the host (`build/iostest`, NewPipe, cold start without a snapshot, to the
first activity being idle):

| | time |
|---|---:|
| uncompiled (first launch) | 43.5 s |
| compiled with dex2oat `speed` | 25.5 s |
| resumed from its snapshot | ~1 s |

How long compiling takes, on the host (the phone is in the same range: WhatsApp's
`verify` took 249 s there):

| App (dex size) | Filter | Time | Peak memory |
|---|---|---:|---:|
| Qalculate (2.9 MB) | speed | ~100 s on the phone | |
| NewPipe (11 MB) | speed | 547 s | 744 MB (with the app) |
| Cromite (11 MB) | speed | 657 s | 983 MB (with the app) |
| Molly (59 MB) | verify, then speed-profile | 169 s + 323 s | 313 MB |
| Element (68 MB) | verify | 229 s | |
| WhatsApp (86 MB) | verify | 249 s on the phone | 409 MB |

An app is compiled once, right after it is added: its card shows how far it is and how
long it still takes, with a cancel button (and "Derle" for one that is not compiled).
Apps with up to 16 MB of dex are compiled fully (`speed`). Bigger ones are verified;
their hot code can then be compiled from the app's profile (`speed-profile`, "Hızlandır"
in the card's menu): `speed` would take 40-50 minutes and over 900 MB. The compile runs
on a low-priority thread and goes on when LiquidAPK is in the background (as an iOS 26
continued-processing task: iOS shows its progress); it pauses only while the phone is very hot, or hot (or
in Low Power Mode) while an app is in use next to it. When it is done, the app is
started once out of sight and saved, so its first tap resumes it in about a second. That
clean snapshot is kept: after "Baştan başlat", a crash or a LiquidAPK update the app
still opens in about a second (an update saves it again once). "Yeniden derle" in the
card's menu compiles it again. Chromium browsers (Kiwi, Cromite) cannot be saved yet.

When LiquidAPK opens, Android itself starts at once in the background and waits; a tap
on an app then only loads the app (about 8 s less: NewPipe 38 s -> 30 s on the host).

## Install

1. GitHub → **Releases** → the latest `v0.x.N` → `ApkOnIphone.ipa` (every push to `main`
   builds and publishes it).
2. Install it with SideStore, AltStore or Sideloadly; they sign it with your Apple ID.
3. Open **LiquidAPK** → **Add APK** → pick an `.apk` from Files. Tap its card to run it;
   swipe in from the left edge for Android's back, from the right edge to go back to
   LiquidAPK; hold a card for its menu. **Developer › Copy log** copies the log for a bug
   report. The interface is in English, or Turkish on a Turkish phone.

More in `docs/IOS.md`.

## How it works

```
APK (dex + arm64 .so) ── ART (AOT code from dex2oat, else its interpreter)
        │                         │
        │                Android framework (framework.jar, HWUI, libgui …)
        │                         │
        │      our in-process system services (java/src/aoi: PackageManager,
        │      ActivityManager, WindowManager, Keystore, WebView provider …)
        │                         │
  bionic libc, linker64 ──────────┘
        │
  core/: AArch64 interpreter (cpu.c, simd.c) · Linux syscalls (proc.c) · binder and
         servicemanager (binder.c) · SurfaceFlinger (sf.c) · gralloc · snapshots (snap.c)
        │
  gpu/: guest OpenGL ES → ANGLE on Metal       ios/: UIKit app, launcher, input, keyboard
```

- **CPU:** an AArch64 interpreter (base, SIMD/FP, atomics, CRC, crypto as apps need them),
  checked instruction by instruction against Unicorn (`make difftest`). iOS forbids
  writable-executable memory without a debugger, so there is no JIT.
- **Kernel:** Linux syscalls on Darwin: threads (green threads on one host thread),
  futex, mmap with a sparse 128 GiB guest address space, epoll, eventfd, timerfd, memfd,
  sockets, signals, userfaultfd for ART's GC.
- **Android:** the AOSP 14 GSI's own binaries (`linker64`, ART, `framework.jar`,
  `libhwui` …), unmodified. No `system_server`: the services an app talks to are written
  in Java and run in the app's process (`java/src/aoi`); the rest answer with defaults.
- **Graphics:** the guest's `libGLES_aoi.so` (`guest/gles.c`) forwards OpenGL ES calls to
  the host (ANGLE on Metal on the phone, Mesa on Linux for tests).

## Repository

| | |
|---|---|
| `core/` | interpreter, syscalls, binder, SurfaceFlinger, snapshots |
| `gpu/`, `guest/` | OpenGL ES bridge (host side, guest driver) |
| `java/src/aoi/` | the in-process Android system services |
| `ios/` | the iOS app (launcher, app runner, background compile) |
| `tools/` | host tools: `aoiproc`, `iostest`, root building (`fetch-android.sh`, `android-root.sh`), `app-install.sh` |
| `tests/` | `make test`, `tests/run_android.sh` (20 checks on a real Android root), `make difftest` |
| `docs/` | `STATUS.md` (what works, dated), `RESEARCH.md` (route and decisions), `IOS.md`, `MEASUREMENTS.md` |

Build and test on Linux: `make test`; with an Android root
(`tools/fetch-android.sh ~/aroot`, ~800 MB download): `AOI_ANDROID_ROOT=~/aroot sh tests/run_android.sh`.
Run an app like the phone does: `tools/app-install.sh ROOT app.apk DIR`, then
`AOI_ANDROID_ROOT=ROOT AOI_APP_DATA=DIR ./build/iostest` (`build/iostest-gpu` for the GPU path).

No Android or app binaries are committed: the root is built from the pinned AOSP GSI by
the workflow and by `tools/fetch-android.sh`.

## License and credits

LiquidAPK is free software under the **GNU General Public License v3.0 or later**
(`LICENSE`). The IPA also carries AOSP 14 (mostly Apache-2.0), ANGLE (BSD-3-Clause) and
the other components listed, with their sources, licenses and the projects this one
learned from, in [`NOTICE.md`](NOTICE.md); in the app: Geliştirici › Lisanslar.
Android is a trademark of Google LLC; this project is not affiliated with Google or Apple.
