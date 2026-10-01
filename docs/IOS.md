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
   needed): the largest PROT_NONE reservation iOS grants, whether its far end is
   usable, and core/vm.c's real 64 GiB guest space with an 8 GiB scudo-style
   reservation. Android programs (core/proc.c) need that; a "blocker" line means the
   guest space must shrink or the app needs the extended-virtual-addressing entitlement.
5. **Logu kopyala** to share the log; it is also in Files → On My iPhone → APK on iPhone →
   `log.txt`.

First device result (iPhone 16 Pro, iOS 27.0.1): 20000! (77,338 digits, 28 M guest
instructions) in 0.33 s, about 85 M instructions/s.

Native execution through StikDebug was tried in versions 0.1–0.3 and dropped (see
`docs/RESEARCH.md`, "Route").
