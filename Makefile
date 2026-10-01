# Host build: the scanner and its tests. The iOS app is built separately (later).
CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
AARCH64 := clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld

CORE := core/elf.c core/scan.c core/vm.c core/cpu.c core/simd.c core/linux.c core/load.c core/dl.c core/bionic.c core/proc.c

all: build/apkscan

build/apkscan: tools/apkscan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/apkscan.c $(CORE) -lm

build/test_scan: tests/test_scan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_scan.c $(CORE) -lm

build/fixture.elf: tests/fixture.S | build
	$(AARCH64) -o $@ $<

test: build/test_scan build/test_vm build/fixture.elf build/apkscan build/aoirun build/aoiproc build/signals.elf
	./build/test_vm
	./build/test_scan build/fixture.elf
	./build/apkscan build/fixture.elf
	sh tests/run_guest.sh
	$(MAKE) build/libstep1.so
	$(PYTHON) tests/difftest.py 300
	./build/aoiproc build /signals.elf
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
	$(CC) $(CFLAGS) -o $@ tools/iostest.c $(CORE) $(IOS_SRC) -lm -lz

build/test_vm: tests/test_vm.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_vm.c $(CORE) -lm

# Random-encoding differential test against Unicorn (tests/difftest.py).
build/libstep1.so: tests/step1.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -fPIC -shared -o $@ tests/step1.c $(CORE) -lm

difftest: build/libstep1.so
	$(PYTHON) tests/difftest.py 5000

# Android programs with Android's own linker64 (needs a root from tools/android-root.sh).
build/aoiproc: tools/aoiproc.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoiproc.c $(CORE) -lm

# Debug knobs in the hot paths (AOI_WATCH, AOI_PCRING): off in every other build.
build/aoiproc-debug: tools/aoiproc.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -DAOI_DEBUG -o $@ tools/aoiproc.c $(CORE) -lm

android-test: build/aoiproc
	sh tests/run_android.sh

# Signal delivery (SIGSEGV from a fault, sigreturn, tgkill, masks) through core/proc.c.
build/signals.elf: tests/signals.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-stack-protector -fuse-ld=lld -O1 -o $@ $<
