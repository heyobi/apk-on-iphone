#!/bin/sh
# Exploration: why /proc/self/fd is not found in a child process (traced).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
rm -f /tmp/ct
AOI_CHILD_TRACE=/tmp/ct LXRUN_QUIET=1 build/lxrun -t "$R" /bin/bash -c 'python3 -c "import os; f = open(\"/etc/os-release\"); print(os.listdir(\"/proc/self/fd\"))"; echo rc=$?' > /tmp/o 2> /tmp/t
cat /tmp/o | tail -n 4
echo "--- child trace, proc lines"; grep -n "proc\|ENOENT\|= -2" /tmp/ct | grep -i proc | tail -n 15
echo "--- parent trace, proc lines"; grep -n "proc" /tmp/t | tail -n 5
