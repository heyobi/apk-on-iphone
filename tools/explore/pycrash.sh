#!/bin/sh
# Exploration: the CPython test files whose worker exits (regrtest "worker non-zero exit"):
# each run alone, verbose, with the last lines it printed and its exit status.
# usage: sudo sh tools/explore/pycrash.sh ROOT
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
R=$1
cd "$DIR" || exit 1
cp /etc/resolv.conf "$R/etc/resolv.conf"
LXRUN_QUIET=1 build/lxrun "$R" /bin/bash -c 'dnf -y -q install python3-test' > /dev/null 2>&1
for t in test_json test_tarfile test_lib2to3 test_logging test_signal test_pydoc test_ssl test_regrtest \
         test.test_multiprocessing_spawn.test_misc test.test_multiprocessing_fork.test_processes; do
    LXRUN_QUIET=1 timeout 900 build/lxrun "$R" /bin/bash -c "cd /tmp && python3 -X faulthandler -m test -v $t; echo EXIT=\$?" > /tmp/v.out 2>&1
    echo "######## $t (rc=$?)"
    grep -E "^(ERROR|FAIL): |Fatal Python|Segmentation|Traceback|Error:|EXIT=|Ran [0-9]+ test|^(OK|FAILED)" /tmp/v.out | grep -v "^Traceback" | head -n 25
    echo "  --- last lines"
    tail -n 15 /tmp/v.out | cut -c1-220 | sed 's/^/  | /'
done
