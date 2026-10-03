#!/bin/sh
# app-install.sh ROOT APP.apk DIR: an APK installed on the host as the iOS app
# installs it (ios/main.m: AoiApp install + prepare): DIR is the app's /data, a copy
# of the root's /data with the APK at app/apk/base.apk and this build's aoi.dex.
# Then run it as the phone does (native libraries, dex2oat once, the app):
#   AOI_ANDROID_ROOT=ROOT AOI_APP_DATA=DIR AOI_APP_LOG=DIR.log AOI_APP_FRAME=DIR.frame \
#   AOI_APP_TAPS="x,y;..." build/iostest
set -eu
ROOT=${1:?usage: app-install.sh ROOT APP.apk DIR}
APK=${2:?usage: app-install.sh ROOT APP.apk DIR}
DIR=${3:?usage: app-install.sh ROOT APP.apk DIR}
TOP="$(cd "$(dirname "$0")/.." && pwd)"
[ -d "$DIR" ] || cp -a "$ROOT/data" "$DIR"
mkdir -p "$DIR/app/apk" "$DIR/local/tmp" "$DIR/dalvik-cache/arm64"
cmp -s "$APK" "$DIR/app/apk/base.apk" || cp "$APK" "$DIR/app/apk/base.apk"
[ -f "$TOP/build/aoi.dex" ] || sh "$TOP/tools/javadex.sh" "$TOP/build/aoi.dex"
cp "$TOP/build/aoi.dex" "$DIR/local/tmp/aoi.dex"
echo "$DIR: $(basename "$APK") installed"
