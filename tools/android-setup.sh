#!/bin/sh
# android-setup.sh ROOT: what init and apexd would have done at boot, so that ART
# can start in ROOT. Each step runs Android's own tool in the interpreter
# (build/aoiproc), except the property area:
#   1. /dev/__properties__ from the build.prop files      (tools/mkprops.py)
#   2. /linkerconfig/ld.config.txt with APEX namespaces    (linkerconfig)
#   3. /data/system/environ/classpath (BOOTCLASSPATH etc.) (derive_classpath)
#   4. /system/etc/public.libraries.txt with every entry "nopreload": the
#      public NDK libraries stay visible to apps but are loaded when first
#      needed, not at runtime start (libhwui, libgui, libpdfium... cost
#      150 M instructions of linking for an ART hello world otherwise)
#   5. /vendor/lib64/hw/mapper.aoi.so, our gralloc mapper (build/mapper.aoi.so,
#      guest/mapper.c), where libui looks for it (the sphal namespace)
#   6. /vendor/lib64/egl/libGLES_aoi.so, our OpenGL ES driver (build/libGLES_aoi.so,
#      guest/gles.c), where libEGL looks for it (ro.hardware.egl=aoi)
# Idempotent; prints the aoiproc -e options for dalvikvm on stdout (with
# ANDROID_NO_USE_FWMARK_CLIENT: there is no netd to tag sockets, core/proc.c).
set -eu
R=$1
DIR="$(cd "$(dirname "$0")/.." && pwd)"
P="$DIR/build/aoiproc"
mkdir -p "$R/linkerconfig" "$R/data/system/environ" "$R/data/dalvik-cache/arm64" "$R/data/local/tmp"
[ -s "$R/dev/__properties__" ] || python3 "$DIR/tools/mkprops.py" "$R" >&2
[ -s "$R/linkerconfig/ld.config.txt" ] && grep -q com_android_art "$R/linkerconfig/ld.config.txt" ||
    "$P" "$R" /apex/com.android.runtime/bin/linkerconfig --target /linkerconfig >/dev/null 2>&1
[ -s "$R/data/system/environ/classpath" ] ||
    "$P" "$R" /apex/com.android.sdkext/bin/derive_classpath /data/system/environ/classpath >/dev/null 2>&1
PL="$R/system/etc/public.libraries.txt"
if [ -f "$PL" ] && [ ! -f "$PL.orig" ]; then
    cp "$PL" "$PL.orig"
    awk '/^#/ || NF == 0 { print; next } { print $1, "nopreload" }' "$PL.orig" > "$PL"
fi
if [ -f "$DIR/build/mapper.aoi.so" ]; then
    mkdir -p "$R/vendor/lib64/hw"
    cp "$DIR/build/mapper.aoi.so" "$R/vendor/lib64/hw/mapper.aoi.so"
fi
if [ -f "$DIR/build/libGLES_aoi.so" ]; then
    mkdir -p "$R/vendor/lib64/egl"
    cp "$DIR/build/libGLES_aoi.so" "$R/vendor/lib64/egl/libGLES_aoi.so"
fi
awk '$2=="BOOTCLASSPATH" || $2=="DEX2OATBOOTCLASSPATH" {printf "-e %s=%s ", $2, $3}' "$R/data/system/environ/classpath"
printf '%s\n' "-e ANDROID_NO_USE_FWMARK_CLIENT=1"            # libnetd_client: no fwmarkd to tag sockets
