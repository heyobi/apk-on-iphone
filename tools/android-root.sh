#!/bin/sh
# android-root.sh IMAGE OUT: turn an AOSP arm64 system image (raw ext4, e.g. a GSI)
# into a guest root for the interpreter: OUT/system plus every APEX unpacked at
# OUT/apex/<name>, the way apexd would mount them. Needs debugfs and unzip.
# The tree stays local (scratch space): never commit Android binaries.
set -eu
IMG=$1; OUT=$2
mkdir -p "$OUT/apex" "$OUT/linkerconfig" "$OUT/dev" "$OUT/proc" "$OUT/data/local/tmp"
[ -d "$OUT/system/bin" ] || debugfs -R "rdump /system $OUT" "$IMG" >/dev/null 2>&1
TMP=$(mktemp -d)
for a in "$OUT"/system/apex/*.apex "$OUT"/system/apex/*.capex; do
    [ -e "$a" ] || continue
    name=$(basename "$a"); name=${name%.capex}; name=${name%.apex}
    [ -d "$OUT/apex/$name" ] && continue
    rm -rf "$TMP"/*
    if [ "${a%.capex}" != "$a" ]; then                 # compressed APEX: a zip around the APEX
        unzip -q -o "$a" original_apex -d "$TMP" && mv "$TMP/original_apex" "$TMP/x.apex"
    else cp "$a" "$TMP/x.apex"; fi
    unzip -q -o "$TMP/x.apex" apex_payload.img -d "$TMP" 2>/dev/null || { echo "skip $name (no payload)"; continue; }
    mkdir -p "$OUT/apex/$name"
    if debugfs -R "rdump / $OUT/apex/$name" "$TMP/apex_payload.img" >/dev/null 2>&1 && [ -n "$(ls "$OUT/apex/$name")" ]; then
        echo "apex $name"
    else echo "skip $name (payload not ext4)"; rmdir "$OUT/apex/$name"; fi
done
rm -rf "$TMP"
