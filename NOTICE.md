# LiquidAPK — license and third-party notices

LiquidAPK (this repository: `core/`, `gpu/`, `guest/`, `ios/`, `java/`, `tools/`,
`tests/`, `docs/`) is Copyright (C) 2026 İbrahim Polat and contributors.

It is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version. It is distributed in
the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See `LICENSE` for the
full text (SPDX: `GPL-3.0-or-later`).

LiquidAPK is not affiliated with or endorsed by Google, Apple, or the makers of the
Android apps it is tested with. Android is a trademark of Google LLC; iPhone and iOS
are trademarks of Apple Inc. Users run APKs they are entitled to use.

## Shipped inside the iOS app (the IPA)

These are not part of this repository; the build fetches them (`.github/workflows/ios.yml`,
`tools/fetch-android.sh`) and keeps their own licenses.

| Component | Where it comes from | License |
|---|---|---|
| Android Open Source Project 14 userspace (bionic, linker64, ART and its boot image, `framework.jar`, HWUI, Skia, libgui, ICU, Conscrypt/BoringSSL, toybox, mksh, fonts, the AOSP WebView, …) | AOSP arm64 GSI built by [ponces/treble_aosp](https://github.com/ponces/treble_aosp) v2024.08.16 "vanilla" (no Google apps), files listed in `ios/android-files.txt` | Mostly Apache-2.0; each component's notice is in the bundled `aroot/system/etc/NOTICE.xml.gz` (BSD, MIT, ISC, OFL-1.1 for the Noto fonts, Unicode for ICU, MirOS for mksh, 0BSD for toybox, …) |
| ANGLE (OpenGL ES on Metal) | [chromium/angle](https://chromium.googlesource.com/angle/angle), static libraries built by [godotengine/godot-angle-static](https://github.com/godotengine/godot-angle-static) | BSD-3-Clause (ANGLE), MIT (build scripts); texts in the app's `licenses/` |
| Khronos EGL / OpenGL ES headers | [KhronosGroup/EGL-Registry](https://github.com/KhronosGroup/EGL-Registry), [OpenGL-Registry](https://github.com/KhronosGroup/OpenGL-Registry) | Apache-2.0 / MIT |
| Extra root certificates (`tools/cacerts-extra.pem`) | Mozilla's CA store via [certifi](https://github.com/certifi/python-certifi) 2026.06.17 | MPL-2.0 |
| `dx` (turns `java/src` into `aoi.dex`, build time) | AOSP dalvik-dx 14.0.0_r21, [Maven repackaging](https://repo1.maven.org/maven2/com/jakewharton/android/repackaged/dalvik-dx/) | Apache-2.0 |

The Android apps a user adds are theirs, not part of LiquidAPK, and are not
distributed with it.

## Tools used to build and test (not shipped)

| Tool | Use | License |
|---|---|---|
| [Unicorn](https://www.unicorn-engine.org/) | reference CPU for `make difftest` / `isacheck` (every instruction is checked against it) | GPL-2.0 |
| [LLVM / Clang / lld](https://llvm.org/) | builds the host tools, the guest test programs and the iOS app | Apache-2.0 WITH LLVM-exception |
| [Mesa](https://mesa3d.org/) | the host's OpenGL ES for GPU tests (`build/iostest-gpu`) | MIT |
| e2fsprogs `debugfs`, xz, Python 3, OpenJDK `javac` | reading the GSI image, scripts, Java services | GPL-2.0 / public domain & LGPL / PSF / GPL-2.0 with Classpath exception |
| Xcode SDK on GitHub Actions | the iOS build | Apple's terms |
| SideStore, AltStore, Sideloadly | installing the IPA on a phone | their own |

## Prior work this project learned from

No code was copied from these; their designs and documents shaped ours
(`docs/RESEARCH.md` has the details and the dated research).

- [AIM](https://github.com/hahnlee/aim) (Apache-2.0) — running the original Android
  userspace on an in-process Linux syscall layer (its ADR 0012), and native system
  services in the app's process instead of booting Android (ADR 0013).
- [Android Translation Layer](https://gitlab.com/android_translation_layer/android_translation_layer)
  (GPL-3.0-or-later) and [art_standalone](https://gitlab.com/android_translation_layer/art_standalone).
- [StikDebug](https://github.com/StikDebug/StikDebug) — JIT on iOS through a debugger
  (tried in app 0.1–0.3, dropped).
- [LiveContainer](https://github.com/LiveContainer/LiveContainer), Apple's
  [XNU sources](https://github.com/apple-oss-distributions/xnu), Android's
  [source.android.com](https://source.android.com/) documentation.
