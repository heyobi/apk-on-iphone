#!/usr/bin/env python3
"""mkprops.py ROOT [NAME=VALUE ...]: write ROOT/dev/__properties__, Android's
system-property area, from the root's build.prop files plus the extra pairs.

bionic reads properties from /dev/__properties__. On a device init builds a
directory of per-SELinux-context areas there; when it is a single file instead,
bionic uses it as one "pre-split" area holding every property. That is what we
write: bionic's prop_area layout (libc/system_properties/prop_area.cpp):

  header (128 bytes): bytes_used, serial, magic 'PROP', version 0xfc6ed0ab, reserved
  data: a trie of prop_bt nodes, one per dot-separated name segment, each level a
        binary search tree ordered by (length, bytes); a node's `prop` points at a
        prop_info { serial, value[92], name }. Values of 92 bytes and more use the
        long form: a flag in serial, and an offset to the value stored after it.
All offsets are relative to the start of data. The root node sits at offset 0,
followed by the 92-byte "dirty backup area".
"""
import os
import struct
import sys

MAGIC, VERSION = 0x504F5250, 0xFC6ED0AB
PROP_VALUE_MAX = 92
LONG_FLAG = 1 << 16
LONG_ERROR = b"Must use __system_property_read_callback() to read"
AREA_SIZE = 128 * 1024
BT = struct.Struct("<5I")                 # namelen, prop, left, right, children


class Area:
    def __init__(self):
        self.data = bytearray()
        self.alloc(BT.size)                # root node (empty name)
        self.alloc(PROP_VALUE_MAX)         # dirty backup area

    def alloc(self, size):
        off = len(self.data)
        self.data += bytes((size + 3) & ~3)
        return off

    def bt(self, off):
        return list(BT.unpack_from(self.data, off))

    def set_bt(self, off, fields):
        BT.pack_into(self.data, off, *fields)

    def new_bt(self, name):
        off = self.alloc(BT.size + len(name) + 1)
        self.set_bt(off, [len(name), 0, 0, 0, 0])
        self.data[off + BT.size:off + BT.size + len(name)] = name
        return off

    def name_of(self, off):
        n = self.bt(off)[0]
        return bytes(self.data[off + BT.size:off + BT.size + n])

    def child(self, parent, name):
        """The node for `name` among parent's children, created if missing."""
        f = self.bt(parent)
        if not f[4]:
            f[4] = self.new_bt(name)
            self.set_bt(parent, f)
            return f[4]
        cur = f[4]
        key = (len(name), name)
        while True:
            other = self.name_of(cur)
            okey = (len(other), other)
            if key == okey:
                return cur
            cf = self.bt(cur)
            side = 2 if key < okey else 3      # left / right
            if not cf[side]:
                cf[side] = self.new_bt(name)
                self.set_bt(cur, cf)
                return cf[side]
            cur = cf[side]

    def add(self, name, value):
        node = 0
        for seg in name.split(b"."):
            node = self.child(node, seg)
        f = self.bt(node)
        if f[1]:
            raise ValueError("duplicate property %r" % name)
        info = self.alloc(4 + PROP_VALUE_MAX + len(name) + 1)
        self.data[info + 4 + PROP_VALUE_MAX:info + 4 + PROP_VALUE_MAX + len(name)] = name
        if len(value) < PROP_VALUE_MAX:
            struct.pack_into("<I", self.data, info, len(value) << 24)
            self.data[info + 4:info + 4 + len(value)] = value
        else:                                  # long form (ro.* only on a device)
            val = self.alloc(len(value) + 1)
            self.data[val:val + len(value)] = value
            struct.pack_into("<I", self.data, info, len(LONG_ERROR) << 24 | LONG_FLAG)
            self.data[info + 4:info + 4 + len(LONG_ERROR)] = LONG_ERROR
            struct.pack_into("<I", self.data, info + 4 + 56, val - info)
        f[1] = info
        self.set_bt(node, f)

    def image(self):
        head = struct.pack("<4I", len(self.data), 0, MAGIC, VERSION) + bytes(28 * 4)
        out = head + bytes(self.data)
        if len(out) > AREA_SIZE:
            raise ValueError("property area overflow: %d bytes" % len(out))
        return out + bytes(AREA_SIZE - len(out))


