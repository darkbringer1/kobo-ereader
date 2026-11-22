#!/bin/sh
# Delegates to existing verified key-only Dropbear SSH wrapper (port 2222).
set -eu
ACTION="${1:-status}"
case "$ACTION" in
    start)
        exec /bin/sh /mnt/onboard/.adds/dashboard/start-ssh.sh
        ;;
    stop)
        exec /bin/sh /mnt/onboard/.adds/dashboard/stop-ssh.sh
        ;;
    *)
        if [ -f /var/run/kobo-dashboard-ssh.pid ]; then
            PID="$(cat /var/run/kobo-dashboard-ssh.pid)"
            kill -0 "$PID" 2>/dev/null && exit 0
        fi
        exit 1
        ;;
esac
