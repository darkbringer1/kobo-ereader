#!/bin/sh
set -eu
PIDFILE=/var/run/kobo-dashboard-ssh.pid
if [ ! -f "$PIDFILE" ]; then echo 'Dashboard SSH is stopped.'; exit 0; fi
PID=$(cat "$PIDFILE")
case "$PID" in ''|*[!0-9]*) echo 'Invalid PID file; no process stopped.'; exit 1 ;; esac
if [ ! -d "/proc/$PID" ]; then
    rm "$PIDFILE"
    echo 'Removed stale dashboard SSH PID file.'
    exit 0
fi
EXECUTABLE=$(readlink "/proc/$PID/exe")
case "$EXECUTABLE" in
    /mnt/onboard/.adds/dashboard/bin/dropbearmulti)
        kill "$PID"
        rm -f "$PIDFILE"
        echo 'Dashboard SSH listener stopped.'
        ;;
    *) echo 'PID belongs to another program; no process stopped.'; exit 1 ;;
esac
