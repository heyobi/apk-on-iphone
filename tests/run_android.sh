#!/bin/sh
# Step 1 check: unmodified Android programs, loaded by Android's own linker64,
# run in the interpreter. Needs a guest root (tools/fetch-android.sh) in
# $AOI_ANDROID_ROOT; skipped without one.
DIR="$(cd "$(dirname "$0")/.." && pwd)"
R=${AOI_ANDROID_ROOT:-}
if [ -z "$R" ] || [ ! -x "$R/system/bin/toybox" ]; then
    echo "SKIP android: set AOI_ANDROID_ROOT to a root from tools/fetch-android.sh"; exit 0
fi
P="$DIR/build/aoiproc"
fail=0
check() {   # check NAME EXPECTED-LINE PROGRAM ARGS...: exit 0 and that line in the output
    name=$1; exp=$2; shift 2
    opts=""
    while [ "$1" = "-e" ]; do opts="$opts -e $2"; shift 2; done
    # shellcheck disable=SC2086
    out="$("$P" $opts "$R" "$@" 2>/dev/null)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qxF -- "$exp"; then echo "OK  android: $name"
    else echo "FAIL android: $name (rc=$rc)"; fail=1; fi
}
check "toybox echo"   "hello"          /system/bin/toybox echo hello
check "mksh"          "42"             /system/bin/sh -c 'echo merhaba; echo $((6*7))'
check "toybox ls"     "linker64"       /system/bin/toybox ls /apex/com.android.runtime/bin
check "toybox cat"    "# end of file"  /system/bin/toybox cat /system/build.prop
check "toybox uname"  "aarch64"        /system/bin/toybox uname -m

# Boot-time setup (properties, linkerconfig, derive_classpath), then ART itself.
ENV=$(sh "$DIR/tools/android-setup.sh" "$R")
check "getprop"       "34"             /system/bin/getprop ro.build.version.sdk
python3 "$DIR/tests/mkdex.py" "$R/data/local/tmp/hello.dex"
python3 "$DIR/tests/mkdex.py" "$R/data/local/tmp/gc.dex" 0 20000    # 20 MB of garbage, then Runtime.gc()
# Boot dex files are system files: their structural verification is skipped
# (-Xverify:none), as a device skips it through the boot image's vdex.
# shellcheck disable=SC2086
check "ART hello dex" "Merhaba from ART" $ENV /apex/com.android.art/bin/dalvikvm64 -Xverify:none \
    -cp /data/local/tmp/hello.dex Hello
# shellcheck disable=SC2086
check "ART GC (concurrent copying)" "Merhaba from ART" $ENV /apex/com.android.art/bin/dalvikvm64 -Xverify:none \
    -cp /data/local/tmp/gc.dex Hello
# The same with userfaultfd offered: ART then takes the CMC GC and maps the boot
# image (15 components, AOT code for every boot class) instead of running imageless.
export AOI_UFFD=1
# shellcheck disable=SC2086
check "ART hello, boot image + CMC" "Merhaba from ART" $ENV /apex/com.android.art/bin/dalvikvm64 -Xverify:none \
    -cp /data/local/tmp/hello.dex Hello
# Compaction: MREMAP_DONTUNMAP, SIGBUS on missing pages, UFFDIO_COPY/ZEROPAGE.
# shellcheck disable=SC2086
check "ART GC, CMC compaction" "Merhaba from ART" $ENV /apex/com.android.art/bin/dalvikvm64 -Xverify:none \
    -cp /data/local/tmp/gc.dex Hello
# Android's own launcher for framework-using Java (am, pm): AndroidRuntime,
# libandroid_runtime's JNI, RuntimeInit, ProcessState over the in-process binder.
SCP=$(awk '$2=="SYSTEMSERVERCLASSPATH" {printf "-e %s=%s", $2, $3}' "$R/data/system/environ/classpath")
# shellcheck disable=SC2086
check "app_process64 (framework runtime, binder)" "Merhaba from ART" $ENV $SCP -e CLASSPATH=/data/local/tmp/hello.dex \
    /system/bin/app_process64 /system/bin Hello
