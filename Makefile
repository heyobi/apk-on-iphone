# Host build: the scanner and its tests. The iOS app is built separately (later).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
AARCH64 := clang --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld

CORE := core/elf.c core/scan.c

all: build/apkscan

build/apkscan: tools/apkscan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tools/apkscan.c $(CORE)

build/test_scan: tests/test_scan.c $(CORE) core/*.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_scan.c $(CORE)

build/fixture.elf: tests/fixture.S | build
	$(AARCH64) -o $@ $<

test: build/test_scan build/fixture.elf build/apkscan
	./build/test_scan build/fixture.elf
	./build/apkscan build/fixture.elf

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all test clean
