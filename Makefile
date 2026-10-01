# Host build: the scanner and its tests. The iOS app is built separately (later).
CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
AARCH64 := clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld

CORE := core/elf.c core/scan.c core/cpu.c core/linux.c core/load.c core/dl.c core/bionic.c core/simd.c
LDLIBS  := -lm

all: build/apkscan

build/apkscan: tools/apkscan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/apkscan.c $(CORE) $(LDLIBS)

build/test_scan: tests/test_scan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_scan.c $(CORE) $(LDLIBS)

build/fixture.elf: tests/fixture.S | build
	$(AARCH64) -o $@ $<

test: build/test_scan build/fixture.elf build/apkscan build/aoirun build/libstep1.so
	./build/test_scan build/fixture.elf
	./build/apkscan build/fixture.elf
	sh tests/run_guest.sh
	$(PYTHON) tests/difftest.py 300

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all test clean difftest

build/aoirun: tools/aoirun.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/aoirun.c $(CORE) $(LDLIBS)

build/hello.elf: tests/hello.c | build
	clang --target=aarch64-linux-gnu -nostdlib -static -mgeneral-regs-only -fuse-ld=lld -O1 -o $@ tests/hello.c

run: build/aoirun build/hello.elf
	./build/aoirun build/hello.elf

build/gmpdemo: tools/gmpdemo.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/gmpdemo.c $(CORE) $(LDLIBS)

build/libstep1.so: tests/step1.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -fPIC -shared -o $@ tests/step1.c $(CORE) $(LDLIBS)

difftest: build/libstep1.so
	$(PYTHON) tests/difftest.py 5000