# Our own Java (java/src, built by tools/javadex.sh) in the app process: a service
# registered with servicemanager comes back as the same local object.
if command -v javac >/dev/null 2>&1 && sh "$DIR/tools/javadex.sh" "$R/data/local/tmp/aoi.dex" >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    check "in-process services (Java binder, servicemanager)" "servicemanager: local binder ok" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.ServiceTest
    # A stand-in service's list calls give an empty ParceledListSlice (ShortcutManager
    # would NPE on null).
    # shellcheck disable=SC2086
    check "stand-in services answer empty lists" "stand-in: getShortcuts 0, getAllPendingJobsInNamespace 0" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.ServiceTest
    # Shared storage (aoi.StorageService's volume) and ashmem regions as memfds
    # (SharedMemory, a CursorWindow with a 100 KB blob: SQLite queries need one).
    # shellcheck disable=SC2086
    check "storage volume, ashmem as memfd" "storage: mounted at /data/media/0, shared memory 8192 41534d21, cursor window true 42" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.StorageTest
    # TLS trust: today's roots (tools/cacerts-extra.pem) in conscrypt's store.
    # shellcheck disable=SC2086
    check "trust store with today's roots" "ca: Sectigo R46 true, SSL.com 2022 true, chain trusted true" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.CaTest
    # AndroidKeyStore: the framework's provider on aoi.Keystore (keystore2, software keys).
    rm -rf "$R/data/misc/keystore/aoitest"
    # shellcheck disable=SC2086
    check "AndroidKeyStore (keystore2: AES, HMAC, EC, ECDH, RSA)" "keystore: aes ok, hmac 32, ec ok, ecdh ok, rsa ok, aliases [aes, ec, hmac, rsa], cert ok, reload ok, deleted true" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.KeystoreTest
    # aoi.Clipboard through the host's clipboard (aoiproc's AOI_CLIP): paste in, copy out.
    # shellcheck disable=SC2086
    export AOI_CLIP="from the host"
    # shellcheck disable=SC2086
    check "clipboard shared with the host" 'clipboard: after copy, paste gives "kopyalandı 42"' $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.ClipboardTest
    unset AOI_CLIP
    # Games' sound effects: android.media.SoundPool with libsoundpool.so and aoi.AudioService.
    # shellcheck disable=SC2086
    check "SoundPool (libsoundpool, audio service)" "soundpool: built and released" $ENV $SCP \
        -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.SoundPoolTest
    # The network: aoi.ConnectivityService, DNS through netd's dnsproxyd (answered by the
    # host's resolver) and an HTTP GET on the host's sockets, from a local server.
    if command -v python3 >/dev/null 2>&1; then
        W=$(mktemp -d); echo "merhaba ag" > "$W/hello.txt"
        PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
        (cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) & HTTPD=$!
        sleep 1
        # shellcheck disable=SC2086
        check "network (connectivity, DNS, HTTP on host sockets)" \
            'network: connected true, internet true, 100, localhost 127.0.0.1, http 200 "merhaba ag"' $ENV $SCP \
            -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.NetworkTest "$PORT"
        kill $HTTPD 2>/dev/null; rm -rf "$W"
    else echo "SKIP android: network (no python3)"; fi
    # OpenGL ES: Android's libEGL -> our driver (/vendor/lib64/egl/libGLES_aoi.so) -> the host's GPU.
    if [ -f "$R/vendor/lib64/egl/libGLES_aoi.so" ] && nm "$P" 2>/dev/null | grep -q aoi_gpu_call; then
        # shellcheck disable=SC2086
        check "OpenGL ES (libEGL, libGLES_aoi, host GPU)" "gles: red triangle on blue, both ways" $ENV $SCP \
            -e CLASSPATH=/data/local/tmp/aoi.dex /system/bin/app_process64 /system/bin aoi.GlesTest
    else echo "SKIP android: OpenGL ES (no host EGL/GLES or driver)"; fi
else echo "SKIP android: in-process services (no javac)"; fi
unset AOI_UFFD
exit $fail
