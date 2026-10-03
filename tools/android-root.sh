#!/bin/sh
# android-root.sh IMAGE OUT: turn an AOSP arm64 system image (raw ext4, e.g. a GSI)
# into a guest root for the interpreter: OUT/system plus every APEX unpacked at
# OUT/apex/<name>, the way apexd would mount them. Needs debugfs and unzip.
# The tree stays local (scratch space): never commit Android binaries.
set -eu
IMG=$1; OUT=$2
mkdir -p "$OUT/apex" "$OUT/linkerconfig" "$OUT/dev" "$OUT/proc" "$OUT/data/local/tmp" "$OUT/data/dalvik-cache/arm64"
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
# /apex/apex-info-list.xml, as apexd writes it: linkerconfig only builds APEX
# namespaces for the modules listed there
{
    echo '<?xml version="1.0" encoding="utf-8"?>'
    echo '<apex-info-list>'
    for a in "$OUT"/system/apex/*.apex "$OUT"/system/apex/*.capex; do
        [ -e "$a" ] || continue
        f=$(basename "$a"); name=${f%.capex}; name=${name%.apex}
        [ -d "$OUT/apex/$name" ] || continue
        echo "    <apex-info moduleName=\"$name\" modulePath=\"/system/apex/$f\" preinstalledModulePath=\"/system/apex/$f\" versionCode=\"1\" versionName=\"\" isFactory=\"true\" isActive=\"true\" lastUpdateMillis=\"0\" provideSharedApexLibs=\"false\" />"
    done
    echo '</apex-info-list>'
} > "$OUT/apex/apex-info-list.xml"
# a device's root links these partitions into /system on a GSI
for part in product system_ext; do
    [ -e "$OUT/$part" ] || [ ! -d "$OUT/system/$part" ] || ln -s "/system/$part" "$OUT/$part"
done
# boot images stored uncompressed: ART maps them instead of LZ4-decompressing
# ~25 MB at every start (a third of the boot-image start in the interpreter)
python3 "$(dirname "$0")/uncompress-art.py" "$OUT"
# today's roots that the GSI's store lacks (tools/cacerts-extra.pem): sites whose
# chain ends there (Sectigo R46, SSL.com 2022...) fail "Trust anchor not found"
CA="$OUT/apex/com.android.conscrypt/cacerts"
awk -v d="$CA" '/^# file: /{f=d "/" $3; next} f{print > f} /END CERT/{close(f); f=""}' "$(dirname "$0")/cacerts-extra.pem"
