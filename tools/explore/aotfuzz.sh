#!/bin/sh
# Exploration: the AOT differential test (tests/aot/run.sh) with a few seeds.
# usage: sh tools/explore/aotfuzz.sh ROOT   (ROOT unused: the host is the guest root)
cd "$(dirname "$0")/../.." || exit 1
for s in 1 2 3; do sh tests/aot/run.sh $s 300 48 40; done
