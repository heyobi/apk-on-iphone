# Host build: the scanner and its tests. The iOS app is built separately (later).
CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
AARCH64 := clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld

CORE := core/elf.c core/scan.c core/vm.c core/cpu.c core/simd.c core/linux.c core/load.c core/dl.c core/bionic.c core/proc.c core/binder.c core/sf.c core/af.c core/gralloc.c core/snap.c core/hle.c

all: build/apkscan

build/apkscan: tools/apkscan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/apkscan.c $(CORE) -lm

build/test_scan: tests/test_scan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_scan.c $(CORE) -lm

build/fixture.elf: tests/fixture.S | build
	$(AARCH64) -o $@ $<

test: build/test_scan build/test_vm build/fixture.elf build/apkscan build/aoirun build/aoiproc build/signals.elf build/pipes.elf build/libGLES_aoi.so build/libaoi_media.so
	./build/test_vm
	./build/test_scan build/fixture.elf
	./build/apkscan build/fixture.elf
	sh tests/run_guest.sh
	$(MAKE) build/libstep1.so
	$(PYTHON) tests/difftest.py 300
	./build/aoiproc build /signals.elf
	./build/aoiproc build /pipes.elf
	sh tests/run_android.sh

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all test clean difftest android-test

build/aoirun: tools/aoirun.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoirun.c $(CORE) -lm

build/hello.elf: tests/hello.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -mgeneral-regs-only -fuse-ld=lld -O1 -o $@ tests/hello.c

run: build/aoirun build/hello.elf
	./build/aoirun build/hello.elf

build/gmpdemo: tools/gmpdemo.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/gmpdemo.c $(CORE) -lm

# Debug build that cross-checks every instruction against Unicorn (QEMU's AArch64 core).
# pip install unicorn   (ships the headers and libunicorn.so.2)
UNICORN ?= $(shell python3 -c "import unicorn,os;print(os.path.dirname(unicorn.__file__))" 2>/dev/null)
ORACLE  := -DAOI_ORACLE -I$(UNICORN)/include core/oracle.c $(UNICORN)/lib/libunicorn.so.2 -Wl,-rpath,$(UNICORN)/lib -lm

build/gmpdemo-check: tools/gmpdemo.c $(CORE) core/oracle.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/gmpdemo.c $(CORE) $(ORACLE)

build/aoiproc-check: tools/aoiproc.c $(CORE) core/oracle.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoiproc.c $(CORE) $(ORACLE)

build/isacheck: tools/isacheck.c $(CORE) core/oracle.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/isacheck.c $(CORE) $(ORACLE)

# The iOS app's test sequence on the host.
IOS_SRC := ios/gmptest.c ios/vmprobe.c ios/androidtest.c core/apk.c
build/iostest: tools/iostest.c $(CORE) $(IOS_SRC) core/*.h ios/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/iostest.c $(CORE) $(IOS_SRC) -lm -lz -lpthread

# The same with the host's OpenGL ES (Mesa) as the app's GPU, as on the phone: HWUI's GPU
# pipeline, WebView's in-process GPU thread.
build/iostest-gpu: tools/iostest.c $(CORE) $(IOS_SRC) core/*.h ios/*.h gpu/host.c gpu/gl_gen.h | build
	$(CC) $(CFLAGS) -o $@ tools/iostest.c $(CORE) $(IOS_SRC) $(AOIPROC_GPU) -lm -lz -lpthread

build/test_vm: tests/test_vm.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_vm.c $(CORE) -lm

# Random-encoding differential test against Unicorn (tests/difftest.py).
build/libstep1.so: tests/step1.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -fPIC -shared -o $@ tests/step1.c $(CORE) -lm

difftest: build/libstep1.so
	$(PYTHON) tests/difftest.py 5000

# Android programs with Android's own linker64 (needs a root from tools/android-root.sh).
# With the host's EGL and GLES (pkg-config egl glesv2: Mesa) the guest gets OpenGL ES
# (gpu/host.c, guest/gles.c); without them AOI_SYS_GL is ENOSYS.
GPU ?= $(shell pkg-config --exists egl glesv2 2>/dev/null && echo 1)
ifeq ($(GPU),1)
AOIPROC_GPU := -DAOI_GPU gpu/host.c $(shell pkg-config --libs egl glesv2)
endif
build/aoiproc: tools/aoiproc.c $(CORE) core/*.h gpu/host.c gpu/gl_gen.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoiproc.c $(CORE) $(AOIPROC_GPU) -lm -lpthread

# Debug knobs in the hot paths (AOI_WATCH, AOI_PCRING): off in every other build.
build/aoiproc-debug: tools/aoiproc.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -DAOI_DEBUG -o $@ tools/aoiproc.c $(CORE) -lm -lpthread

android-test: build/aoiproc build/mapper.aoi.so build/libGLES_aoi.so
	sh tests/run_android.sh

# The gralloc mapper libui loads in the guest (core/gralloc.c names it): guest code,
# installed into a root as /vendor/lib64/hw/mapper.aoi.so.
GUEST_LD ?= -fuse-ld=lld
build/mapper.aoi.so: guest/mapper.c core/gralloc.h | build
	clang --target=aarch64-linux-android29 -shared -nostdlib -ffreestanding -fno-stack-protector -fPIC -O2 \
	    -fvisibility=hidden $(GUEST_LD) -Wl,--hash-style=both -Wl,-soname,mapper.aoi.so -Wall -Wextra -o $@ guest/mapper.c

# The software codecs in the app's process (guest/media.c): /system/lib64/libaoi_media.so,
# linked against stand-ins for libdl and liblog (guest/stubs.c) for their names.
build/stub/libdl.so build/stub/liblog.so build/stub/libc.so: guest/stubs.c | build
	mkdir -p build/stub
	clang --target=aarch64-linux-android29 -shared -nostdlib -ffreestanding -fPIC $(GUEST_LD) \
	    -Wl,-soname,$(notdir $@) -o $@ guest/stubs.c
build/libaoi_media.so: guest/media.c build/stub/libdl.so build/stub/liblog.so build/stub/libc.so | build
	clang --target=aarch64-linux-android29 -shared -nostdlib -ffreestanding -fno-stack-protector -fPIC -O2 \
	    -fvisibility=hidden $(GUEST_LD) -Wl,--hash-style=both -Wl,-soname,libaoi_media.so -Wall -Wextra \
	    -o $@ guest/media.c -Lbuild/stub -ldl -llog -lc

# The guest's OpenGL ES driver (guest/gles.c): /vendor/lib64/egl/libGLES_aoi.so.
build/libGLES_aoi.so: guest/gles.c guest/gl_gen.h core/gpu.h core/gralloc.h | build
	clang --target=aarch64-linux-android29 -shared -nostdlib -ffreestanding -fno-stack-protector -fPIC -O2 \
	    -fvisibility=hidden $(GUEST_LD) -Wl,--hash-style=both -Wl,-soname,libGLES_aoi.so -Wall -Wextra -o $@ guest/gles.c

# Signal delivery (SIGSEGV from a fault, sigreturn, tgkill, masks) through core/proc.c.
build/signals.elf: tests/signals.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-stack-protector -fuse-ld=lld -O1 -o $@ $<

# pipe2 (a blocking read across green threads, O_NONBLOCK), eventfd, epoll.
build/pipes.elf: tests/pipes.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-stack-protector -fuse-ld=lld -O1 -o $@ $<
