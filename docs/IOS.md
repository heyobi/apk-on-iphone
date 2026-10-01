# Trying it on an iPhone

The test app (`ios/`) runs the real `libgmp.so` from an APK twice: in the interpreter
and natively from JIT memory, and compares the result and the time.

## 1. Get the IPA

GitHub → Actions → **iOS IPA** → the latest green run → artifact **ApkOnIphone-ipa**
(a zip containing `ApkOnIphone.ipa`). The workflow builds it on every push that
touches `core/`, `ios/` or the workflow; *Run workflow* builds it on demand.

The IPA is unsigned: the sideloading tool signs it with your Apple ID.

## 2. Install

SideStore, AltStore or Sideloadly → install `ApkOnIphone.ipa`.

## 3. Enable JIT

Open StikDebug and enable JIT for **APK on iPhone** (it launches the app attached).
Without JIT the interpreter test still runs; the app says JIT is missing and does
not try native code (executing it would kill the app).

## 4. Run

1. Put an APK in Files (the Qalculate APK has `libgmp.so`).
2. **APK seç** → pick it. The app lists its arm64 libraries.
3. **Hepsini çalıştır**: [1] interpreter, [2] JIT probe, [3] native, then a summary line
   (same result? how many times faster?).
4. **Logu kopyala** and paste the log back into the conversation. The log is also in
   Files → On My iPhone → APK on iPhone → `log.txt`.

## What the JIT probe means

Three ways to get executable memory are tried, each with a two-instruction function:

| strategy | expected on |
|---|---|
| `MAP_JIT + pthread_jit_write_protect_np` | iOS 17.4–18.x with StikDebug |
| `mprotect RW -> RX` | older devices / iOS versions with CS_DEBUGGED |
| `vm_remap dual mapping` | the MeloNX-style path |

If all three report "kernel did not grant execute", the device is a TXM one
(A15+/M2+ on iOS 26): it needs the StikDebug script protocol (a debugger that stays
attached and authorizes one region at launch), which is the next step for this app.
If a strategy crashes the app, the next launch notes it and skips it.
