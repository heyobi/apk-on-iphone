#!/usr/bin/env python3
"""Unpack the arm64-v8a native libraries of an APK and run apkscan on each.

usage: tools/apkscan.py APP.apk        (build apkscan first: make)
"""
import os
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCAN = os.path.join(HERE, "..", "build", "apkscan")


def main(apk):
    with zipfile.ZipFile(apk) as z, tempfile.TemporaryDirectory() as tmp:
        libs = [n for n in z.namelist() if n.startswith("lib/arm64-v8a/") and n.endswith(".so")]
        dex = [n for n in z.namelist() if n.endswith(".dex")]
        print(f"{apk}: {len(dex)} dex file(s), {len(libs)} arm64-v8a native librar{'y' if len(libs) == 1 else 'ies'}")
        if not libs:
            print("  pure Java/Kotlin: only ART (the dex runtime) is needed, no native code to patch")
            return 0
        paths = []
        for n in libs:
            p = os.path.join(tmp, os.path.basename(n))
            with open(p, "wb") as fh:
                fh.write(z.read(n))
            paths.append(p)
        return subprocess.call([SCAN] + paths)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
