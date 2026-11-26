# dnf's fast start (core aoi_proc_zygote): exec of /usr/bin/dnf-3 runs this script. It loads
# dnf's command line (most of every dnf command's start-up), asks the core to save the
# process (/dev/aoi_snapshot: saved only when the core wants a new snapshot) and goes on.
# A later dnf command resumes right after the save, with its own arguments in
# /proc/self/cmdline (python3 [ARG] SCRIPT [--env=FILE] GIVEN ARGS...) and its environment in FILE.
# The rest is what /usr/bin/dnf-3 does.
import os
import sys
import time

if sys.path and sys.path[0] == os.path.dirname(os.path.abspath(__file__)):
    del sys.path[0]                            # (as dnf-3 does: nothing imported from the script's folder)
from dnf.cli import main

try:
    open("/dev/aoi_snapshot").close()          # (always "not found": it is a request)
except OSError:
    pass
while True:                                    # saved between two time slices: wait for it
    try:
        open("/dev/aoi_snapshot_pending").close()
    except OSError as e:
        if e.errno == 16:                      # EBUSY: not taken yet
            time.sleep(0.001)
            continue
    break

args = open("/proc/self/cmdline", "rb").read().split(b"\0")
if args and args[-1] == b"":
    args.pop()
args = [os.fsdecode(a) for a in args]
args = args[args.index(__file__) + 1:] if __file__ in args else args[2:]   # (after INTERP [ARG] SCRIPT)
if args and args[0].startswith("--env="):      # resumed: this command's environment
    path = args.pop(0)[6:]
    try:
        with open(path, "rb") as f:
            env = f.read().split(b"\0")
        os.unlink(path)
        os.environ.clear()
        for kv in env:
            if b"=" in kv:
                k, v = kv.split(b"=", 1)
                os.environ[os.fsdecode(k)] = os.fsdecode(v)
    except OSError:
        pass
    import random
    random.seed()                              # (not the saved process's sequence)
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=stream.isatty())
        except (AttributeError, ValueError, OSError):
            pass
sys.argv = args
main.user_main(args[1:], exit_code=True)
