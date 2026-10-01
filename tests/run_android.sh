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
    out="$("$P" "$R" "$@" 2>/dev/null)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qxF -- "$exp"; then echo "OK  android: $name"
    else echo "FAIL android: $name (rc=$rc)"; fail=1; fi
}
check "toybox echo"   "hello"          /system/bin/toybox echo hello
check "mksh"          "42"             /system/bin/sh -c 'echo merhaba; echo $((6*7))'
check "toybox ls"     "linker64"       /system/bin/toybox ls /apex/com.android.runtime/bin
check "toybox cat"    "# end of file"  /system/bin/toybox cat /system/build.prop
check "toybox uname"  "aarch64"        /system/bin/toybox uname -m
exit $fail
