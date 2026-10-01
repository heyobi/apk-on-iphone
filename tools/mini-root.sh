#!/bin/sh
# mini-root.sh FULLROOT OUT LIST: copy the guest paths named in LIST (one per
# line, '#' comments) from a root made by android-root.sh into OUT. Symlinks
# stay symlinks but become relative, so OUT can live inside an app bundle. A line
# "~/path" makes an empty file there (one the guest only stats, never reads).
set -eu
FULL=$1; OUT=$2; LIST=$3
rm -rf "$OUT"; mkdir -p "$OUT"
grep -v '^#' "$LIST" | while read -r g; do
    [ -n "$g" ] || continue
    case "$g" in "~"*) g=${g#\~}; mkdir -p "$OUT$(dirname "$g")"; : > "$OUT$g"; continue;; esac
    mkdir -p "$OUT$(dirname "$g")"
    if [ -L "$FULL$g" ]; then
        t=$(readlink "$FULL$g")
        case "$t" in /*) t=$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))' "$t" "$g");; esac
        ln -s "$t" "$OUT$g"
    else
        cp "$FULL$g" "$OUT$g"
    fi
done
mkdir -p "$OUT/linkerconfig" "$OUT/dev" "$OUT/data/local/tmp" "$OUT/data/dalvik-cache/arm64"
