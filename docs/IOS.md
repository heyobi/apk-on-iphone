# Trying it on an iPhone

The test app (`ios/`) runs the real `libgmp.so` from an APK in the interpreter: no JIT,
no debugger, nothing beyond a normal sideloaded app.

1. **Get the IPA:** GitHub → **Releases** → latest `v0.x.N` → `ApkOnIphone.ipa`
   (built and published by Actions on every push to main). Branch builds are only under
   Actions → **iOS IPA** → artifact **ApkOnIphone-ipa**.
2. **Install** with SideStore, AltStore or Sideloadly (they sign it with your Apple ID).
3. **Run:** put the Qalculate APK in Files → **APK seç** → **Çalıştır**. The log shows
   n!, its digit count, the time and the interpreter speed (guest instructions per second).
   "Çalıştır" runs three rounds and reports the best (the phone throttles when warm:
   the same build measured 81 then 75 M/s on consecutive runs).
4. At launch the app also runs an **address-space probe** (`ios/vmprobe.c`, no APK
   needed): the largest contiguous reservation iOS grants (6 GiB on an iPhone 16 Pro,
   iOS 27), and core/vm.c's sparse 64 GiB guest space with an 8 GiB scudo-style
   reservation, which Android programs (core/proc.c) need. The last line must read
   "fit on this device".
5. **Android** (no APK needed) runs step 1 on the phone: Android's own `linker64`
   loads `toybox` and `mksh` from the AOSP 14 files bundled in the app (`aroot/`,
   9 MB, list in `ios/android-files.txt`, put there by the workflow) and they run in
   the interpreter through `core/proc.c`. Expected: "merhaba, ben Android toybox",
   "mksh: 42", "7 harf" and a listing of /system/lib64, each with `exit 0`; then
   **ART**: `dalvikvm64` runs a hello-world dex ("Merhaba from ART", ~85 M
   instructions). The bundled root is ~73 MB (list in `ios/android-files.txt`).
6. **Logu kopyala** to share the log; it is also in Files → On My iPhone → APK on iPhone →
   `log.txt`.

First device result (iPhone 16 Pro, iOS 27.0.1): 20000! (77,338 digits, 28 M guest
instructions) in 0.33 s, about 85 M instructions/s.

Native execution through StikDebug was tried in versions 0.1–0.3 and dropped (see
`docs/RESEARCH.md`, "Route").
