#!/bin/sh
# Exploration: tests/phone/smoke-1.0.3.sh under lxrun (the script's own check before a phone runs it).
# usage: sudo sh tools/explore/smoke.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
cp tests/phone/smoke-1.0.3.sh "$R/root/smoke.sh"
LXRUN_QUIET=1 timeout 600 build/lxrun "$R" /bin/bash -l -c 'bash /root/smoke.sh' 2>&1 | tail -n 40
