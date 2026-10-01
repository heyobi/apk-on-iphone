#!/bin/sh
# fetch-android.sh OUT: download the AOSP Android 14 arm64 GSI this project uses,
# verify it and build the guest root OUT (tools/android-root.sh). About 800 MB
# download, 2.3 GB image; the image is deleted afterwards. Never commit OUT.
#
# Source: ponces/treble_aosp v2024.08.16, "vanilla" (pure AOSP, no Google apps),
# listed in https://raw.githubusercontent.com/ponces/treble_aosp/android-14.0/config/ota.json
# (dl.google.com is not reachable from the cloud sessions; GitHub releases are).
set -eu
OUT=${1:?usage: fetch-android.sh OUT}
URL=https://github.com/ponces/treble_aosp/releases/download/v2024.08.16/aosp-arm64-ab-vanilla-14.0-20240816.img.xz
SHA=cbd37132766666f2913de1613a9605ac21f1e54303a29d0f0b8dc99ecc09cb94
if [ -x "$OUT/system/bin/toybox" ]; then echo "$OUT already built"; exit 0; fi
mkdir -p "$OUT"
TMP="$OUT.download"
mkdir -p "$TMP"
curl -fL --retry 3 -o "$TMP/gsi.img.xz" "$URL"
echo "$SHA  $TMP/gsi.img.xz" | sha256sum -c -
xz -dT0 "$TMP/gsi.img.xz"
sh "$(dirname "$0")/android-root.sh" "$TMP/gsi.img" "$OUT"
rm -rf "$TMP"
