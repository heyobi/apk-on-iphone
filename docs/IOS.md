# Trying it on an iPhone

LiquidAPK (`ios/`) runs Android apps in the interpreter: no JIT, no debugger, nothing
beyond a normal sideloaded app.

1. **Get the IPA:** GitHub → **Releases** → latest `v0.x.N` → `ApkOnIphone.ipa`
   (built and published by Actions on every push to main).
2. **Install** with SideStore, AltStore or Sideloadly (they sign it with your Apple ID).
3. **Add an app:** LiquidAPK → **APK ekle** → pick an `.apk` in Files. It is installed
   into the app's container (its native libraries extracted, a data directory of its
   own) and gets a card.
4. **Compile:** right after it is added the app is compiled once with Android's own
   dex2oat; the card shows the percentage and the time left, with Cancel. It goes on
   with LiquidAPK in the background (iOS shows its progress) and pauses only when the phone is very hot (or hot
   while an app is in use). Then the app is started once out of sight and saved.
5. **Run it:** tap the card: a compiled app resumes from its snapshot in about a second
   (an uncompiled one asks: compile first, or open it slow). Swipe in from the left edge
   for Android's back, from the right edge to leave the app (it keeps running); the iOS
   keyboard opens for text fields. If an app dies, LiquidAPK says so, with the reason
   from its log and a button to copy the log.
6. **Cards:** hold one for **Ana ekrana ekle** (a home-screen link through Shortcuts),
   **Baştan başlat** (drop the saved state; it still opens fast), **Derle / Hızlandır /
   Yeniden derle / Derlemeyi sil**, or **Kaldır**.
7. **Logu kopyala** copies the log (with the end of the app's own `app.log`) for a bug
   report; the logs are also in Files → On My iPhone → LiquidAPK.
8. **Geliştirici** has the licenses and the older checks: the address-space probe, Android's toybox and
   mksh, ART hello-world and GC, and GMP n! from an APK's `libgmp.so`.

The interpreter runs about 80-90 M guest instructions/s on an iPhone 16 Pro; the
bundled Android root (framework, boot image, fonts, WebView) makes the IPA ~240 MB.

First device result (iPhone 16 Pro, iOS 27.0.1): 20000! (77,338 digits, 28 M guest
instructions) in 0.33 s, about 85 M instructions/s.

Native execution through StikDebug was tried in versions 0.1–0.3 and dropped (see
`docs/RESEARCH.md`, "Route").
