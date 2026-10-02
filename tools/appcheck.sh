#!/bin/sh
# appcheck.sh LOG...: the compatibility checklist of an app run, from its log (the
# iOS app's "Logu kopyala" text, a <package>.log, or aoiproc's output):
#   crashes            the exception that ended the app
#   missing calls      calls of our services not written yet (aoi.Services' default answers)
#   stand-in services  services nobody wrote, answered with defaults
#   missing services   services still absent (kept out on purpose, or not Context names)
#   missing libraries  native libraries the system image lacks
# Each line is a to-do, most important first.
for f in "$@"; do
    echo "== $f"
    echo "-- crashes"
    grep -a "aoi: the app crashed\|FATAL EXCEPTION\|Caused by:" "$f" | sed 's/^[A-Z]\/[A-Za-z]*: //' | sort | uniq -c | sort -rn
    echo "-- missing calls (default answers)"
    grep -a "aoi: missing " "$f" | sed 's/.*aoi: missing //; s/ (default answer)//' | sort -u
    grep -a "AbstractMethodError" "$f" | sed 's/.*abstract method //' | sort -u
    echo "-- stand-in services"
    grep -a 'is a stand-in' "$f" | sed 's/.*service "\([^"]*\)".*/\1/' | sort -u | tr '\n' ' '; echo
    echo "-- missing services"
    grep -a "No service published for:" "$f" | sed 's/.*for: //' | sort | uniq -c | sort -rn
    echo "-- missing libraries"
    grep -a 'library "[^"]*" not found' "$f" | sed 's/.*library "\([^"]*\)".*/\1/' | sort -u
done