def read_props(path, props):
    try:
        lines = open(path, "rb").read().splitlines()
    except OSError:
        return 0
    n = 0
    for line in lines:
        line = line.strip()
        if not line or line.startswith(b"#") or b"=" not in line or line.startswith(b"import "):
            continue
        k, v = line.split(b"=", 1)
        props[k.strip()] = v.strip()
        n += 1
    return n


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.split("\n\n")[0])
    root = sys.argv[1]
    # What a device's vendor partition (absent from a GSI) would say; build.prop wins.
    props = {b"ro.product.cpu.abi": b"arm64-v8a", b"ro.product.cpu.abilist": b"arm64-v8a",
             b"ro.product.cpu.abilist64": b"arm64-v8a", b"ro.product.cpu.abilist32": b"",
             b"ro.zygote": b"zygote64",
             b"servicemanager.ready": b"true",       # set by servicemanager on a device (ours is in core/binder.c)
             b"hwservicemanager.ready": b"true",     # the same for HIDL's (core/binder.c, on /dev/hwbinder)
             # the Java heap a phone's vendor sets; without these ART caps apps at 16 MB
             # (AndroidRuntime's -Xmx default) and they die of OutOfMemoryError. (Setting
             # heapstartsize/minfree/maxfree/targetutilization too makes the CMC GC run
             # past its space here: left at ART's defaults.)
             b"dalvik.vm.heapgrowthlimit": b"128m", b"dalvik.vm.heapsize": b"256m",
             # OpenGL ES: libEGL loads /vendor/lib64/egl/libGLES_aoi.so (guest/gles.c)
             b"ro.hardware.egl": b"aoi",
             # libcutils makes ashmem regions as memfds (core/proc.c), not /dev/ashmem
             b"sys.use_memfd": b"true",
             # ART saves the methods its JIT finds hot into the app's profile, which a
             # big app's second dex2oat (speed-profile, ios/androidtest.c) compiles
             b"dalvik.vm.usejitprofiles": b"true",
             # AAudio (native games, Chromium) without MMAP: its legacy path is an
             # AudioTrack, which our AudioFlinger (core/af.c) plays; there is no
             # "media.aaudio" service
             b"aaudio.mmap_policy": b"1", b"aaudio.mmap_exclusive_policy": b"1",
             # the software codecs' store registers as AIDL in the app process
             # (guest/media.c): MediaCodec looks for it there too (libcodec2_vndk)
             b"media.c2.hal.selection": b"aidl",
             # (which libcodec2_vndk honours from vendor API level 202404 on)
             b"ro.vendor.api_level": b"202404",
             # MediaPlayer's extractors in the process too (no media.extractor service)
             b"media.stagefright.extractremote": b"false"}
    for rel in ("system/build.prop", "system/system_ext/etc/build.prop", "system/product/etc/build.prop",
                "vendor/build.prop", "odm/etc/build.prop"):
        read_props(os.path.join(root, rel), props)
    # A release ("user") build, as on a phone: the GSI says userdebug and debuggable,
    # and apps such as WhatsApp then warn of a custom ROM.
    for k in list(props):
        if b"fingerprint" in k or k.endswith(b".description") or k.endswith(b"display.id") or k.endswith(b".flavor"):
            props[k] = props[k].replace(b"userdebug", b"user").replace(b"test-keys", b"release-keys")
    props.update({b"ro.build.type": b"user", b"ro.debuggable": b"0", b"ro.build.tags": b"release-keys"})
    for kv in sys.argv[2:]:
        k, v = kv.encode().split(b"=", 1)
        props[k] = v
    area = Area()
    for k in sorted(props):
        area.add(k, props[k])
    os.makedirs(os.path.join(root, "dev"), exist_ok=True)
    out = os.path.join(root, "dev", "__properties__")
    with open(out, "wb") as f:
        f.write(area.image())
    os.chmod(out, 0o444)
    print("%s: %d properties, %d bytes used" % (out, len(props), len(area.data)))


if __name__ == "__main__":
    main()
