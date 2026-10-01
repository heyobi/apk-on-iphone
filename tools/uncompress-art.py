#!/usr/bin/env python3
"""uncompress-art.py ROOT: rewrite ROOT's boot images (system/framework/arm64/*.art)
without their LZ4 compression, so ART maps them instead of decompressing them at
every start (a third of the boot-image start in the interpreter).

An ART image file (version 109) is an ImageHeader, the compressed blocks, then the
live-object bitmap at a page-aligned offset. Uncompressed, the file *is* the image as
it lies in memory: the header, the objects and native data up to image_size, then the
bitmap. ART takes that path when no block is compressed (it maps the file at
image_begin), so the header's blocks go and the bitmap section moves. Checksums stay:
image_checksum is a recorded value other components refer to, not a hash ART redoes.
"""
import glob
import os
import struct
import sys

PAGE = 4096
HDR = 0x100                       # sizeof(ImageHeader), version 109
OFF_IMAGE_SIZE = 0x14
OFF_BITMAP = 0xa0                 # sections[kSectionImageBitmap] {offset, size}
OFF_DATA_SIZE = 0xf0              # data_size_, blocks_offset_, blocks_count_


def lz4_block(src, size):
    """LZ4 block format decoder (what ART writes for storage modes LZ4 and LZ4HC)."""
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        n = tok >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255:
                    break
        out += src[i:i + n]; i += n
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8; i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255:
                    break
        m += 4
        start = len(out) - off
        if off >= m:
            out += out[start:start + m]
        else:                     # overlapping copy: byte by byte
            for k in range(m):
                out.append(out[start + k])
    if len(out) != size:
        raise ValueError("lz4: %d bytes, expected %d" % (len(out), size))
    return bytes(out)


def uncompress(path):
    d = open(path, "rb").read()
    if d[:4] != b"art\n" or d[4:8] != b"109\0":
        raise ValueError("%s: not an ART image version 109" % path)
    image_size = struct.unpack_from("<I", d, OFF_IMAGE_SIZE)[0]
    bm_off, bm_size = struct.unpack_from("<II", d, OFF_BITMAP)
    data_size, blocks_off, nblocks = struct.unpack_from("<III", d, OFF_DATA_SIZE)
    if nblocks == 0:
        return False                                    # already uncompressed
    img = bytearray(image_size)
    img[:HDR] = d[:HDR]
    for k in range(nblocks):
        mode, doff, dsize, ioff, isize = struct.unpack_from("<5I", d, blocks_off + 20 * k)
        if mode == 0:
            img[ioff:ioff + isize] = d[doff:doff + dsize]
        elif mode in (1, 2):
            img[ioff:ioff + isize] = lz4_block(d[doff:doff + dsize], isize)
        else:
            raise ValueError("%s: storage mode %d" % (path, mode))
    new_bm = (image_size + PAGE - 1) // PAGE * PAGE
    struct.pack_into("<II", img, OFF_BITMAP, new_bm, bm_size)
    struct.pack_into("<III", img, OFF_DATA_SIZE, image_size - HDR, 0, 0)
    out = bytes(img) + bytes(new_bm - image_size) + d[bm_off:bm_off + bm_size]
    tmp = path + ".tmp"
    open(tmp, "wb").write(out)
    os.replace(tmp, path)
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[0])
    for p in sorted(glob.glob(os.path.join(sys.argv[1], "system/framework/arm64/*.art"))):
        before = os.path.getsize(p)
        if uncompress(p):
            print("%s: %d -> %d bytes" % (os.path.basename(p), before, os.path.getsize(p)))
