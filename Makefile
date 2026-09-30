# Host build: the scanner and its tests. The iOS app is built separately (later).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
AARCH64 := clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld

CORE := core/elf.c core/scan.c core/cpu.c core/linux.c core/load.c core/dl.c core/bionic.c

all: build/apkscan

build/apkscan: tools/apkscan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/apkscan.c $(CORE)

build/test_scan: tests/test_scan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_scan.c $(CORE)

build/fixture.elf: tests/fixture.S | build
	$(AARCH64) -o $@ $<

test: build/test_scan build/fixture.elf build/apkscan build/aoirun
	./build/test_scan build/fixture.elf
	./build/apkscan build/fixture.elf
	sh tests/run_guest.sh

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all test clean

build/aoirun: tools/aoirun.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoirun.c $(CORE)

build/hello.elf: tests/hello.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -mgeneral-regs-only -fuse-ld=lld -O1 -o $@ tests/hello.c

run: build/aoirun build/hello.elf
	./build/aoirun build/hello.elf

build/gmpdemo: tools/gmpdemo.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/gmpdemo.c $(CORE)

# Debug build that cross-checks every instruction against Unicorn (QEMU's AArch64 core).
# pip install unicorn   (ships the headers and libunicorn.so.2)
UNICORN ?= $(shell python3 -c "import unicorn,os;print(os.path.dirname(unicorn.__file__))" 2>/dev/null)
ORACLE  := -DAOI_ORACLE -I$(UNICORN)/include core/oracle.c $(UNICORN)/lib/libunicorn.so.2 -Wl,-rpath,$(UNICORN)/lib

build/gmpdemo-check: tools/gmpdemo.c $(CORE) core/oracle.c core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/gmpdemo.c $(CORE) $(ORACLE)
