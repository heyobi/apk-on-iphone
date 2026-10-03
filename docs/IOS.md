# Trying it on an iPhone

The test app (`ios/`) runs the real `libgmp.so` from an APK in the interpreter: no JIT,
no debugger, nothing beyond a normal sideloaded app.

1. **Get the IPA:** GitHub → **Releases** → latest `v0.x.N` → `ApkOnIphone.ipa`
   (built and published by Actions on every push to main).
2. **Install** with SideStore, AltStore or Sideloadly (they sign it with your Apple ID).
3. **Add an app:** LiquidAPK → **APK ekle** → pick an `.apk` in Files. It is installed
   into the app's container (its native libraries extracted, a data directory of its
   own) and gets a card.
4. **Run it:** tap the card. The first launch starts at once, uncompiled; dex2oat
   compiles the app in the background (paused while the phone is hot or in Low Power
   Mode), and later launches use the compiled code. Once the app is idle a snapshot is
   taken: the next launch resumes from it in about a second. Swipe from the left edge
   for Android's back; the iOS keyboard opens for text fields.
5. **Cards:** hold one for **Ana ekrana ekle** (a home-screen link through Shortcuts),
   **Baştan başlat** (drop the snapshot) or **Kaldır**.
6. **Logu kopyala** copies the log (with the end of the app's own `app.log`) for a bug
   report; the logs are also in Files → On My iPhone → LiquidAPK.
7. **Geliştirici** has the older checks: the address-space probe, Android's toybox and
   mksh, ART hello-world and GC, and GMP n! from an APK's `libgmp.so`.

The interpreter runs about 80-90 M guest instructions/s on an iPhone 16 Pro; the
bundled Android root (framework, boot image, fonts, WebView) makes the IPA ~240 MB.

First device result (iPhone 16 Pro, iOS 27.0.1): 20000! (77,338 digits, 28 M guest
instructions) in 0.33 s, about 85 M instructions/s.

Native execution through StikDebug was tried in versions 0.1–0.3 and dropped (see
`docs/RESEARCH.md`, "Route").
