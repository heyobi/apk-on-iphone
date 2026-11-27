#!/bin/sh
# Exploration: why /proc/self/fd is not found (traced).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
LXRUN_QUIET=1 build/lxrun -t "$R" /usr/bin/python3 -c 'import os; print(os.listdir("/proc/self/fd"))' > /tmp/o 2> /tmp/t
cat /tmp/o | tail -n 3
grep -n "proc" /tmp/t | tail -n 20
LXRUN_QUIET=1 build/lxrun -t "$R" /bin/ls -la /proc/self/fd /proc/$$ > /tmp/o2 2> /tmp/t2
cat /tmp/o2 | head; grep -n "proc" /tmp/t2 | tail -n 10
