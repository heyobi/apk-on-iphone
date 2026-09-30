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
        names = z.namelist()
        libs = [n for n in names if n.startswith("lib/arm64-v8a/") and n.endswith(".so")]
        dex = [n for n in names if n.endswith(".dex")]
        abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.endswith(".so")})
        print(f"{apk}: {len(dex)} dex file(s), {len(libs)} arm64-v8a native librar{'y' if len(libs) == 1 else 'ies'}")
        if not libs:
            if abis:
                others = [n for n in names if n.startswith("lib/") and n.endswith(".so")]
                print(f"  NO arm64-v8a code, but native libraries for: {', '.join(abis)} ({len(others)} files)")
                print("  this is an APK for another CPU; get the arm64-v8a build of the same app")
                return 1
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
