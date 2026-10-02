#!/usr/bin/env python3
"""mktestapk.py OUT.apk: the GL test app (java/testapp) as an APK the app process
can run (aoi.Main): classes.dex (javac against java/testapp/stubs, then dx as in
tools/javadex.sh) and a binary AndroidManifest.xml written here, so no Android SDK
is needed. No resources.arsc: the manifest holds plain values only.

The manifest: package aoi.glapp, one exported activity aoi.glapp.GlActivity with
the MAIN/LAUNCHER intent filter, minSdk 26, targetSdk 34."""
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

ANDROID = "http://schemas.android.com/apk/res/android"
ATTR_IDS = {"name": 0x01010003, "exported": 0x01010010, "versionCode": 0x0101021B,
            "minSdkVersion": 0x0101020C, "targetSdkVersion": 0x01010270, "hasCode": 0x0101000C}
T_STRING, T_INT, T_BOOL = 0x03, 0x10, 0x12

# (tag, [(namespace?, attr, value)], children)
MANIFEST = ("manifest", [(False, "package", "aoi.glapp"), (True, "versionCode", 1)], [
    ("uses-sdk", [(True, "minSdkVersion", 26), (True, "targetSdkVersion", 34)], []),
    ("application", [(True, "hasCode", True)], [
        ("activity", [(True, "name", "aoi.glapp.GlActivity"), (True, "exported", True)], [
            ("intent-filter", [], [
                ("action", [(True, "name", "android.intent.action.MAIN")], []),
                ("category", [(True, "name", "android.intent.category.LAUNCHER")], []),
            ]),
        ]),
    ]),
])


def binary_xml(root):
    # string pool: attribute names with resource ids first (the resource map's order)
    strings = [a for a in ATTR_IDS]
    def add(s):
        if s not in strings:
            strings.append(s)
    def walk(el):
        tag, attrs, kids = el
        add(tag)
        for ns, name, value in attrs:
            add(name)
            if isinstance(value, str):
                add(value)
        for k in kids:
            walk(k)
    add("android"); add(ANDROID)
    walk(root)
    idx = {s: i for i, s in enumerate(strings)}

    data = b""
    offsets = []
    for s in strings:
        offsets.append(len(data))
        u = s.encode("utf-16-le")
        data += struct.pack("<H", len(s)) + u + b"\0\0"
    while len(data) % 4:
        data += b"\0"
    hdr = 28
    start = hdr + 4 * len(strings)
    pool = struct.pack("<HHIIIIII", 0x0001, hdr, start + len(data), len(strings), 0, 0, start, 0)
    pool += b"".join(struct.pack("<I", o) for o in offsets) + data

    resmap = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(ATTR_IDS)) + b"".join(
        struct.pack("<I", ATTR_IDS[a]) for a in ATTR_IDS)

    body = struct.pack("<HHIIIII", 0x0100, 16, 24, 1, 0xFFFFFFFF, idx["android"], idx[ANDROID])
    def element(el):
        nonlocal body
        tag, attrs, kids = el
        raw = b""
        for ns, name, value in attrs:
            if isinstance(value, bool):
                typ, d, rv = T_BOOL, 0xFFFFFFFF if value else 0, 0xFFFFFFFF
            elif isinstance(value, int):
                typ, d, rv = T_INT, value, 0xFFFFFFFF
            else:
                typ, d, rv = T_STRING, idx[value], idx[value]
            raw += struct.pack("<IIIHBBI", idx[ANDROID] if ns else 0xFFFFFFFF, idx[name], rv, 8, 0, typ, d)
        ext = struct.pack("<IIHHHHHH", 0xFFFFFFFF, idx[tag], 20, 20, len(attrs), 0, 0, 0)
        body += struct.pack("<HHIII", 0x0102, 16, 16 + len(ext) + len(raw), 1, 0xFFFFFFFF) + ext + raw
        for k in kids:
            element(k)
        body += struct.pack("<HHIIIII", 0x0103, 16, 24, 1, 0xFFFFFFFF, 0xFFFFFFFF, idx[tag])
    element(root)
    body += struct.pack("<HHIIIII", 0x0101, 16, 24, 1, 0xFFFFFFFF, idx["android"], idx[ANDROID])

    content = pool + resmap + body
    return struct.pack("<HHI", 0x0003, 8, 8 + len(content)) + content


def dex(top, tmp):
    dx = os.path.join(top, "build", "dalvik-dx.jar")
    if not os.path.exists(dx):        # tools/javadex.sh fetches it
        subprocess.check_call(["sh", os.path.join(top, "tools", "javadex.sh"), os.path.join(tmp, "aoi.dex")])
    def java(d):
        return [os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs if f.endswith(".java")]
    stubs, classes = os.path.join(tmp, "stubs"), os.path.join(tmp, "classes")
    subprocess.check_call(["javac", "-nowarn", "--release", "8", "-d", stubs] + java(os.path.join(top, "java/testapp/stubs")))
    subprocess.check_call(["javac", "-nowarn", "--release", "8", "-cp", stubs, "-d", classes]
                          + java(os.path.join(top, "java/testapp/src")))
    out = os.path.join(tmp, "classes.dex")
    subprocess.check_call(["java", "-cp", dx, "com.android.dx.command.Main", "--dex", "--min-sdk-version=26",
                           "--output=" + out, classes])
    return open(out, "rb").read()


def main():
    out = sys.argv[1]
    top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with tempfile.TemporaryDirectory() as tmp:
        d = dex(top, tmp)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("AndroidManifest.xml", binary_xml(MANIFEST))
        z.writestr("classes.dex", d)


if __name__ == "__main__":
    main()
