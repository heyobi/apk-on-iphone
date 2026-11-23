#!/bin/sh
# Downloads Alpine Linux's aarch64 minirootfs from the official mirror, checks it
# against the SHA-256 Alpine publishes next to it (latest-releases.yaml and the
# .sha256 file must agree), and unpacks it into DIR.
# usage: tests/linux/fetch-alpine.sh DIR [VERSION]   (VERSION: e.g. 3.22.2; default: latest-stable)
set -eu
DIR=$1
MIRROR=https://dl-cdn.alpinelinux.org/alpine
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if [ $# -ge 2 ]; then
    V=$2
    BR=v$(echo "$V" | cut -d. -f1-2)
    FILE=alpine-minirootfs-$V-aarch64.tar.gz
else
    BR=latest-stable
    curl -fsSL --retry 3 -o "$TMP/rel.yaml" "$MIRROR/$BR/releases/aarch64/latest-releases.yaml"
    FILE=$(awk '/flavor: alpine-minirootfs/{f=1} f&&/file:/{print $2; exit}' "$TMP/rel.yaml")
    YSUM=$(awk '/flavor: alpine-minirootfs/{f=1} f&&/sha256:/{print $2; exit}' "$TMP/rel.yaml")
fi
URL=$MIRROR/$BR/releases/aarch64/$FILE
curl -fsSL --retry 3 -o "$TMP/$FILE" "$URL"
curl -fsSL --retry 3 -o "$TMP/$FILE.sha256" "$URL.sha256"
SUM=$(cut -d' ' -f1 "$TMP/$FILE.sha256")
if [ -n "${YSUM:-}" ] && [ "$YSUM" != "$SUM" ]; then echo "checksum files disagree" >&2; exit 1; fi
GOT=$(sha256sum "$TMP/$FILE" | cut -d' ' -f1)
if [ "$GOT" != "$SUM" ]; then echo "SHA-256 mismatch for $FILE: $GOT != $SUM" >&2; exit 1; fi
echo "verified $FILE sha256 $GOT"
mkdir -p "$DIR"
tar -xzf "$TMP/$FILE" -C "$DIR" --no-same-owner
echo "$FILE" > "$DIR/.lxrun-image"
