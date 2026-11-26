#!/bin/sh
# Exploration: the AOT differential test (tests/aot/run.sh) with a few seeds, then with two
# deliberately broken translators: the test must fail on both.
# usage: sh tools/explore/aotfuzz.sh ROOT   (ROOT unused: the host is the guest root)
cd "$(dirname "$0")/../.." || exit 1
for s in 4 5; do sh tests/aot/run.sh $s 300 48 40; done
sed "216s/'\^'/'|'/" tools/aotgen.c > /tmp/aotgen_eor.c          # eor with an immediate done as orr
sed '374s/c->c ? r_ <= a_ : r_ < a_/c->c ? r_ < a_ : r_ < a_/' tools/aotgen.c > /tmp/aotgen_adcs.c   # adds-with-carry's C flag wrong when carry is set
for m in eor adcs; do
    cmp -s tools/aotgen.c /tmp/aotgen_$m.c && { echo "MUTATION $m: sed changed nothing"; continue; }
    if AOTGEN_SRC=/tmp/aotgen_$m.c sh tests/aot/run.sh 1 300 48 40 > /tmp/mut.txt 2>&1; then echo "MUTATION $m: NOT caught"; else echo "MUTATION $m: caught"; grep -E "FAIL|native" /tmp/mut.txt | head -n 3; fi
done
