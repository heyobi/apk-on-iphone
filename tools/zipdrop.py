#!/usr/bin/env python3
"""zipdrop.py APK PREFIX...: remove the entries under each PREFIX from APK, in place.
The entries kept are copied as they are (compressed bytes too), and every stored
(uncompressed) one starts at a multiple of 4096 in the new file, as zipalign -p
leaves them: the system loads libraries from an APK by mapping them there.

tools/android-root.sh drops the WebView's 32-bit library (lib/armeabi-v7a, 68 MB)
from the system image's webview.apk: the phone runs only arm64 code."""
import os
import struct
import sys
import zipfile

ALIGN = 4096


def main():
    apk, prefixes = sys.argv[1], tuple(sys.argv[2:])
    tmp = apk + ".tmp"
    with zipfile.ZipFile(apk) as zin, open(apk, "rb") as raw, open(tmp, "wb") as out:
        central = []
        for info in zin.infolist():
            if info.filename.startswith(prefixes):
                continue
            raw.seek(info.header_offset)
            h = raw.read(30)
            name_len, extra_len = struct.unpack("<HH", h[26:30])
            raw.seek(info.header_offset + 30 + name_len + extra_len)
            data = raw.read(info.compress_size)
            name = info.filename.encode("utf-8")
            offset = out.tell()
            extra = b""
            if info.compress_type == zipfile.ZIP_STORED:
                pad = (-(offset + 30 + len(name))) % ALIGN
                extra = b"\0" * pad
            flags = info.flag_bits & ~0x08               # sizes are in the header: no data descriptor
            out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, info.extract_version, flags, info.compress_type,
                                  *dos_time(info), info.CRC, info.compress_size, info.file_size,
                                  len(name), len(extra)))
            out.write(name + extra + data)
            central.append((info, name, offset, flags))
        start = out.tell()
        for info, name, offset, flags in central:
            out.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, info.create_version, info.extract_version,
                                  flags, info.compress_type, *dos_time(info), info.CRC, info.compress_size,
                                  info.file_size, len(name), 0, 0, 0, info.internal_attr, info.external_attr,
                                  offset))
            out.write(name)
        size = out.tell() - start
        out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central), size, start, 0))
    os.replace(tmp, apk)


def dos_time(info):
    y, mo, d, h, mi, s = info.date_time
    return (h << 11 | mi << 5 | s // 2), ((y - 1980) << 9 | mo << 5 | d)


if __name__ == "__main__":
    main()
