#!/bin/sh
# A short, reversible framebuffer smoke test. Always resume Nickel on exit.
set -eu
BASE=/mnt/onboard/.adds/dashboard
export LD_LIBRARY_PATH="$BASE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$BASE/logs"
exec >>"$BASE/logs/display.txt" 2>&1
date
LOCK=/tmp/kobo-dashboard-display.lock
if ! mkdir "$LOCK" 2>/dev/null; then exit 1; fi
STOPPED=""
cleanup() {
    for pid in $STOPPED; do kill -CONT "$pid" 2>/dev/null || true; done
    rmdir "$LOCK" 2>/dev/null || true
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
test -s "$BASE/hello-dashboard.png"
test -x "$BASE/bin/fbink"
sleep 1
for pid in $(pidof nickel); do
    STOPPED="$STOPPED $pid"
    kill -STOP "$pid"
done
# Here -f requests an e-ink refresh; it does not write firmware.
"$BASE/bin/fbink" -c -f -m -M 'Hello World'
sleep 3
"$BASE/bin/fbink" -c -f -g "file=$BASE/hello-dashboard.png"
sleep 15
printf 'Text and image commands completed; resuming Nickel.\n'
