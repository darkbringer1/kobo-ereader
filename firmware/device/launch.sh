#!/bin/sh
# Detached entry point invoked by NickelMenu.
# Copies runner + binary into /tmp and starts a detached session so stopping Nickel
# cannot signal or terminate KoboDash.
set -eu

BASE=/mnt/onboard/.adds/kobodash
LOCK=/tmp/kobodash.lock

if ! mkdir "$LOCK" 2>/dev/null; then
    exit 0
fi

cp -f "$BASE/runner.sh" /tmp/kobodash-runner.sh
cp -f "$BASE/bin/kobodash" /tmp/kobodash
chmod 755 /tmp/kobodash-runner.sh /tmp/kobodash

if [ -x /mnt/onboard/.adds/dashboard/bin/busybox ]; then
    /mnt/onboard/.adds/dashboard/bin/busybox setsid /bin/sh /tmp/kobodash-runner.sh </dev/null >/tmp/kobodash.log 2>&1 &
else
    nohup /bin/sh /tmp/kobodash-runner.sh </dev/null >/tmp/kobodash.log 2>&1 &
fi
exit 0
