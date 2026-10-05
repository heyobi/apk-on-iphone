#!/bin/sh
# compat.sh ROOT OUT APK...: the compatibility run. Each APK is installed as the iOS app
# installs it (tools/app-install.sh), compiled with dex2oat as the app's "Compile"
# button does, then launched and left running for $RUN seconds (default 150) without
# input, through build/iostest-gpu (the phone's code path, the host's GPU). Writes
# OUT/NAME.{out,log,frame.ppm} and prints one line per app:
#   OPENS     it drew a frame and was still running at the end
#   CRASH     a Java exception ended it, it exited, or it faulted
#   NO-FRAME  it ran but never drew a frame
# then OUT/summary.txt (tools/appcheck.sh's to-do list for each). JOBS (default 2)
# apps run at a time.
set -u
ROOT=${1:?usage: compat.sh ROOT OUT APK...}
OUT=${2:?usage: compat.sh ROOT OUT APK...}
shift 2
TOP="$(cd "$(dirname "$0")/.." && pwd)"
RUN=${RUN:-150}
JOBS=${JOBS:-2}
mkdir -p "$OUT"
[ -x "$TOP/build/iostest-gpu" ] || make -C "$TOP" build/iostest-gpu >/dev/null

one() {
    apk=$1
    name=$(basename "$apk" .apk)
    dir=$OUT/$name
    sh "$TOP/tools/app-install.sh" "$ROOT" "$apk" "$dir" >/dev/null 2>&1
    AOI_ANDROID_ROOT=$ROOT AOI_APP_DATA=$dir AOI_APP_LOG=$dir.log AOI_APP_FRAME=$dir.frame \
    AOI_APP_DISPLAY="804 1556 320" AOI_APP_COMPILE=1 AOI_APP_STOP_AFTER=$RUN \
        timeout 7200 "$TOP/build/iostest-gpu" > "$dir.out" 2>&1
    frames=$(grep -c '^frame ' "$dir.out")
    crash=$(grep -a -m1 'FATAL EXCEPTION\|aoi: the app crashed' "$dir.log" 2>/dev/null)
    end=$(grep -a -m1 '^app: exit\|^app: stopped' "$dir.out")
    case "$end" in
    *"stopped (0)"*) ended=0 ;;
    "") ended=1 ;;
    *) ended=1 ;;
    esac
    if [ -n "$crash" ] || [ "$ended" = 1 ]; then st=CRASH
    elif [ "$frames" -gt 0 ]; then st=OPENS
    else st=NO-FRAME; fi
    why=$(grep -a -m1 'Caused by:\|FATAL EXCEPTION' "$dir.log" 2>/dev/null | sed 's/^[A-Z]\/[A-Za-z]*: //')
    [ -z "$why" ] && why=$end
    printf '%-9s %-40s frames %-4s %s\n' "$st" "$name" "$frames" "$why"
}

: > "$OUT/results.txt"
n=0
for apk in "$@"; do
    one "$apk" >> "$OUT/results.txt" &
    n=$((n + 1))
    if [ "$n" -ge "$JOBS" ]; then wait; n=0; fi
done
wait
sort "$OUT/results.txt"
for apk in "$@"; do sh "$TOP/tools/appcheck.sh" "$OUT/$(basename "$apk" .apk).log"; done > "$OUT/summary.txt" 2>/dev/null
echo "opens: $(grep -c '^OPENS' "$OUT/results.txt") of $(wc -l < "$OUT/results.txt")"
