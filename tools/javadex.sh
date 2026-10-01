#!/bin/sh
# javadex.sh OUT.dex: compile java/src (Java 8 language level) against the
# framework stubs in java/stubs and turn it into one dex with dx. The stubs only
# declare what our code touches of Android's (often hidden) framework classes;
# they are compile-time only and left out of the dex, so the real classes of the
# guest's framework.jar are used at run time. dx comes from Maven Central
# (dl.google.com, home of d8, is not reachable from the cloud sessions).
set -eu
OUT=${1:?usage: javadex.sh OUT.dex}
DIR="$(cd "$(dirname "$0")/.." && pwd)"
DX="$DIR/build/dalvik-dx.jar"
DX_URL=https://repo1.maven.org/maven2/com/jakewharton/android/repackaged/dalvik-dx/14.0.0_r21/dalvik-dx-14.0.0_r21.jar
DX_SHA=9b13bc80bf86f193f8de9c959f52a8ff83fc85a1cc552f5126f1140da9b85a36
mkdir -p "$DIR/build"
if [ ! -f "$DX" ]; then
    curl -fsSL -o "$DX.tmp" "$DX_URL"
    echo "$DX_SHA  $DX.tmp" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >/dev/null
    mv "$DX.tmp" "$DX"
fi
T="$DIR/build/javadex"
rm -rf "$T"; mkdir -p "$T/stubs" "$T/classes"
javac -nowarn --release 8 -d "$T/stubs" $(find "$DIR/java/stubs" -name '*.java')
javac -nowarn --release 8 -cp "$T/stubs" -d "$T/classes" $(find "$DIR/java/src" -name '*.java')
java -cp "$DX" com.android.dx.command.Main --dex --min-sdk-version=26 --output="$OUT" "$T/classes"
